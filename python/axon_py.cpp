// SPDX-License-Identifier: Apache-2.0
// axon — Python bindings (pybind11), design doc §7.2 (Phase 1).
//
// Exposes the public C++ API to Python with a NumPy-native surface:
//   - producers publish a NumPy array (written into the pooled dma-buf)
//   - consumers get a zero-copy, read-only NumPy view backed directly by the
//     mmap'd dma-buf — no copy on the read path
//
// Build: -DAXON_BUILD_PYTHON=ON  (import name: `axon`).

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "axon/pool.h"
#include "axon/publisher.h"
#include "axon/rt.h"
#include "axon/subscriber.h"
#include "axon/types.h"

namespace py = pybind11;
using namespace axon;

namespace {

// Map a axon DType onto a NumPy dtype. BF16 has no native NumPy type, so it is
// surfaced as raw uint16 (bit-preserving) — callers reinterpret as needed.
py::dtype numpy_dtype(DType t) {
    switch (t) {
        case DType::U8:   return py::dtype("u1");
        case DType::U16:  return py::dtype("u2");
        case DType::I16:  return py::dtype("i2");
        case DType::F16:  return py::dtype("f2");
        case DType::BF16: return py::dtype("u2");  // no native bf16
        case DType::F32:  return py::dtype("f4");
        case DType::F64:  return py::dtype("f8");
        case DType::I32:  return py::dtype("i4");
        case DType::I64:  return py::dtype("i8");
    }
    return py::dtype("u1");
}

// __cuda_array_interface__ type strings (little-endian). BF16 has no CAI code,
// so it is surfaced as raw uint16 (bit-preserving), mirroring numpy_dtype.
std::string cai_typestr(DType t) {
    switch (t) {
        case DType::U8:   return "|u1";
        case DType::U16:  return "<u2";
        case DType::I16:  return "<i2";
        case DType::F16:  return "<f2";
        case DType::BF16: return "<u2";
        case DType::F32:  return "<f4";
        case DType::F64:  return "<f8";
        case DType::I32:  return "<i4";
        case DType::I64:  return "<i8";
    }
    return "|u1";
}

// A zero-copy handle to a device (GPU) pool buffer, exposed to Python via the
// CUDA Array Interface so CuPy / PyTorch / Numba can wrap it without a copy.
// The buffer is owned by the TensorPool; this is a borrowed view.
struct PyDeviceArray {
    std::uintptr_t ptr;             // CUdeviceptr as an integer
    std::vector<py::ssize_t> shape;
    DType dtype;
    bool read_only;
};

// A Python-facing snapshot of a consumed frame.
struct PyView {
    SeqNo         seqno;
    std::uint64_t staleness_ns;
    int           seqlock_retries;
    DType         dtype;
    std::vector<std::uint32_t> shape;
    py::array     data;         // zero-copy host view (empty for a device frame)
    std::uintptr_t device_ptr = 0;  // consumer device pointer (0 for host frames)
};

}  // namespace

