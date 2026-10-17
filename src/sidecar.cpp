// SPDX-License-Identifier: Apache-2.0
// axon — FD sidecar implementation (design doc §1.3)

#include "axon/detail/sidecar.h"

#include <cerrno>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

namespace axon::detail {

namespace {

void sleep_ms(int ms) {
    struct timespec ts {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

// Sanitize a service name into a filesystem-safe token.
std::string sanitize(const std::string& service_name) {
    std::string out;
    out.reserve(service_name.size());
    for (char c : service_name) {
        out.push_back((c == '/' || c == ' ' || c == ':') ? '_' : c);
    }
    return out;
}

// Read exactly `n` bytes from `fd` into `buf`. EINTR-safe.
// Returns 0 on success, -1 with errno=ETIMEDOUT (poll timeout) or errno=EPROTO (EOF before n bytes).
int read_exact(int fd, void* buf, size_t n, int timeout_ms) {
    if (n == 0) return 0;
    char* p = static_cast<char*>(buf);
    size_t remaining = n;
    while (remaining > 0) {
        struct pollfd pfd {fd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) {
            if (pr == 0) errno = ETIMEDOUT;
            return -1;
        }
        ssize_t r = ::recv(fd, p, remaining, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) { errno = EPROTO; return -1; }
        p += r;
        remaining -= static_cast<size_t>(r);
        // Reduce timeout for subsequent reads (already consumed some).
        timeout_ms = 1000;
    }
    return 0;
}

}  // namespace

std::string sidecar_socket_path(const std::string& service_name) {
    return "/tmp/axon." + sanitize(service_name) + ".sock";
}

// ---------------------------------------------------------------- primitives

int send_fds(int sockfd, const int* fds, int n_fds,
             const void* payload, std::size_t payload_len) {
    if (n_fds < 0 || n_fds > kMaxSidecarFds) {
        errno = EINVAL;
        return -1;
    }
    struct iovec iov {const_cast<void*>(payload), payload_len};

    union {
        char buf[CMSG_SPACE(sizeof(int) * kMaxSidecarFds)];
        struct cmsghdr align;
    } cmsg_un;
    std::memset(&cmsg_un, 0, sizeof(cmsg_un));

    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (n_fds > 0) {
        msg.msg_control = cmsg_un.buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * static_cast<size_t>(n_fds));
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * static_cast<size_t>(n_fds));
        std::memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * static_cast<size_t>(n_fds));
    }

    for (;;) {
        ssize_t r = sendmsg(sockfd, &msg, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        return 0;
    }
}

int recv_fds(int sockfd, int* fds_out, int max_fds, int* n_fds_out,
             void* payload, std::size_t payload_len) {
    struct iovec iov {payload, payload_len};

    union {
        char buf[CMSG_SPACE(sizeof(int) * kMaxSidecarFds)];
        struct cmsghdr align;
    } cmsg_un;
    std::memset(&cmsg_un, 0, sizeof(cmsg_un));

    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_un.buf;
    msg.msg_controllen = sizeof(cmsg_un.buf);

    ssize_t r;
    do {
        r = recvmsg(sockfd, &msg, MSG_CMSG_CLOEXEC);
    } while (r < 0 && errno == EINTR);

    if (r < 0) return -1;
    if (static_cast<std::size_t>(r) != payload_len) {
        errno = EPROTO;
        return -1;
    }

    int n = 0;
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            n = static_cast<int>((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            if (n > max_fds) {
                // Close the surplus FDs to avoid leaking them.
                int tmp[kMaxSidecarFds];
                std::memcpy(tmp, CMSG_DATA(cmsg), sizeof(int) * static_cast<size_t>(n));
                for (int i = 0; i < n; ++i) ::close(tmp[i]);
                errno = EOVERFLOW;
                return -1;
            }
            std::memcpy(fds_out, CMSG_DATA(cmsg), sizeof(int) * static_cast<size_t>(n));
            break;
        }
    }
    *n_fds_out = n;
    return 0;
}

int pidfd_getfd_fallback(int pidfd, int remote_fd) {
#ifdef SYS_pidfd_getfd
    long r = syscall(SYS_pidfd_getfd, pidfd, remote_fd, 0u);
    return static_cast<int>(r);
#else
    errno = ENOSYS;
    return -1;
#endif
}

// ------------------------------------------------------------------- server

struct SidecarServer::Impl {
    int listen_fd = -1;
    std::string path;
    std::vector<int> clients;

    // Cached pool bundle.
    std::vector<int> pool_fds;
    PoolHandshakeHeader header {};
    std::vector<std::uint8_t> ext;  // backend import blob

