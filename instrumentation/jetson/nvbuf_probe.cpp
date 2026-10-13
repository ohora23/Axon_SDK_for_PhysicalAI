// SPDX-License-Identifier: Apache-2.0
// axon — Jetson NvBufSurface feasibility probe (JetPack 5, L4T r35).
//
// Answers the three questions the Jetson accelerator backend rests on:
//   P1  Does a UVC camera capture straight INTO an NvBufSurface (V4L2_MEMORY_DMABUF)?
//   P2  Does an NvBufSurface dma-buf FD survive SCM_RIGHTS and resolve in another
//       process? NvBufSurfaceFromFd only knows FDs allocated in-process, so the
//       producer also ships NvBufSurfaceGetMapParams (a POD) and the consumer
//       calls NvBufSurfaceImport with the received FD patched in.
//   P3  Can CUDA in that other process address the same memory via EGLImage, with
//       no copy (device hash == CPU hash, and a GPU write is seen by the producer)?
//
// Fork happens BEFORE any CUDA/EGL init (CUDA + fork is unsafe once a context exists).
//
// Build:  instrumentation/jetson/build.sh      Run:  ./nvbuf_probe /dev/video0

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <csignal>

#include <cuda.h>
#include <cudaEGL.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nvbufsurface.h"

namespace {

constexpr int kW = 640, kH = 480, kBufs = 4, kFrames = 30;
constexpr uint8_t kMark = 0xAB;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); return 1; } } while (0)

struct Msg { int index; uint32_t seq; uint64_t cpu_hash; };

uint64_t fnv(const uint8_t* p, uint32_t pitch, uint32_t row_bytes, uint32_t rows) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t r = 0; r < rows; ++r)
        for (uint32_t c = 0; c < row_bytes; ++c) h = (h ^ p[r * pitch + c]) * 1099511628211ull;
    return h;
}

int send_fds(int sock, const int* fds, int n) {
    char dummy = 'F';
    iovec iov{&dummy, 1};
    std::vector<char> ctrl(CMSG_SPACE(sizeof(int) * n));
    msghdr m{};
    m.msg_iov = &iov; m.msg_iovlen = 1;
    m.msg_control = ctrl.data(); m.msg_controllen = ctrl.size();
    cmsghdr* c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int) * n);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * n);
    return sendmsg(sock, &m, 0) == 1 ? 0 : -1;
}

int recv_fds(int sock, int* fds, int n) {
    char dummy;
    iovec iov{&dummy, 1};
    std::vector<char> ctrl(CMSG_SPACE(sizeof(int) * n));
    msghdr m{};
    m.msg_iov = &iov; m.msg_iovlen = 1;
    m.msg_control = ctrl.data(); m.msg_controllen = ctrl.size();
    if (recvmsg(sock, &m, 0) != 1) return -1;
    cmsghdr* c = CMSG_FIRSTHDR(&m);
    if (!c || c->cmsg_type != SCM_RIGHTS) return -1;
    memcpy(fds, CMSG_DATA(c), sizeof(int) * n);
    return 0;
}

// CPU view hash of plane 0 of a surface (maps, syncs for CPU, hashes, unmaps).
int cpu_hash(NvBufSurface* s, int i, uint64_t* out, uint8_t* first_byte = nullptr) {
    if (NvBufSurfaceMap(s, i, 0, NVBUF_MAP_READ) != 0) return -1;
    if (NvBufSurfaceSyncForCpu(s, i, 0) != 0) { NvBufSurfaceUnMap(s, i, 0); return -1; }
    auto& p = s->surfaceList[i];
    auto* base = static_cast<uint8_t*>(p.mappedAddr.addr[0]);
    *out = fnv(base, p.pitch, kW * 2, kH);
    if (first_byte) *first_byte = base[0];
    NvBufSurfaceUnMap(s, i, 0);
    return 0;
}

