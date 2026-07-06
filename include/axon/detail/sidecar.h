// SPDX-License-Identifier: Apache-2.0
// axon — FD sidecar (internal)
//
// Implements the "FD plane" from the design doc §1.3. Iceoryx2 SHM queues only
// share memory, not the FD table, so dma-buf / sync_file FDs must travel through
// a separate channel. Default transport is SCM_RIGHTS over a Unix domain socket;
// pidfd_getfd(2) is exposed as a helper fallback for the connectionless path.
//
// This header is internal (detail/) — public users go through TensorPublisher /
// TensorSubscriber. It is exposed here only so the implementation and unit tests
// can share one definition.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace axon::detail {

// Maximum FDs deliverable in a single SCM_RIGHTS message. Matches the spike PoC
// control-buffer sizing and bounds the pool ring.
constexpr int kMaxSidecarFds = 32;

// Sidecar message tags (first byte of every framed payload).
enum class SidecarMsg : std::uint8_t {
    PoolHandshake = 1,  // bulk dma-buf FD delivery for a pool generation
    SyncFence     = 2,  // a single sync_file FD keyed by a token
};

// Payload that accompanies a PoolHandshake (sent alongside the FD array).
struct PoolHandshakeHeader {
    std::uint8_t  msg;              // == SidecarMsg::PoolHandshake
    std::uint8_t  reserved0[3];
    std::uint32_t wire_version;
    std::uint32_t pool_generation;
    std::uint32_t n_buffers;
    std::uint64_t buffer_size;
};

// Payload that accompanies a SyncFence (sent alongside exactly one FD).
struct SyncFenceHeader {
    std::uint8_t  msg;              // == SidecarMsg::SyncFence
    std::uint8_t  reserved0[7];
    std::uint64_t sync_fence_token;
};

// Derive the Unix-socket path for a service name (sanitized, under /tmp).
std::string sidecar_socket_path(const std::string& service_name);

// ---- Low-level SCM_RIGHTS primitives (process-agnostic, unit-testable) ----

// Send `n_fds` file descriptors plus an inline payload over a connected socket.
// Returns 0 on success, -1 with errno set on failure.
int send_fds(int sockfd, const int* fds, int n_fds,
             const void* payload, std::size_t payload_len);

// Receive up to `max_fds` descriptors plus an inline payload. On success writes
// the count to *n_fds_out and the descriptors to fds_out (caller owns them).
// Returns 0 on success, -1 with errno set on failure.
int recv_fds(int sockfd, int* fds_out, int max_fds, int* n_fds_out,
             void* payload, std::size_t payload_len);

// pidfd_getfd(2) fallback (design doc §1.3.2). Steals `remote_fd` from the
// process referred to by `pidfd`. Returns the new local FD, or -1 with errno.
// Requires PTRACE_MODE_ATTACH_REALCREDS on the target.
int pidfd_getfd_fallback(int pidfd, int remote_fd);

// ---- Server (publisher side) ----

// Owns a listening Unix socket and the set of connected consumers. The current
// pool bundle is cached so late joiners and re-announces resend it.
class SidecarServer {
public:
    static SidecarServer* create(const std::string& service_name);
    ~SidecarServer();

    // Cache the current pool bundle. Does not transmit on its own.
    void set_pool(const std::vector<int>& fds, std::uint32_t pool_generation,
                  std::uint64_t buffer_size, std::uint32_t wire_version);

    // Accept every consumer connection that is pending within `timeout_ms`
    // (0 = poll once, non-blocking) and send the cached bundle to each new
    // consumer. Returns the number of consumers accepted, or -1 on error.
    int accept_and_handshake(int timeout_ms);

    // Resend the cached bundle to all currently connected consumers. Returns the
    // number of consumers reached, or -1 on error.
    int broadcast_pool();

    // Deliver one sync_file FD (keyed by token) to all connected consumers.
    int broadcast_sync_fence(std::uint64_t token, int sync_fd);

    int connected_count() const noexcept;

    SidecarServer(const SidecarServer&) = delete;
    SidecarServer& operator=(const SidecarServer&) = delete;

private:
    SidecarServer();
    struct Impl;
    Impl* impl_;
};

// ---- Client (subscriber side) ----

class SidecarClient {
public:
    static SidecarClient* create(const std::string& service_name);
    ~SidecarClient();

    // Connect to the server socket, retrying until `timeout_ms` elapses.
    int connect(int timeout_ms);

    // Block (up to timeout_ms) for the bulk pool handshake. On success fills the
    // header and appends received FDs (caller owns them) to fds_out.
    int recv_pool_handshake(PoolHandshakeHeader* header_out,
                            std::vector<int>* fds_out, int timeout_ms);

    // Non-blocking drain of any pending sync-fence messages. For each one,
    // invokes the provided sink with (token, fd). Returns the count processed.
    int poll_sync_fences(int timeout_ms,
                         void (*sink)(std::uint64_t token, int fd, void* ctx),
                         void* ctx);

    int fd() const noexcept;

    SidecarClient(const SidecarClient&) = delete;
    SidecarClient& operator=(const SidecarClient&) = delete;

private:
    SidecarClient();
    struct Impl;
    Impl* impl_;
};

}  // namespace axon::detail
