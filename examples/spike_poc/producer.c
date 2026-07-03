// SPDX-License-Identifier: Apache-2.0
// dczc Spike PoC — producer
//   (V4L2 capture + dma-buf export + SCM_RIGHTS sidecar)
//
// Validation items for week 1-2 (design doc §6.4):
//   1. V4L2 → VIDIOC_REQBUFS(MMAP) → VIDIOC_EXPBUF extracts a dma-buf FD ✓
//   2. Listen on a Unix domain socket ✓
//   3. On consumer connect, deliver every dma-buf FD via SCM_RIGHTS at once ✓
//   4. Capture loop — publish each frame's buffer_index/seqno/timestamp over
//      the socket ✓
//
// Next: replace the metadata publish with Iceoryx2 and add accelerator import.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <linux/videodev2.h>
#include <time.h>
#include <unistd.h>

#define N_BUFFERS       4
#define SOCKET_PATH     "/tmp/dczc_spike.sock"
#define POOL_GENERATION 1
#define WIRE_VERSION    1

// ── Wire format (simplified spike messages) ───────────────────────
struct handshake_msg {
    uint32_t wire_version;
    uint32_t pool_generation;
    uint32_t n_buffers;
    uint32_t buffer_size;
};

struct frame_meta {
    uint32_t wire_version;
    uint32_t buffer_index;
    uint64_t seqno;
    uint64_t capture_ts_ns;     // V4L2 hardware timestamp or monotonic
    uint64_t producer_publish_ts_ns;
    uint32_t pool_generation;
    uint32_t reserved;
};

// ── Utility ───────────────────────────────────────────────────────
static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Deliver N FDs at once via SCM_RIGHTS, attaching the handshake_msg payload.
static int send_fds_with_payload(int sockfd, const int *fds, int n_fds,
                                  const void *payload, size_t payload_len) {
    struct iovec iov = { .iov_base = (void*)payload, .iov_len = payload_len };
    char cmsgbuf[CMSG_SPACE(sizeof(int) * 32)];  // up to 32 FDs
    if ((size_t)n_fds > 32) return -1;

    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)n_fds);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int) * (size_t)n_fds);
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * (size_t)n_fds);

    ssize_t r = sendmsg(sockfd, &msg, 0);
    if (r < 0) {
        perror("sendmsg(SCM_RIGHTS)");
        return -1;
    }
    return 0;
}

// ── V4L2 setup ────────────────────────────────────────────────────
static int v4l2_open(const char *dev) {
    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("open V4L2 device");
        return -1;
    }
    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        close(fd);
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "device does not support V4L2 capture + streaming\n");
        close(fd);
        return -1;
    }
    return fd;
}

static int v4l2_set_format(int fd, uint32_t *out_width, uint32_t *out_height,
                            uint32_t *out_size) {
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width  = 640;
    fmt.fmt.pix.height = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field  = V4L2_FIELD_ANY;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        return -1;
    }
    *out_width  = fmt.fmt.pix.width;
    *out_height = fmt.fmt.pix.height;
    *out_size   = fmt.fmt.pix.sizeimage;
    fprintf(stderr, "V4L2 format: %ux%u (%u bytes/frame)\n",
            fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage);
    return 0;
}

static int v4l2_request_buffers(int fd, uint32_t n) {
    struct v4l2_requestbuffers req = {0};
    req.count  = n;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        return -1;
    }
    if (req.count < n) {
        fprintf(stderr, "requested %u buffers, only %u allocated\n", n, req.count);
        return -1;
    }
    return 0;
}

// Core: export each V4L2 buffer as a dma-buf.
static int v4l2_export_dma_buf(int fd, uint32_t index, int *dma_buf_fd_out) {
    struct v4l2_exportbuffer expbuf = {0};
    expbuf.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    expbuf.index = index;
    expbuf.flags = O_RDWR;
    if (ioctl(fd, VIDIOC_EXPBUF, &expbuf) < 0) {
        perror("VIDIOC_EXPBUF");
        return -1;
    }
    *dma_buf_fd_out = expbuf.fd;
    return 0;
}

static int v4l2_qbuf(int fd, uint32_t index) {
    struct v4l2_buffer buf = {0};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = index;
    return ioctl(fd, VIDIOC_QBUF, &buf);
}