    int send_pool_to(int client_fd) {
        int r = send_fds(client_fd, pool_fds.data(),
                         static_cast<int>(pool_fds.size()),
                         &header, sizeof(header));
        if (r < 0) return -1;
        if (!ext.empty()) {
            const char* p = reinterpret_cast<const char*>(ext.data());
            size_t remaining = ext.size();
            for (;;) {
                ssize_t w = ::send(client_fd, p, remaining, MSG_NOSIGNAL);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    return -1;
                }
                if (static_cast<size_t>(w) == remaining) break;
                p += w;
                remaining -= static_cast<size_t>(w);
            }
        }
        return 0;
    }
};

SidecarServer::SidecarServer() : impl_(new Impl) {}

SidecarServer::~SidecarServer() {
    for (int c : impl_->clients) ::close(c);
    if (impl_->listen_fd >= 0) ::close(impl_->listen_fd);
    if (!impl_->path.empty()) ::unlink(impl_->path.c_str());
    delete impl_;
}

SidecarServer* SidecarServer::create(const std::string& service_name) {
    auto* s = new SidecarServer();
    s->impl_->path = sidecar_socket_path(service_name);
    ::unlink(s->impl_->path.c_str());

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { delete s; return nullptr; }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", s->impl_->path.c_str());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, 16) < 0) {
        ::close(fd);
        delete s;
        return nullptr;
    }
    s->impl_->listen_fd = fd;
    return s;
}

bool SidecarServer::set_pool(const std::vector<int>& fds,
                              std::uint32_t pool_generation,
                              std::uint64_t buffer_size,
                              std::uint32_t wire_version,
                              std::uint8_t backend,
                              const void* ext, std::size_t ext_len) {
    impl_->pool_fds = fds;
    impl_->header = PoolHandshakeHeader{};
    impl_->header.msg = static_cast<std::uint8_t>(SidecarMsg::PoolHandshake);
    impl_->header.backend = backend;
    impl_->header.ext_len = static_cast<std::uint16_t>(ext_len);
    impl_->header.wire_version = wire_version;
    impl_->header.pool_generation = pool_generation;
    impl_->header.n_buffers = static_cast<std::uint32_t>(fds.size());
    impl_->header.buffer_size = buffer_size;
    if (ext_len > 0 && ext_len <= kMaxHandshakeExt && ext) {
        impl_->ext.assign(static_cast<const std::uint8_t*>(ext),
                          static_cast<const std::uint8_t*>(ext) + ext_len);
        return true;
    }
    impl_->ext.clear();
    impl_->header.ext_len = 0;
    return ext_len == 0;  // false: a blob was given but cannot be carried
}

int SidecarServer::accept_and_handshake(int timeout_ms) {
    int accepted = 0;
    for (;;) {
        struct pollfd pfd {impl_->listen_fd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, accepted == 0 ? timeout_ms : 0);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) break;  // nothing (more) pending

        int c = ::accept4(impl_->listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (impl_->send_pool_to(c) < 0) {
            ::close(c);
            return -1;
        }
        impl_->clients.push_back(c);
        ++accepted;
    }
    return accepted;
}

int SidecarServer::broadcast_pool() {
    int n = 0;
    for (int c : impl_->clients) {
        if (impl_->send_pool_to(c) == 0) ++n;
    }
    return n;
}

int SidecarServer::broadcast_sync_fence(std::uint64_t token, int sync_fd) {
    SyncFenceHeader h {};
    h.msg = static_cast<std::uint8_t>(SidecarMsg::SyncFence);
    h.sync_fence_token = token;
    int n = 0;
    for (int c : impl_->clients) {
        if (send_fds(c, &sync_fd, 1, &h, sizeof(h)) == 0) ++n;
    }
    return n;
}

int SidecarServer::connected_count() const noexcept {
    return static_cast<int>(impl_->clients.size());
}

// ------------------------------------------------------------------- client

struct SidecarClient::Impl {
    int sock = -1;
    std::string path;
};

SidecarClient::SidecarClient() : impl_(new Impl) {}

SidecarClient::~SidecarClient() {
    if (impl_->sock >= 0) ::close(impl_->sock);
    delete impl_;
}

SidecarClient* SidecarClient::create(const std::string& service_name) {
    auto* c = new SidecarClient();
    c->impl_->path = sidecar_socket_path(service_name);
    return c;
}

