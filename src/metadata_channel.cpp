// SPDX-License-Identifier: Apache-2.0
// axon — Metadata plane: backend factory + POSIX-SHM seqlock backend (§1.1/§3.3)

#include "axon/detail/metadata_channel.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace axon::detail {

// Defined in metadata_channel_iox2.cpp (only compiled with AXON_WITH_ICEORYX2).
#ifdef AXON_WITH_ICEORYX2
MetadataChannel* make_iceoryx2_publisher(const std::string& service_name);
MetadataChannel* make_iceoryx2_subscriber(const std::string& service_name,
                                          int timeout_ms);
#endif

namespace {

void sleep_ms(int ms) {
    struct timespec ts {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

#ifdef AXON_WITH_ICEORYX2
// Which backend to use: AXON_METADATA_BACKEND = "seqlock" | "iceoryx2".
// Only meaningful when the Iceoryx2 backend is compiled in; default is iceoryx2.
enum class Backend { Seqlock, Iceoryx2 };

Backend selected_backend() {
    const char* env = std::getenv("AXON_METADATA_BACKEND");
    if (env && std::strcmp(env, "seqlock") == 0) return Backend::Seqlock;
    return Backend::Iceoryx2;
}
#endif

// Shared-memory layout for the seqlock backend. The guard is an independent
// seqlock counter (odd while a write is in progress) — distinct from
// TensorDescriptor::seqno, which is the application-level data version.
struct alignas(64) MetadataSlot {
    std::atomic<std::uint64_t> guard;
    std::uint32_t              wire_version;
    std::uint32_t              ready;   // 0 until the first publish lands
    TensorDescriptor           desc;
};

// -------------------------------------------------------- seqlock backend

class SeqlockMetadataChannel final : public MetadataChannel {
public:
    static SeqlockMetadataChannel* create_publisher(const std::string& service_name);
    static SeqlockMetadataChannel* create_subscriber(const std::string& service_name,
                                                     int timeout_ms);
    ~SeqlockMetadataChannel() override;

    void publish(const TensorDescriptor& desc) noexcept override;
    bool read_latest(TensorDescriptor* out, int max_retry,
                     int* retries_out) noexcept override;
    bool has_data() const noexcept override;

private:
    SeqlockMetadataChannel() = default;
    MetadataSlot* slot_ = nullptr;
    std::size_t   map_len_ = sizeof(MetadataSlot);
    std::string   shm_name_;
    bool          owns_ = false;
};

SeqlockMetadataChannel::~SeqlockMetadataChannel() {
    if (slot_ && slot_ != MAP_FAILED) ::munmap(slot_, map_len_);
    if (owns_ && !shm_name_.empty()) ::shm_unlink(shm_name_.c_str());
}

SeqlockMetadataChannel* SeqlockMetadataChannel::create_publisher(
    const std::string& service_name) {
    auto* ch = new SeqlockMetadataChannel();
    ch->shm_name_ = metadata_shm_name(service_name);
    ch->owns_ = true;

    ::shm_unlink(ch->shm_name_.c_str());
    int fd = ::shm_open(ch->shm_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { delete ch; return nullptr; }

    if (::ftruncate(fd, static_cast<off_t>(ch->map_len_)) < 0) {
        ::close(fd);
        ::shm_unlink(ch->shm_name_.c_str());
        delete ch;
        return nullptr;
    }
    void* p = ::mmap(nullptr, ch->map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        ::shm_unlink(ch->shm_name_.c_str());
        delete ch;
        return nullptr;
    }
    ch->slot_ = static_cast<MetadataSlot*>(p);
    new (&ch->slot_->guard) std::atomic<std::uint64_t>(0);
    ch->slot_->wire_version = kWireVersion;
    ch->slot_->ready = 0;
    std::memset(&ch->slot_->desc, 0, sizeof(ch->slot_->desc));
    return ch;
}

SeqlockMetadataChannel* SeqlockMetadataChannel::create_subscriber(
    const std::string& service_name, int timeout_ms) {
    auto* ch = new SeqlockMetadataChannel();
    ch->shm_name_ = metadata_shm_name(service_name);
    ch->owns_ = false;

    int fd = -1, waited = 0;
    const int step = 20;
    for (;;) {
        fd = ::shm_open(ch->shm_name_.c_str(), O_RDWR, 0600);
        if (fd >= 0) break;
        if (waited >= timeout_ms) { delete ch; return nullptr; }
        sleep_ms(step);
        waited += step;
    }
    void* p = ::mmap(nullptr, ch->map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) { delete ch; return nullptr; }
    ch->slot_ = static_cast<MetadataSlot*>(p);
    return ch;
}

void SeqlockMetadataChannel::publish(const TensorDescriptor& desc) noexcept {
    std::atomic<std::uint64_t>& guard = slot_->guard;
    std::uint64_t g = guard.load(std::memory_order_relaxed);
    guard.store(g + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    slot_->desc = desc;
    std::atomic_thread_fence(std::memory_order_release);
    guard.store(g + 2, std::memory_order_release);
    slot_->ready = 1;
}

bool SeqlockMetadataChannel::read_latest(TensorDescriptor* out, int max_retry,
                                         int* retries_out) noexcept {
    if (retries_out) *retries_out = 0;
    if (slot_->ready == 0) return false;
    std::atomic<std::uint64_t>& guard = slot_->guard;
    for (int retry = 0; retry <= max_retry; ++retry) {
        std::uint64_t before = guard.load(std::memory_order_acquire);
        if (before & 1u) { if (retries_out) *retries_out = retry; continue; }
        std::atomic_thread_fence(std::memory_order_acquire);
        TensorDescriptor tmp = slot_->desc;
        std::atomic_thread_fence(std::memory_order_acquire);
        std::uint64_t after = guard.load(std::memory_order_acquire);
        if (before == after) {
            *out = tmp;
            if (retries_out) *retries_out = retry;
            return true;
        }
        if (retries_out) *retries_out = retry;
    }
    return false;
}

bool SeqlockMetadataChannel::has_data() const noexcept {
    return slot_ && slot_->ready != 0;
}

}  // namespace

// ------------------------------------------------------------- public name

std::string metadata_shm_name(const std::string& service_name) {
    std::string out = "/axon.";
    for (char c : service_name) {
        out.push_back((c == '/' || c == ' ' || c == ':') ? '_' : c);
    }
    return out;
}

// ---------------------------------------------------------------- factory

MetadataChannel* MetadataChannel::create_publisher(const std::string& service_name) {
#ifdef AXON_WITH_ICEORYX2
    if (selected_backend() == Backend::Iceoryx2) {
        if (auto* c = make_iceoryx2_publisher(service_name)) return c;
        // else fall through to the always-available seqlock backend
    }
#endif
    return SeqlockMetadataChannel::create_publisher(service_name);
}

MetadataChannel* MetadataChannel::create_subscriber(const std::string& service_name,
                                                    int timeout_ms) {
#ifdef AXON_WITH_ICEORYX2
    if (selected_backend() == Backend::Iceoryx2) {
        if (auto* c = make_iceoryx2_subscriber(service_name, timeout_ms)) return c;
    }
#endif
    return SeqlockMetadataChannel::create_subscriber(service_name, timeout_ms);
}

}  // namespace axon::detail
