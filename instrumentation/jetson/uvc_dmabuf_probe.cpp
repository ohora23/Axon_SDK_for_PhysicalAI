// SPDX-License-Identifier: Apache-2.0
// Can a UVC camera capture into NvBufSurface buffers (V4L2_MEMORY_DMABUF) when the driver
// reports a sizeimage larger than the frame? (OAK-D in UVC mode reports 24.5 MB for a
// 1.5 MB NV12 frame.) Allocates GRAY8 surfaces of W x rows, queues them, streams N frames
// and prints bytesused, the timestamp -> dequeue lag and a Y-plane sanity check.
//
//   uvc_dmabuf_probe /dev/videoN W H fourcc(NV12|YUYV) rows [frames]
//     rows = surface height in GRAY8 rows (NV12: >= H*3/2; try sizeimage/W to cover it)
// Build: instrumentation/jetson/build.sh

#include <nvbufsurface.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r;
}
double now_ms(clockid_t c) { timespec t; clock_gettime(c, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s /dev/videoN W H NV12|YUYV rows [frames]\n", argv[0]); return 2; }
    const char* dev = argv[1];
    const auto W = static_cast<std::uint32_t>(atoi(argv[2])), H = static_cast<std::uint32_t>(atoi(argv[3]));
    const std::uint32_t fourcc = strcmp(argv[4], "NV12") == 0 ? V4L2_PIX_FMT_NV12 : V4L2_PIX_FMT_YUYV;
    const auto rows = static_cast<std::uint32_t>(atoi(argv[5]));
    const int frames = argc > 6 ? atoi(argv[6]) : 90;
    constexpr int kBuf = 4;

    // GRAY8 surfaces: one byte per pixel, so the pitch is the row stride for NV12's Y plane
    // and the UV plane follows contiguously only if pitch == W (checked below).
    NvBufSurfaceAllocateParams p{};
    p.params.width = fourcc == V4L2_PIX_FMT_YUYV ? W * 2 : W;
    p.params.height = rows;
    p.params.layout = NVBUF_LAYOUT_PITCH;
    p.params.colorFormat = NVBUF_COLOR_FORMAT_GRAY8;
    p.params.memType = NVBUF_MEM_SURFACE_ARRAY;
    p.memtag = NvBufSurfaceTag_CAMERA;
    std::vector<NvBufSurface*> s(kBuf);
    for (auto& x : s)
        if (NvBufSurfaceAllocate(&x, 1, &p) != 0) { fprintf(stderr, "NvBufSurfaceAllocate failed\n"); return 1; }
    const std::uint32_t pitch = s[0]->surfaceList[0].pitch;
    const std::uint32_t bsize = s[0]->surfaceList[0].dataSize;
    printf("surface: %ux%u GRAY8 pitch=%u dataSize=%u\n", p.params.width, rows, pitch, bsize);

    int v = open(dev, O_RDWR | O_NONBLOCK);
    if (v < 0) { perror(dev); return 1; }
    v4l2_format f{};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = W;
    f.fmt.pix.height = H;
    f.fmt.pix.pixelformat = fourcc;
    f.fmt.pix.bytesperline = pitch;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(v, VIDIOC_S_FMT, &f) < 0) { perror("S_FMT"); return 1; }
    printf("S_FMT -> %ux%u bytesperline=%u sizeimage=%u (surface %u B, %s)\n", f.fmt.pix.width,
           f.fmt.pix.height, f.fmt.pix.bytesperline, f.fmt.pix.sizeimage, bsize,
           bsize >= f.fmt.pix.sizeimage ? "covers it" : "SMALLER");
    if (f.fmt.pix.bytesperline != pitch)
        printf("note: driver pitch %u != surface pitch %u (layout would not match)\n", f.fmt.pix.bytesperline, pitch);

    v4l2_requestbuffers rb{};
    rb.count = kBuf;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(v, VIDIOC_REQBUFS, &rb) < 0) { perror("REQBUFS(DMABUF)"); return 1; }
    for (int i = 0; i < kBuf; ++i) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_DMABUF;
        b.index = static_cast<std::uint32_t>(i);
        b.m.fd = static_cast<int>(s[static_cast<std::size_t>(i)]->surfaceList[0].bufferDesc);
        b.length = bsize;
        if (xioctl(v, VIDIOC_QBUF, &b) < 0) {
            printf("QBUF(DMABUF, length=%u) FAILED: %s -> buffer smaller than sizeimage is rejected\n", bsize, strerror(errno));
            return 3;
        }
    }
    printf("QBUF x%d accepted\n", kBuf);
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(v, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }

    std::vector<double> lag, gap;
    double prev_ts = 0;
    std::uint32_t used_min = ~0u, used_max = 0;
    int got = 0, bad = 0;
    while (got < frames) {
        pollfd pf{v, POLLIN, 0};
        if (poll(&pf, 1, 3000) <= 0) { printf("no frame within 3 s after %d frames\n", got); break; }
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_DMABUF;
        if (xioctl(v, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }
        const double t_dq = now_ms(CLOCK_MONOTONIC);
        const double ts = b.timestamp.tv_sec * 1e3 + b.timestamp.tv_usec / 1e3;
        if (b.flags & V4L2_BUF_FLAG_ERROR) ++bad;
        used_min = std::min(used_min, b.bytesused);
        used_max = std::max(used_max, b.bytesused);
        if (got >= 10) { lag.push_back(t_dq - ts); if (prev_ts > 0) gap.push_back(ts - prev_ts); }
        prev_ts = ts;
        if (got == frames - 1) {   // Y plane must not be constant (proves the camera wrote it)
            NvBufSurface* x = s[b.index];
            NvBufSurfaceMap(x, 0, 0, NVBUF_MAP_READ);
            NvBufSurfaceSyncForCpu(x, 0, 0);
            const auto* y = static_cast<const std::uint8_t*>(x->surfaceList[0].mappedAddr.addr[0]);
            std::uint32_t lo = 255, hi = 0;
            for (std::uint32_t r = 0; r < H; r += 8)
                for (std::uint32_t c = 0; c < W; c += 8) { lo = std::min<std::uint32_t>(lo, y[r * pitch + c]); hi = std::max<std::uint32_t>(hi, y[r * pitch + c]); }
            printf("last frame Y range %u..%u\n", lo, hi);
            NvBufSurfaceUnMap(x, 0, 0);
        }
        ++got;
        if (xioctl(v, VIDIOC_QBUF, &b) < 0) { perror("re-QBUF"); break; }
    }
    xioctl(v, VIDIOC_STREAMOFF, &type);
    close(v);
    if (!lag.empty()) {
        std::sort(lag.begin(), lag.end());
        std::sort(gap.begin(), gap.end());
        printf("frames=%d error_flag=%d bytesused=%u..%u\n", got, bad, used_min, used_max);
        printf("timestamp->DQBUF ms: min %.2f p50 %.2f p90 %.2f max %.2f | interval p50 %.2f\n", lag.front(),
               lag[lag.size() / 2], lag[lag.size() * 9 / 10], lag.back(), gap.empty() ? 0.0 : gap[gap.size() / 2]);
    }
    for (auto* x : s) NvBufSurfaceDestroy(x);
    return got >= frames ? 0 : 1;
}