int SidecarClient::connect(int timeout_ms) {
    int sock = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) return -1;

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", impl_->path.c_str());

    int waited = 0;
    const int step = 20;
    for (;;) {
        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            if (impl_->sock >= 0) ::close(impl_->sock);  // re-handshake: drop the old link
            impl_->sock = sock;
            return 0;
        }
        if (waited >= timeout_ms) break;
        sleep_ms(step);
        waited += step;
    }
    ::close(sock);
    return -1;
}

int SidecarClient::recv_pool_handshake(PoolHandshakeHeader* header_out,
                                       std::vector<int>* fds_out,
                                       int timeout_ms,
                                       std::vector<std::uint8_t>* ext_out) {
    struct pollfd pfd {impl_->sock, POLLIN, 0};
    int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        if (pr == 0) errno = ETIMEDOUT;
        return -1;
    }

    int fds[kMaxSidecarFds];
    int n = 0;
    if (recv_fds(impl_->sock, fds, kMaxSidecarFds, &n, header_out,
                 sizeof(*header_out)) < 0) {
        return -1;
    }
    if (header_out->msg != static_cast<std::uint8_t>(SidecarMsg::PoolHandshake)) {
        for (int i = 0; i < n; ++i) ::close(fds[i]);
        errno = EPROTO;
        return -1;
    }
    fds_out->assign(fds, fds + n);

    // Read (and discard if ext_out is null) the extension blob to keep the stream framed.
    if (header_out->ext_len > 0) {
        std::vector<std::uint8_t> tmp(header_out->ext_len);
        if (read_exact(impl_->sock, tmp.data(), tmp.size(), timeout_ms) < 0) {
            for (int i = 0; i < n; ++i) ::close(fds[i]);
            fds_out->clear();
            return -1;
        }
        if (ext_out) *ext_out = std::move(tmp);
    } else {
        if (ext_out) ext_out->clear();
    }
    return 0;
}

int SidecarClient::poll_sync_fences(int timeout_ms,
                                    void (*sink)(std::uint64_t, int, void*),
                                    void* ctx) {
    int count = 0;
    for (;;) {
        struct pollfd pfd {impl_->sock, POLLIN, 0};
        int pr = ::poll(&pfd, 1, count == 0 ? timeout_ms : 0);
        if (pr <= 0) break;

        // Peek the message tag first (1 byte).
        std::uint8_t tag;
        ssize_t peeked = ::recv(impl_->sock, &tag, 1, MSG_PEEK);
        if (peeked <= 0) {
            if (peeked < 0 && errno == EINTR) continue;
            break;
        }

        int fds[kMaxSidecarFds];
        int n = 0;
        // SyncFenceHeader is 16 bytes; PoolHandshakeHeader is 24 bytes.
        std::size_t hdr_size = (tag == static_cast<std::uint8_t>(SidecarMsg::SyncFence))
                               ? sizeof(SyncFenceHeader) : sizeof(PoolHandshakeHeader);

        alignas(std::max_align_t) char raw_hdr[24];
        if (recv_fds(impl_->sock, fds, kMaxSidecarFds, &n, raw_hdr, hdr_size) < 0) {
            break;
        }

        if (tag == static_cast<std::uint8_t>(SidecarMsg::SyncFence) && n == 1) {
            const auto& sh = *reinterpret_cast<SyncFenceHeader*>(raw_hdr);
            if (sink) sink(sh.sync_fence_token, fds[0], ctx);
            else ::close(fds[0]);
            ++count;
        } else if (tag == static_cast<std::uint8_t>(SidecarMsg::PoolHandshake)) {
            const auto& ph = *reinterpret_cast<PoolHandshakeHeader*>(raw_hdr);
            // Mid-stream re-announce: read and discard ext_len bytes (100 ms)
            // so the next message stays framed, then close the FDs.
            // The publisher sends header + blob back to back, so this does not
            // wait in practice. If it fails the stream is desynced: shut the
            // socket down rather than misparse every later fence.
            bool framed = true;
            char sink_buf[512];
            for (std::size_t left = ph.ext_len; left > 0 && framed;) {
                std::size_t chunk = left < sizeof sink_buf ? left : sizeof sink_buf;
                framed = read_exact(impl_->sock, sink_buf, chunk, 100) == 0;
                left -= chunk;
            }
            for (int i = 0; i < n; ++i) ::close(fds[i]);
            if (!framed) { ::shutdown(impl_->sock, SHUT_RDWR); break; }
        } else {
            for (int i = 0; i < n; ++i) ::close(fds[i]);
        }
    }
    return count;
}

int SidecarClient::fd() const noexcept { return impl_->sock; }

}  // namespace axon::detail
