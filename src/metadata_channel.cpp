// SPDX-License-Identifier: Apache-2.0
// dczc — Metadata plane: POSIX-SHM seqlock slot (Iceoryx2 stand-in, §1.1 / §3.3)

#include "dczc/detail/metadata_channel.h"

#include <cerrno>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dczc::detail {

namespace {

void sleep_ms(int ms) {
    struct timespec ts {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

}  // namespace

std::string metadata_shm_name(const std::string& service_name) {
    std::string out = "/dczc.";
    for (char c : service_name) {
        out.push_back((c == '/' || c == ' ' || c == ':') ? '_' : c);
    }
    return out;
}

MetadataChannel::MetadataChannel()
    : slot_(nullptr), map_len_(sizeof(MetadataSlot)), owns_(false) {}

MetadataChannel::~MetadataChannel() {
    if (slot_ && slot_ != MAP_FAILED) ::munmap(slot_, map_len_);
    if (owns_ && !shm_name_.empty()) ::shm_unlink(shm_name_.c_str());
}

MetadataChannel* MetadataChannel::create_publisher(const std::string& service_name) {
    auto* ch = new MetadataChannel();
    ch->shm_name_ = metadata_shm_name(service_name);
    ch->owns_ = true;

    // Fresh object every time the publisher starts.
    ::shm_unlink(ch->shm_name_.c_str());
    int fd = ::shm_open(ch->shm_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { delete ch; return nullptr; }

    if (::ftruncate(fd, static_cast<off_t>(ch->map_len_)) < 0) {
        ::close(fd);
        ::shm_unlink(ch->shm_name_.c_str());
        delete ch;
        return nullptr;
    }
    void* p = ::mmap(nullptr, ch->map_len_, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        ::shm_unlink(ch->shm_name_.c_str());
        delete ch;
        return nullptr;
    }
    ch->slot_ = static_cast<MetadataSlot*>(p);

    // Placement-construct the atomic and zero the payload.
    new (&ch->slot_->guard) std::atomic<std::uint64_t>(0);
    ch->slot_->wire_version = kWireVersion;
    ch->slot_->ready = 0;
    std::memset(&ch->slot_->desc, 0, sizeof(ch->slot_->desc));
    return ch;
}

MetadataChannel* MetadataChannel::create_subscriber(const std::string& service_name,
                                                    int timeout_ms) {
    auto* ch = new MetadataChannel();
    ch->shm_name_ = metadata_shm_name(service_name);
    ch->owns_ = false;

    int fd = -1;
    int waited = 0;
    const int step = 20;
    for (;;) {
        fd = ::shm_open(ch->shm_name_.c_str(), O_RDWR, 0600);
        if (fd >= 0) break;
        if (waited >= timeout_ms) { delete ch; return nullptr; }
        sleep_ms(step);
        waited += step;
    }

    void* p = ::mmap(nullptr, ch->map_len_, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) { delete ch; return nullptr; }
    ch->slot_ = static_cast<MetadataSlot*>(p);
    return ch;
}

void MetadataChannel::publish(const TensorDescriptor& desc) noexcept {
    std::atomic<std::uint64_t>& guard = slot_->guard;

    // Enter write: bump to odd. release so prior writes are visible first.
    std::uint64_t g = guard.load(std::memory_order_relaxed);
    guard.store(g + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);

    slot_->desc = desc;  // POD copy

    // Leave write: bump to even. release so the payload is visible before guard.
    std::atomic_thread_fence(std::memory_order_release);
    guard.store(g + 2, std::memory_order_release);

    slot_->ready = 1;
}

bool MetadataChannel::read_latest(TensorDescriptor* out, int max_retry,
                                  int* retries_out) noexcept {
    if (retries_out) *retries_out = 0;
    if (slot_->ready == 0) return false;

    std::atomic<std::uint64_t>& guard = slot_->guard;
    for (int retry = 0; retry <= max_retry; ++retry) {
        std::uint64_t before = guard.load(std::memory_order_acquire);
        if (before & 1u) {                 // writer in progress
            if (retries_out) *retries_out = retry;
            continue;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        TensorDescriptor tmp = slot_->desc;  // POD copy
        std::atomic_thread_fence(std::memory_order_acquire);
        std::uint64_t after = guard.load(std::memory_order_acquire);
        if (before == after) {
            *out = tmp;
            if (retries_out) *retries_out = retry;
            return true;
        }
        if (retries_out) *retries_out = retry;
    }
    return false;  // writer kept interleaving past the cap
}

bool MetadataChannel::has_data() const noexcept {
    return slot_ && slot_->ready != 0;
}

}  // namespace dczc::detail
