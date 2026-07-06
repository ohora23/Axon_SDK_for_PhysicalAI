// SPDX-License-Identifier: Apache-2.0
// axon depth-filter demo — "depth sensor -> CPU filter -> zero-copy publish".
//
// Exercises the v2 imaging/depth metadata (row_pitch, depth_scale, units,
// invalid_value, intrinsics) and the R4 validation path, with no camera:
//
//   producer (parent): synthesizes a U16 depth frame WITH row padding
//     (row_pitch > width*2) and holes (invalid_value = 0), runs a CPU hole-fill
//     filter writing the result straight into the pooled dma-buf (host_view),
//     stamps the v2 descriptor, and publishes.
//   consumer (child): reads the filtered depth zero-copy, indexing padded rows
//     via v.row_pitch, and checks: (a) zero-copy content (seqno stamped at [0,0]),
//     (b) the filter worked (no invalid pixels remain), (c) all v2 metadata
//     round-trips (pitch, depth_scale, units, intrinsics).
//
// The filter's own write is inherent; the cross-process hop is zero-copy.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <ctime>
#include <sys/wait.h>
#include <unistd.h>

#include "axon/pool.h"
#include "axon/publisher.h"
#include "axon/subscriber.h"
#include "axon/rt.h"
#include "axon/detail/descriptor_util.h"

using namespace axon;