// ---------------- consumer (child): P2 + P3 ----------------
int consumer(int sock) {
    int fds[kBufs];
    CHECK(recv_fds(sock, fds, kBufs) == 0, "recv_fds");
    NvBufSurfaceMapParams mp[kBufs];
    CHECK(read(sock, mp, sizeof mp) == sizeof mp, "recv map params");
    NvBufSurface* surf[kBufs];
    for (int i = 0; i < kBufs; ++i) {
        mp[i].fd = static_cast<uint64_t>(fds[i]);   // producer's FD number is meaningless here
        CHECK(NvBufSurfaceImport(&surf[i], &mp[i]) == 0, "NvBufSurfaceImport(%d)", fds[i]);
        surf[i]->numFilled = 1;   // Import leaves it 0, and SyncForCpu rejects index 0 then
    }
    printf("[P2] consumer imported %d received FDs via NvBufSurfaceImport  (pitch=%u memType=%d)\n",
           kBufs, surf[0]->surfaceList[0].pitch, surf[0]->memType);

    CUdevice dev; CUcontext ctx;
    CHECK(cuInit(0) == CUDA_SUCCESS && cuDeviceGet(&dev, 0) == CUDA_SUCCESS &&
          cuCtxCreate(&ctx, 0, dev) == CUDA_SUCCESS, "CUDA init");

    // Register every buffer once (this is what a backend import would cache).
    CUgraphicsResource res[kBufs];
    uint8_t* dptr[kBufs];
    uint32_t pitch[kBufs];
    for (int i = 0; i < kBufs; ++i) {
        CHECK(NvBufSurfaceMapEglImage(surf[i], 0) == 0, "NvBufSurfaceMapEglImage(%d)", i);
        CHECK(cuGraphicsEGLRegisterImage(&res[i], surf[i]->surfaceList[0].mappedAddr.eglImage,
                                         CU_GRAPHICS_MAP_RESOURCE_FLAGS_NONE) == CUDA_SUCCESS,
              "cuGraphicsEGLRegisterImage(%d)", i);
        CUeglFrame f;
        CHECK(cuGraphicsResourceGetMappedEglFrame(&f, res[i], 0, 0) == CUDA_SUCCESS, "GetMappedEglFrame(%d)", i);
        CHECK(f.frameType == CU_EGL_FRAME_TYPE_PITCH, "buffer %d not pitch-linear", i);
        dptr[i] = static_cast<uint8_t*>(f.frame.pPitch[0]);
        pitch[i] = f.pitch;
    }
    printf("[P3] CUDA mapped %d buffers via EGLImage (dev ptr[0]=%p pitch=%u)\n", kBufs, dptr[0], pitch[0]);

    // P4: plain mmap() of the raw dma-buf FD (what axon's existing host path does),
    // no NvBufSurfaceMap / SyncForCpu. Coherent enough to reuse that path as-is?
    uint8_t* raw[kBufs];
    for (int i = 0; i < kBufs; ++i) {
        void* q = mmap(nullptr, surf[i]->surfaceList[0].dataSize, PROT_READ, MAP_SHARED, fds[i], 0);
        raw[i] = q == MAP_FAILED ? nullptr : static_cast<uint8_t*>(q);
    }
    printf("[P4] plain mmap of received FD: %s\n", raw[0] ? "ok" : strerror(errno));
    int raw_matched = 0;

    std::vector<uint8_t> host(static_cast<size_t>(pitch[0]) * kH);
    int matched = 0, total = 0;
    Msg m;
    while (read(sock, &m, sizeof m) == sizeof m && m.index >= 0) {
        int i = m.index;
        // Device-side read of the same physical buffer. The DtoH copy exists only to
        // hash on the host for verification — the payload path itself is zero-copy.
        CHECK(cuMemcpyDtoH(host.data(), reinterpret_cast<CUdeviceptr>(dptr[i]), host.size()) == CUDA_SUCCESS, "DtoH");
        uint64_t dh = fnv(host.data(), pitch[i], kW * 2, kH);
        uint64_t ch = 0;
        CHECK(cpu_hash(surf[i], 0, &ch) == 0, "consumer cpu map");
        if (raw[i] && fnv(raw[i], pitch[i], kW * 2, kH) == m.cpu_hash) ++raw_matched;
        ++total;
        if (dh == m.cpu_hash && ch == m.cpu_hash) ++matched;
        else fprintf(stderr, "  mismatch seq=%u buf=%d prod=%016llx dev=%016llx cpu=%016llx\n", m.seq, i,
                     (unsigned long long)m.cpu_hash, (unsigned long long)dh, (unsigned long long)ch);

        // On the last frame: GPU writes a marker; producer must see it (same memory).
        if (m.seq == kFrames) {
            CHECK(cuMemsetD8(reinterpret_cast<CUdeviceptr>(dptr[i]), kMark, 1) == CUDA_SUCCESS, "memset");
            cuCtxSynchronize();
        }
        uint8_t ack = 1;
        CHECK(write(sock, &ack, 1) == 1, "ack");
    }
    printf("[P3] device hash == producer CPU hash on %d/%d frames\n", matched, total);
    printf("[P4] plain-mmap hash == producer CPU hash on %d/%d frames\n", raw_matched, total);

    for (int i = 0; i < kBufs; ++i) {
        cuGraphicsUnregisterResource(res[i]);
        NvBufSurfaceUnMapEglImage(surf[i], 0);
    }
    cuCtxDestroy(ctx);
    return matched == total && total == kFrames ? 0 : 1;
}