static int v4l2_dqbuf(int fd, struct v4l2_buffer *buf) {
    memset(buf, 0, sizeof(*buf));
    buf->type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf->memory = V4L2_MEMORY_MMAP;
    return ioctl(fd, VIDIOC_DQBUF, buf);
}

// ── Socket setup ──────────────────────────────────────────────────
static int unix_listen(const char *path) {
    unlink(path);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return -1; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(s); return -1;
    }
    if (listen(s, 1) < 0) { perror("listen"); close(s); return -1; }
    return s;
}

// ── Main ──────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s /dev/videoN\n", argv[0]);
        return 1;
    }
    const char *dev = argv[1];

    int v4l2_fd = v4l2_open(dev);
    if (v4l2_fd < 0) return 1;

    uint32_t width, height, frame_size;
    if (v4l2_set_format(v4l2_fd, &width, &height, &frame_size) < 0) return 1;
    if (v4l2_request_buffers(v4l2_fd, N_BUFFERS) < 0) return 1;

    int dma_buf_fds[N_BUFFERS];
    for (uint32_t i = 0; i < N_BUFFERS; i++) {
        if (v4l2_export_dma_buf(v4l2_fd, i, &dma_buf_fds[i]) < 0) return 1;
        if (v4l2_qbuf(v4l2_fd, i) < 0) {
            perror("VIDIOC_QBUF");
            return 1;
        }
        fprintf(stderr, "✓ buffer %u → dma-buf FD %d\n", i, dma_buf_fds[i]);
    }

    // Listen for the consumer on a Unix domain socket.
    int srv = unix_listen(SOCKET_PATH);
    if (srv < 0) return 1;
    fprintf(stderr, "✓ listening on %s — waiting for consumer\n", SOCKET_PATH);

    int conn = accept(srv, NULL, NULL);
    if (conn < 0) { perror("accept"); return 1; }
    fprintf(stderr, "✓ consumer connected\n");

    // ─── Handshake: bulk-deliver every dma-buf FD via SCM_RIGHTS ───
    struct handshake_msg hs = {
        .wire_version    = WIRE_VERSION,
        .pool_generation = POOL_GENERATION,
        .n_buffers       = N_BUFFERS,
        .buffer_size     = frame_size,
    };
    if (send_fds_with_payload(conn, dma_buf_fds, N_BUFFERS,
                               &hs, sizeof(hs)) < 0) return 1;
    fprintf(stderr, "✓ handshake done — %d FDs sent\n", N_BUFFERS);

    // ─── Start streaming ───
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON"); return 1;
    }

    // ─── Capture loop ───
    uint64_t seqno = 0;
    fprintf(stderr, "✓ streaming. Press Ctrl-C to stop.\n");
    for (;;) {
        // Simplified poll — busy retry. OK for the spike.
        struct v4l2_buffer buf;
        int r = v4l2_dqbuf(v4l2_fd, &buf);
        if (r < 0) {
            if (errno == EAGAIN) {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
                nanosleep(&ts, NULL);
                continue;
            }
            perror("VIDIOC_DQBUF");
            break;
        }

        // Publish metadata.
        struct frame_meta fm = {
            .wire_version           = WIRE_VERSION,
            .buffer_index           = buf.index,
            .seqno                  = ++seqno,
            .capture_ts_ns          = (uint64_t)buf.timestamp.tv_sec * 1000000000ULL
                                      + (uint64_t)buf.timestamp.tv_usec * 1000ULL,
            .producer_publish_ts_ns = monotonic_ns(),
            .pool_generation        = POOL_GENERATION,
            .reserved               = 0,
        };
        if (write(conn, &fm, sizeof(fm)) != (ssize_t)sizeof(fm)) {
            perror("write meta");
            break;
        }

        if (seqno % 30 == 0) {
            fprintf(stderr, "[producer] frame #%lu (buffer %u)\n",
                    seqno, buf.index);
        }

        // Re-enqueue.
        if (v4l2_qbuf(v4l2_fd, buf.index) < 0) {
            perror("VIDIOC_QBUF requeue");
            break;
        }
    }

    ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type);
    for (uint32_t i = 0; i < N_BUFFERS; i++) close(dma_buf_fds[i]);
    close(conn);
    close(srv);
    close(v4l2_fd);
    unlink(SOCKET_PATH);
    return 0;
}