namespace {

constexpr uint32_t W = 316;         // odd width so W*2 (632) is not 64-aligned
constexpr uint32_t H = 240;
constexpr uint32_t ROW_PITCH = 640; // padded row stride (632 -> 640), exercises pitch
constexpr uint64_t IMG_BYTES = uint64_t(ROW_PITCH) * H;   // 153600
constexpr uint16_t INVALID = 0;     // no-data sentinel
constexpr float DEPTH_SCALE = 0.001f;   // U16 value is millimetres -> metres
constexpr uint64_t FINAL_SEQNO = 60;
const char* kService = "depth/filtered";

void sleep_ms(int ms) { struct timespec t{ms/1000,(ms%1000)*1000000L}; nanosleep(&t,nullptr); }

// A pixel that is a "hole" every 7th sample (except [0,0], reserved for seqno).
bool is_hole(uint32_t r, uint32_t c) { return (r || c) && ((r * W + c) % 7 == 0); }
uint16_t base_depth(uint32_t r, uint32_t c) { return uint16_t(1000 + (r + c) * 10); }

int run_consumer() {
    auto sub = TensorSubscriber::create(kService);
    if (!sub || sub->wait_handshake(5000) != 0) return 11;

    for (int i = 0; i < 4000; ++i) {
        auto v = sub->latest_view(8);
        if (v && v->seqno >= FINAL_SEQNO) {
            if (!v->data) return 12;
            const auto* rows = static_cast<const uint8_t*>(v->data);

            // (a) zero-copy content: seqno stamped at pixel [0,0].
            uint16_t p00 = *reinterpret_cast<const uint16_t*>(rows);
            if (p00 != uint16_t(v->seqno & 0xffff)) { std::fprintf(stderr,"content mismatch\n"); return 13; }

            // (b) filter worked: no INVALID pixels remain (skip [0,0]).
            uint64_t holes = 0;
            for (uint32_t r = 0; r < H; ++r) {
                const uint16_t* row = reinterpret_cast<const uint16_t*>(rows + uint64_t(r) * v->row_pitch);
                for (uint32_t c = 0; c < W; ++c)
                    if (!(r == 0 && c == 0) && row[c] == INVALID) ++holes;
            }
            if (holes) { std::fprintf(stderr,"hole-fill failed: %lu invalid left\n",(unsigned long)holes); return 14; }

            // (c) v2 metadata round-trip.
            bool meta_ok =
                v->row_pitch == ROW_PITCH &&
                v->depth_scale == DEPTH_SCALE &&
                v->invalid_value == INVALID &&
                v->sample_units == SampleUnits::Millimeters &&
                v->capture_clock == CaptureClock::MonotonicRaw &&
                v->layout == TensorLayout::HW &&
                v->intr_fx == 600.0f && v->intr_cx == W / 2.0f &&
                v->intr_ref_width == W && v->intr_ref_height == H;

            std::printf("\n────── axon depth-filter demo (consumer) ──────\n");
            std::printf("  frame seqno:       %lu  (%ux%u U16 depth, pitch=%u)\n",
                        (unsigned long)v->seqno, W, H, v->row_pitch);
            std::printf("  zero-copy content: OK (seqno read back from dma-buf)\n");
            std::printf("  hole-fill:         OK (0 invalid pixels remain)\n");
            std::printf("  v2 metadata:       %s (scale=%.4f units=mm fx=%.0f cx=%.1f)\n",
                        meta_ok ? "OK" : "MISMATCH",
                        v->depth_scale, v->intr_fx, v->intr_cx);
            std::printf("  padded rows read via row_pitch (%u > packed %u)\n", v->row_pitch, W * 2);
            std::printf("  staleness:         %.1f us\n",
                        static_cast<double>(v->staleness_ns) / 1e3);
            std::printf("───────────────────────────────────────────────\n");
            std::fflush(stdout);
            return meta_ok ? 0 : 15;
        }
        sleep_ms(1);
    }
    return 16;
}

int run_producer(TensorPublisher& pub, pid_t child) {
    std::vector<uint16_t> row(W);
    for (uint64_t s = 1; s <= FINAL_SEQNO; ++s) {
        AcquiredDescriptor a = pub.acquire_descriptor();
        if (a.buffer_index < 0 || !a.host_view) return 20;
        auto* dst = static_cast<uint8_t*>(a.host_view);

        for (uint32_t r = 0; r < H; ++r) {
            // Build the input row (gradient + holes), then hole-fill left->right.
            uint16_t last_valid = base_depth(r, 0);
            for (uint32_t c = 0; c < W; ++c) {
                uint16_t val = is_hole(r, c) ? INVALID : base_depth(r, c);
                if (val == INVALID) val = last_valid;   // fill hole with last valid
                else last_valid = val;
                row[c] = val;
            }
            std::memcpy(dst + uint64_t(r) * ROW_PITCH, row.data(), W * sizeof(uint16_t));
        }
        // Stamp seqno into pixel [0,0] for the zero-copy content check.
        *reinterpret_cast<uint16_t*>(dst) = uint16_t(s & 0xffff);

        auto* d = a.desc;
        d->rank = 2;
        d->shape[0] = H; d->shape[1] = W;
        d->dtype = DType::U16;
        d->offset = 0;
        d->size = IMG_BYTES;
        d->row_pitch = ROW_PITCH;
        d->depth_scale = DEPTH_SCALE;
        d->invalid_value = INVALID;
        d->sample_units = SampleUnits::Millimeters;
        d->capture_clock = CaptureClock::MonotonicRaw;
        d->layout = TensorLayout::HW;
        d->intr_fx = 600.0f; d->intr_fy = 600.0f;
        d->intr_cx = W / 2.0f; d->intr_cy = H / 2.0f;
        d->intr_ref_width = W; d->intr_ref_height = H;
        d->capture_ts_ns = rt_now_ns();

        pub.publish(std::move(a));
        sleep_ms(2);
    }
    int status = 0; waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 99;
}

}  // namespace

int main() {
    // Pool buffer must hold one padded frame.
    auto pool = TensorPool::create(
        TensorPoolConfig{8, static_cast<size_t>(IMG_BYTES), PoolBackend::Custom, nullptr});
    if (!pool) { std::fprintf(stderr, "pool create failed\n"); return 1; }
    auto pub = TensorPublisher::create(kService, *pool);
    if (!pub) { std::fprintf(stderr, "publisher create failed\n"); return 1; }

    std::fprintf(stderr, "depth-filter demo: %ux%u U16, pitch=%u (pad), hole-fill, %lu frames\n",
                 W, H, ROW_PITCH, (unsigned long)FINAL_SEQNO);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) _exit(run_consumer());

    pub->handshake_pool();
    return run_producer(*pub, pid) == 0 ? 0 : 1;
}