PYBIND11_MODULE(axon, m) {
    m.doc() = "axon — data-centric zero-copy for Physical AI (Python bindings)";

    py::enum_<DType>(m, "DType")
        .value("U8", DType::U8).value("U16", DType::U16).value("I16", DType::I16)
        .value("F16", DType::F16).value("BF16", DType::BF16).value("F32", DType::F32)
        .value("F64", DType::F64).value("I32", DType::I32).value("I64", DType::I64);

    py::enum_<PoolBackend>(m, "PoolBackend")
        .value("V4L2", PoolBackend::V4L2)
        .value("UDMABUF", PoolBackend::UDMABUF)
        .value("Custom", PoolBackend::Custom)
        .value("Accelerator", PoolBackend::Accelerator);

    py::enum_<FallbackPolicy>(m, "FallbackPolicy")
        .value("LastKnownGood", FallbackPolicy::LastKnownGood)
        .value("ZeroCommand", FallbackPolicy::ZeroCommand)
        .value("UserCallback", FallbackPolicy::UserCallback)
        .value("AbortLoop", FallbackPolicy::AbortLoop);

    py::enum_<SyncFenceKind>(m, "SyncFenceKind")
        .value("None_", SyncFenceKind::None)
        .value("DmaResvImplicit", SyncFenceKind::DmaResvImplicit)
        .value("SyncFileViaSidecar", SyncFenceKind::SyncFileViaSidecar);

    // ---- TensorPool ----
    py::class_<TensorPool>(m, "TensorPool")
        .def_static("create",
            [](std::size_t n_buffers, std::size_t buffer_size,
               PoolBackend backend, const std::string& v4l2_device) {
                TensorPoolConfig cfg{};
                cfg.n_buffers = n_buffers;
                cfg.buffer_size = buffer_size;
                cfg.backend = backend;
                cfg.v4l2_device = v4l2_device.empty() ? nullptr : v4l2_device.c_str();
                auto p = TensorPool::create(cfg);
                if (!p) throw std::runtime_error("TensorPool::create failed");
                return p;
            },
            py::arg("n_buffers"), py::arg("buffer_size"),
            py::arg("backend") = PoolBackend::Custom,
            py::arg("v4l2_device") = std::string(),
            "Allocate a dma-buf-backed buffer pool.")
        .def("generation", &TensorPool::generation)
        .def("buffer_count",
             [](const TensorPool& p) { return p.dma_buf_fds().size(); })
        .def("device_ptr",
             [](const TensorPool& p, std::size_t index) {
                 return reinterpret_cast<std::uintptr_t>(p.device_ptr(index));
             },
             py::arg("index"),
             "Raw device pointer (CUdeviceptr as int) for an Accelerator-backed "
             "buffer; 0 for host backends or an out-of-range index.")
        .def("device_array",
             [](TensorPool& p, std::size_t index, std::vector<py::ssize_t> shape,
                DType dtype, bool read_only) {
                 void* dp = p.device_ptr(index);
                 if (!dp)
                     throw std::runtime_error(
                         "device_ptr is null — the pool is not Accelerator-backed "
                         "(build -DAXON_WITH_CUDA=ON and use PoolBackend.Accelerator)");
                 PyDeviceArray d{};
                 d.ptr = reinterpret_cast<std::uintptr_t>(dp);
                 d.shape = std::move(shape);
                 d.dtype = dtype;
                 d.read_only = read_only;
                 return d;
             },
             py::arg("index"), py::arg("shape"), py::arg("dtype") = DType::U8,
             py::arg("read_only") = false, py::keep_alive<0, 1>(),
             "Wrap a device pool buffer as a __cuda_array_interface__ object so a "
             "framework (CuPy/PyTorch/Numba) can alias it zero-copy.")
        .def("retire_and_reallocate", &TensorPool::retire_and_reallocate,
             py::arg("new_buffer_size"));

    // ---- Device array (CUDA Array Interface) ----
    py::class_<PyDeviceArray>(m, "DeviceArray")
        .def_property_readonly("__cuda_array_interface__",
            [](const PyDeviceArray& d) {
                py::tuple shape(d.shape.size());
                for (std::size_t i = 0; i < d.shape.size(); ++i)
                    shape[i] = d.shape[i];
                py::dict cai;
                cai["shape"] = shape;
                cai["typestr"] = cai_typestr(d.dtype);
                cai["data"] = py::make_tuple(py::int_(d.ptr), d.read_only);
                cai["strides"] = py::none();   // C-contiguous
                cai["version"] = 3;
                return cai;
            },
            "CUDA Array Interface v3 aliasing the device pool buffer.");

    // ---- TensorPublisher ----
    py::class_<TensorPublisher>(m, "TensorPublisher")
        .def_static("create",
            [](const std::string& service_name, TensorPool& pool) {
                auto p = TensorPublisher::create(service_name, pool);
                if (!p) throw std::runtime_error("TensorPublisher::create failed");
                return p;
            },
            py::arg("service_name"), py::arg("pool"),
            // Keep the pool alive at least as long as the publisher.
            py::keep_alive<0, 2>())
        .def("handshake_pool", &TensorPublisher::handshake_pool,
             "Bulk-deliver every pool FD to connected consumers. Returns the "
             "number of consumers reached.")
        .def("publish",
            [](TensorPublisher& pub, py::array arr, DType dtype) {
                // Acquire a pooled buffer, write the array bytes into its dma-buf
                // host view, stamp the descriptor, and publish.
                AcquiredDescriptor a = pub.acquire_descriptor();
                if (a.buffer_index < 0 || a.host_view == nullptr)
                    throw std::runtime_error("no writable pool buffer available");

                py::buffer_info info = arr.request();
                std::size_t nbytes = static_cast<std::size_t>(info.size) *
                                     static_cast<std::size_t>(info.itemsize);
                if (nbytes > a.accel_handle.size_bytes)
                    throw std::runtime_error("array larger than pool buffer");
                // NumPy may hand us a non-contiguous array; request() on a plain
                // ndarray is contiguous, but guard anyway.
                std::memcpy(a.host_view, info.ptr, nbytes);

                a.desc->rank = static_cast<std::uint8_t>(
                    info.ndim > kMaxRank ? kMaxRank : info.ndim);
                for (std::uint8_t i = 0; i < a.desc->rank; ++i)
                    a.desc->shape[i] = static_cast<std::uint32_t>(info.shape[i]);
                a.desc->dtype = dtype;
                a.desc->offset = 0;
                a.desc->size = nbytes;
                a.desc->sync_fence_kind = SyncFenceKind::None;

                pub.publish(std::move(a));
            },
            py::arg("array"), py::arg("dtype") = DType::U8,
            "Publish a NumPy array as the next frame.")
        .def("acquire",
            [](TensorPublisher& pub) { return pub.acquire_descriptor(); },
            "Acquire the next pool buffer. Use acquired.buffer_index to select "
            "the device_array (or host buffer) to fill, then publish_device(). "
            "One frame in flight: do not acquire again before publishing.")
        .def("publish_device",
            [](TensorPublisher& pub, AcquiredDescriptor& a,
               std::vector<std::uint32_t> shape, DType dtype) {
                if (a.buffer_index < 0)
                    throw std::runtime_error("no writable pool buffer available");
                std::uint64_t count = 1;
                a.desc->rank = static_cast<std::uint8_t>(
                    shape.size() > kMaxRank ? kMaxRank : shape.size());
                for (std::uint8_t i = 0; i < a.desc->rank; ++i) {
                    a.desc->shape[i] = shape[i];
                    count *= shape[i];
                }
                a.desc->dtype = dtype;
                a.desc->offset = 0;
                a.desc->size = count * dtype_size(dtype);
                a.desc->sync_fence_kind = SyncFenceKind::None;
                pub.publish(std::move(a));
            },
            py::arg("acquired"), py::arg("shape"), py::arg("dtype"),
            "Publish a device frame already written into "
            "pool.device_array(acquired.buffer_index). No host copy.")
        .def("reannounce_pool", &TensorPublisher::reannounce_pool);

    // ---- AcquiredDescriptor (returned by acquire) ----
    py::class_<AcquiredDescriptor>(m, "AcquiredDescriptor")
        .def_readonly("buffer_index", &AcquiredDescriptor::buffer_index,
                      "Pool ring index of the acquired buffer.");

    // ---- View (returned by latest_view) ----
    py::class_<PyView>(m, "TensorView")
        .def_readonly("seqno", &PyView::seqno)
        .def_readonly("staleness_ns", &PyView::staleness_ns)
        .def_readonly("seqlock_retries", &PyView::seqlock_retries)
        .def_readonly("dtype", &PyView::dtype)
        .def_readonly("shape", &PyView::shape)
        .def_readonly("data", &PyView::data,
                      "Zero-copy, read-only NumPy view over the dma-buf (empty "
                      "for a device-backed frame — use __cuda_array_interface__).")
        .def_readonly("device_ptr", &PyView::device_ptr,
                      "Consumer device pointer (int) for an Accelerator frame; "
                      "0 for a host frame.")
        .def_property_readonly("__cuda_array_interface__",
            [](const PyView& pv) -> py::object {
                if (pv.device_ptr == 0) return py::none();  // host frame
                py::tuple shape(pv.shape.size());
                for (std::size_t i = 0; i < pv.shape.size(); ++i)
                    shape[i] = static_cast<py::ssize_t>(pv.shape[i]);
                py::dict cai;
                cai["shape"] = shape;
                cai["typestr"] = cai_typestr(pv.dtype);
                cai["data"] = py::make_tuple(py::int_(pv.device_ptr), true);  // read-only
                cai["strides"] = py::none();
                cai["version"] = 3;
                return cai;
            },
            "CUDA Array Interface v3 over the consumer device buffer, or None "
            "for a host frame.");

    // ---- TensorSubscriber ----
    py::class_<TensorSubscriber>(m, "TensorSubscriber")
        .def_static("create",
            [](const std::string& service_name) {
                auto s = TensorSubscriber::create(service_name);
                if (!s) throw std::runtime_error("TensorSubscriber::create failed");
                return s;
            },
            py::arg("service_name"))
        .def("wait_handshake", &TensorSubscriber::wait_handshake,
             py::arg("timeout_ms") = 5000,
             "Connect to the sidecar and receive the pool FDs (non-RT).")
        .def("set_fallback_policy", &TensorSubscriber::set_fallback_policy,
             py::arg("policy"))
        .def("pool_handshake_count", &TensorSubscriber::pool_handshake_count)
        .def("fallback_invocation_count", &TensorSubscriber::fallback_invocation_count)
        .def("latest_view",
            [](py::object self, int max_retry) -> py::object {
                auto& sub = self.cast<TensorSubscriber&>();
                std::optional<TensorView> v = sub.latest_view(max_retry);
                if (!v) return py::none();

                PyView pv{};
                pv.seqno = v->seqno;
                pv.staleness_ns = v->staleness_ns;
                pv.seqlock_retries = v->seqlock_retries;
                pv.dtype = v->dtype;
                pv.device_ptr = reinterpret_cast<std::uintptr_t>(v->device_ptr);

                std::vector<py::ssize_t> np_shape;
                std::uint64_t count = 1;
                for (std::uint8_t i = 0; i < v->shape.rank && i < kMaxRank; ++i) {
                    pv.shape.push_back(v->shape.dims[i]);
                    np_shape.push_back(static_cast<py::ssize_t>(v->shape.dims[i]));
                    count *= v->shape.dims[i];
                }

                if (v->data && count > 0) {
                    // Zero-copy: NumPy array points straight at the mmap region.
                    // `self` (the subscriber) is the base object, so the mapping
                    // outlives the array.
                    pv.data = py::array(numpy_dtype(v->dtype), np_shape,
                                        v->data, self);
                } else {
                    pv.data = py::array(numpy_dtype(v->dtype),
                                        std::vector<py::ssize_t>{0});
                }
                return py::cast(pv);
            },
            py::arg("max_retry") = 8,
            "RT-side seqlock read. Returns a TensorView or None (fallback).");

    // ---- RT helpers ----
    m.def("rt_now_ns", &rt_now_ns, "CLOCK_MONOTONIC_RAW nanoseconds (RT-safe).");
}