// ---------------- producer (parent): P1 ----------------
int producer(int sock, const char* devpath) {
    NvBufSurfaceAllocateParams ap{};
    ap.params.width = kW;
    ap.params.height = kH;
    ap.params.colorFormat = NVBUF_COLOR_FORMAT_YUYV;
    ap.params.layout = NVBUF_LAYOUT_PITCH;
    ap.params.memType = NVBUF_MEM_SURFACE_ARRAY;
    ap.memtag = NvBufSurfaceTag_CAMERA;

    NvBufSurface* surf[kBufs];
    int fds[kBufs];
    for (int i = 0; i < kBufs; ++i) {
        CHECK(NvBufSurfaceAllocate(&surf[i], 1, &ap) == 0, "NvBufSurfaceAllocate");
        surf[i]->numFilled = 1;
        fds[i] = static_cast<int>(surf[i]->surfaceList[0].bufferDesc);
    }
    auto& p0 = surf[0]->surfaceList[0];
    printf("[alloc] %d NvBufSurface YUYV %ux%u pitch=%u size=%u fd0=%d\n", kBufs, p0.width, p0.height,
           p0.pitch, p0.dataSize, fds[0]);

    int v = open(devpath, O_RDWR);
    CHECK(v >= 0, "open %s", devpath);
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = kW;
    fmt.fmt.pix.height = kH;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.bytesperline = p0.pitch;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    CHECK(ioctl(v, VIDIOC_S_FMT, &fmt) == 0, "S_FMT");
    printf("[v4l2] %ux%u bytesperline=%u sizeimage=%u\n", fmt.fmt.pix.width, fmt.fmt.pix.height,
           fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
    CHECK(fmt.fmt.pix.bytesperline == p0.pitch, "driver forced bytesperline %u != surface pitch %u",
          fmt.fmt.pix.bytesperline, p0.pitch);

    v4l2_requestbuffers rb{};
    rb.count = kBufs; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_DMABUF;
    CHECK(ioctl(v, VIDIOC_REQBUFS, &rb) == 0, "REQBUFS(DMABUF): %s", strerror(errno));
    for (int i = 0; i < kBufs; ++i) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_DMABUF;
        b.index = i; b.m.fd = fds[i]; b.length = surf[i]->surfaceList[0].dataSize;
        CHECK(ioctl(v, VIDIOC_QBUF, &b) == 0, "QBUF(%d): %s", i, strerror(errno));
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    CHECK(ioctl(v, VIDIOC_STREAMON, &type) == 0, "STREAMON: %s", strerror(errno));
    printf("[P1] UVC accepted %d NvBufSurface FDs as V4L2_MEMORY_DMABUF capture targets\n", kBufs);

    CHECK(send_fds(sock, fds, kBufs) == 0, "send_fds");
    NvBufSurfaceMapParams mp[kBufs];
    for (int i = 0; i < kBufs; ++i)
        CHECK(NvBufSurfaceGetMapParams(surf[i], 0, &mp[i]) == 0, "GetMapParams(%d)", i);
    CHECK(write(sock, mp, sizeof mp) == sizeof mp, "send map params");
    printf("[P2] producer sent %d FDs (SCM_RIGHTS) + map params (%zu B each)\n", kBufs, sizeof mp[0]);

    int last_idx = -1;
    for (uint32_t seq = 1; seq <= kFrames; ++seq) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_DMABUF;
        CHECK(ioctl(v, VIDIOC_DQBUF, &b) == 0, "DQBUF: %s", strerror(errno));
        int i = b.index;
        Msg m{i, seq, 0};
        CHECK(cpu_hash(surf[i], 0, &m.cpu_hash) == 0, "producer cpu map");
        CHECK(write(sock, &m, sizeof m) == sizeof m, "send meta");
        uint8_t ack;
        CHECK(read(sock, &ack, 1) == 1, "wait ack");
        if (seq == kFrames) { last_idx = i; break; }   // keep the last buffer dequeued
        b.m.fd = fds[i]; b.length = surf[i]->surfaceList[0].dataSize;
        CHECK(ioctl(v, VIDIOC_QBUF, &b) == 0, "re-QBUF");
    }
    Msg end{-1, 0, 0};
    CHECK(write(sock, &end, sizeof end) == sizeof end, "send end");
    ioctl(v, VIDIOC_STREAMOFF, &type);

    uint64_t h; uint8_t b0 = 0;
    CHECK(cpu_hash(surf[last_idx], 0, &h, &b0) == 0, "readback");
    printf("[P3] producer reads byte0=0x%02X after consumer's GPU write (expect 0x%02X)\n", b0, kMark);

    close(v);
    for (int i = 0; i < kBufs; ++i) NvBufSurfaceDestroy(surf[i]);
    return b0 == kMark ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const char* dev = argc > 1 ? argv[1] : "/dev/video0";
    setvbuf(stdout, nullptr, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 1; }
    pid_t pid = fork();
    if (pid == 0) { close(sv[0]); _exit(consumer(sv[1])); }
    close(sv[1]);
    int prc = producer(sv[0], dev);
    close(sv[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    int crc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    printf("\nRESULT: producer=%s consumer=%s\n", prc ? "FAIL" : "ok", crc ? "FAIL" : "ok");
    return prc || crc;
}
