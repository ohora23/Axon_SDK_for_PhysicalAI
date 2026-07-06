// SPDX-License-Identifier: Apache-2.0
// FD sidecar — SCM_RIGHTS primitives verified across a real process boundary.
//
// The child receives FDs over a socketpair and reads their contents; matching
// bytes prove the *same* kernel object crossed the boundary (true FD passing,
// not a copy).

#include "axon/detail/sidecar.h"
#include "axon_test.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace axon::detail;

namespace {

constexpr std::size_t kBufSize = 4096;

// Create a memfd of kBufSize filled with `fill`.
int make_filled_memfd(unsigned char fill) {
    int fd = memfd_create("axon_test", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, kBufSize) < 0) { close(fd); return -1; }
    void* p = mmap(nullptr, kBufSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return -1; }
    memset(p, fill, kBufSize);
    munmap(p, kBufSize);
    return fd;
}

// Read the first byte of an FD's mapping.
bool first_byte_equals(int fd, unsigned char expect) {
    void* p = mmap(nullptr, kBufSize, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return false;
    bool ok = (static_cast<unsigned char*>(p)[0] == expect) &&
              (static_cast<unsigned char*>(p)[kBufSize - 1] == expect);
    munmap(p, kBufSize);
    return ok;
}

struct WirePayload {
    std::uint32_t magic;
    std::uint32_t n;
};

}  // namespace

AXON_TEST(single_fd_roundtrip) {
    int fd = make_filled_memfd(0x5a);
    REQUIRE(fd >= 0);

    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        int got[kMaxSidecarFds];
        int n = 0;
        WirePayload p {};
        int r = recv_fds(sv[1], got, kMaxSidecarFds, &n, &p, sizeof(p));
        int rc = (r == 0 && n == 1 && p.magic == 0xC0FFEE && first_byte_equals(got[0], 0x5a)) ? 0 : 1;
        _exit(rc);
    }
    close(sv[1]);
    WirePayload p {0xC0FFEE, 1};
    CHECK(send_fds(sv[0], &fd, 1, &p, sizeof(p)) == 0);

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    close(sv[0]);
    close(fd);
}

AXON_TEST(bulk_fd_roundtrip) {
    const int N = 4;
    int fds[N];
    for (int i = 0; i < N; ++i) {
        fds[i] = make_filled_memfd(static_cast<unsigned char>(0x10 + i));
        REQUIRE(fds[i] >= 0);
    }

    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        int got[kMaxSidecarFds];
        int n = 0;
        WirePayload p {};
        int r = recv_fds(sv[1], got, kMaxSidecarFds, &n, &p, sizeof(p));
        int rc = 0;
        if (r != 0 || n != N || p.n != static_cast<std::uint32_t>(N)) rc = 1;
        for (int i = 0; i < n && rc == 0; ++i) {
            if (!first_byte_equals(got[i], static_cast<unsigned char>(0x10 + i))) rc = 1;
        }
        _exit(rc);
    }
    close(sv[1]);
    WirePayload p {0xC0FFEE, N};
    CHECK(send_fds(sv[0], fds, N, &p, sizeof(p)) == 0);

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(sv[0]);
    for (int i = 0; i < N; ++i) close(fds[i]);
}

AXON_TEST(too_many_fds_rejected) {
    int fd = make_filled_memfd(0x01);
    REQUIRE(fd >= 0);
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    // Sender caps at kMaxSidecarFds.
    CHECK(send_fds(sv[0], &fd, kMaxSidecarFds + 1, "x", 1) == -1);
    CHECK(errno == EINVAL);
    close(sv[0]); close(sv[1]); close(fd);
}

AXON_TEST(pidfd_getfd_bad_args) {
    // Bogus descriptors must fail cleanly, not crash.
    CHECK(pidfd_getfd_fallback(-1, -1) == -1);
}

AXON_TEST(socket_path_sanitized) {
    std::string p = sidecar_socket_path("camera/inference:out");
    const std::string prefix = "/tmp/axon.";
    REQUIRE(p.rfind(prefix, 0) == 0);
    std::string tail = p.substr(prefix.size());  // service-derived portion
    CHECK(tail.find('/') == std::string::npos);   // slashes sanitized away
    CHECK(tail.find(':') == std::string::npos);   // colons too
}

AXON_TEST_MAIN("sidecar")
