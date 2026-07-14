# SPDX-License-Identifier: Apache-2.0
# A-1: device (GPU) pool buffers exposed to Python via the CUDA Array Interface.
#
# Framework-free by default: verifies the __cuda_array_interface__ dict is
# well-formed and aliases the pool's real device pointer. If CuPy is importable,
# it additionally does a zero-copy write/read round-trip on the GPU.
#
# Needs the module built with -DAXON_BUILD_PYTHON=ON -DAXON_WITH_CUDA=ON and a
# CUDA device; otherwise it SKIPs cleanly (exit 0).

import sys

import axon

N = 1024
BYTES = N * 4  # one f32 buffer


def main() -> int:
    try:
        pool = axon.TensorPool.create(
            n_buffers=4, buffer_size=BYTES,
            backend=axon.PoolBackend.Accelerator)
    except Exception as e:  # no CUDA device / not built with AXON_WITH_CUDA
        print("SKIP: Accelerator pool unavailable:", e)
        return 0

    ptr = pool.device_ptr(0)
    assert ptr != 0, "device_ptr(0) is null on an Accelerator pool"

    dev = pool.device_array(0, shape=[N], dtype=axon.DType.F32)
    cai = dev.__cuda_array_interface__
    assert cai["version"] == 3, cai
    assert cai["shape"] == (N,), cai["shape"]
    assert cai["typestr"] == "<f4", cai["typestr"]
    data_ptr, read_only = cai["data"]
    assert data_ptr == ptr, (data_ptr, ptr)      # CAI aliases the real buffer
    assert read_only is False
    assert cai["strides"] is None                # C-contiguous

    # read_only flag propagates into the interface.
    dev_ro = pool.device_array(0, shape=[N], dtype=axon.DType.F32, read_only=True)
    assert dev_ro.__cuda_array_interface__["data"][1] is True

    # Optional: prove it is real, framework-usable device memory.
    try:
        import cupy as cp
    except Exception:
        print("PASS: CAI plumbing verified (CuPy absent; GPU round-trip skipped)")
        return 0

    a = cp.asarray(dev)                          # zero-copy alias of buffer 0
    a[:] = cp.arange(N, dtype=cp.float32)
    b = cp.asarray(pool.device_array(0, shape=[N], dtype=axon.DType.F32))
    assert bool((a == b).all()) and int(b[7]) == 7   # same physical buffer
    print("PASS: CAI verified + CuPy zero-copy round-trip on device")
    return 0


if __name__ == "__main__":
    sys.exit(main())
