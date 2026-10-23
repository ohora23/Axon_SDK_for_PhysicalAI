// SPDX-License-Identifier: Apache-2.0
// axon — RT-Linux helpers (design doc §3.2)

#include "axon/rt.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

namespace axon {

namespace {

// Touch every page of a region so it is wired by mlockall before the RT loop.
void touch_pages(volatile char* p, std::size_t n) {
    const long page = sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096;
    for (std::size_t i = 0; i < n; i += static_cast<std::size_t>(page)) {
        p[i] = 0;
    }
    if (n > 0) p[n - 1] = 0;
}

std::string cpu_attr(int cpu, const char* rel) {
    return "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/" + rel;
}

bool read_line(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) {
        errno = ENOENT;
        return false;
    }
    std::getline(f, out);
    return true;
}

bool write_str(const std::string& path, const std::string& v) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;  // errno from open (EACCES without root / the udev rule)
    const ssize_t n = ::write(fd, v.data(), v.size());
    const int err = errno;
    ::close(fd);
    errno = err;
    return n == static_cast<ssize_t>(v.size());
}

// Held for the process lifetime when RtSetupConfig::cpu_latency_us >= 0.
std::optional<CpuLatencyHold> g_setup_latency_hold;

}  // namespace

std::optional<CpuLatencyHold> CpuLatencyHold::acquire(int max_latency_us, int cpu) {
    if (max_latency_us < 0 || cpu < -1) {
        errno = EINVAL;
        return std::nullopt;
    }
    if (cpu == -1) {
        // The kernel keeps the request only while this fd is open.
        const int fd = ::open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
        if (fd < 0) return std::nullopt;
        const std::int32_t v = max_latency_us;
        if (::write(fd, &v, sizeof v) != static_cast<ssize_t>(sizeof v)) {
            const int err = errno;
            ::close(fd);
            errno = err;
            return std::nullopt;
        }
        return CpuLatencyHold(fd, -1, max_latency_us, {});
    }
    // sysfs reads "n/a" for a 0 µs limit and "0" for no limit, and takes the same strings back,
    // so the value read here restores exactly.
    const std::string path = cpu_attr(cpu, "power/pm_qos_resume_latency_us");
    std::string prev;
    if (!read_line(path, prev)) return std::nullopt;
    if (!write_str(path, max_latency_us == 0 ? "n/a" : std::to_string(max_latency_us))) return std::nullopt;
    return CpuLatencyHold(-1, cpu, max_latency_us, prev);
}

CpuLatencyHold::CpuLatencyHold(int fd, int cpu, int us, std::string restore) noexcept
    : fd_(fd), cpu_(cpu), us_(us), restore_(std::move(restore)) {}

CpuLatencyHold::CpuLatencyHold(CpuLatencyHold&& o) noexcept
    : fd_(o.fd_), cpu_(o.cpu_), us_(o.us_), restore_(std::move(o.restore_)) {
    o.fd_ = -1;
    o.restore_.clear();
}

CpuLatencyHold& CpuLatencyHold::operator=(CpuLatencyHold&& o) noexcept {
    if (this != &o) {
        release();
        fd_ = o.fd_;
        cpu_ = o.cpu_;
        us_ = o.us_;
        restore_ = std::move(o.restore_);
        o.fd_ = -1;
        o.restore_.clear();
    }
    return *this;
}

CpuLatencyHold::~CpuLatencyHold() { release(); }

void CpuLatencyHold::release() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    } else if (cpu_ >= 0 && !restore_.empty()) {
        const int saved = errno;
        write_str(cpu_attr(cpu_, "power/pm_qos_resume_latency_us"), restore_);
        errno = saved;
        restore_.clear();
    }
}

std::vector<int> rt_core_siblings(int cpu) {
    std::vector<int> out;
    std::string list;
    if (cpu < 0 || !read_line(cpu_attr(cpu, "topology/thread_siblings_list"), list)) return out;
    std::stringstream ss(list);  // e.g. "6,14" or "0-1"
    for (std::string item; std::getline(ss, item, ',');) {
        const auto dash = item.find('-');
        const int lo = std::atoi(item.c_str());
        const int hi = dash == std::string::npos ? lo : std::atoi(item.c_str() + dash + 1);
        for (int c = lo; c <= hi; ++c) out.push_back(c);
    }
    return out;
}

int rt_setup_memory_and_sched(const RtSetupConfig& cfg) {
    if (cfg.disable_malloc_trim) {
        // Keep freed memory resident — avoids brk/munmap inside the RT loop.
        mallopt(M_TRIM_THRESHOLD, -1);
        mallopt(M_MMAP_MAX, 0);
    }

    if (cfg.lock_all_memory) {
        int flags = MCL_CURRENT | MCL_FUTURE;
#ifdef MCL_ONFAULT
        flags |= MCL_ONFAULT;
#endif
        if (mlockall(flags) < 0) {
            // MCL_ONFAULT may be unsupported on older kernels — retry without it.
            if (errno == EINVAL && mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
                return -1;
            } else if (errno != 0 && (flags == (MCL_CURRENT | MCL_FUTURE))) {
                return -1;
            }
        }
    }

    if (cfg.prefault_stack) {
        volatile char stack_prefault[64 * 1024];
        touch_pages(stack_prefault, sizeof(stack_prefault));
    }

    if (cfg.prefault_heap_bytes > 0) {
        void* heap = std::malloc(cfg.prefault_heap_bytes);
        if (!heap) return -1;
        touch_pages(static_cast<volatile char*>(heap), cfg.prefault_heap_bytes);
        std::free(heap);  // stays resident thanks to M_TRIM_THRESHOLD = -1
    }

    if (cfg.set_cpu_affinity && cfg.cpu_id >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<unsigned>(cfg.cpu_id), &set);
        if (sched_setaffinity(0, sizeof(set), &set) < 0) return -1;
    }

    if (cfg.cpu_latency_us >= 0) {
        const int cpu = cfg.set_cpu_affinity ? cfg.cpu_id : -1;
        auto hold = CpuLatencyHold::acquire(cfg.cpu_latency_us, cpu);
        if (!hold) return -1;
        g_setup_latency_hold = std::move(hold);
    }

    if (cfg.sched_priority > 0) {
        struct sched_param param {};
        param.sched_priority = cfg.sched_priority;
        if (sched_setscheduler(0, SCHED_FIFO, &param) < 0) {
            // Lacking CAP_SYS_NICE is the common non-fatal case; report via errno
            // and let the caller decide (tests run unprivileged).
            return -1;
        }
    }

    return 0;
}

int rt_prefault_dma_buf_view(void* mapped_view, std::size_t size) noexcept {
    if (!mapped_view || size == 0) {
        errno = EINVAL;
        return -1;
    }
    // Read-touch every page so the first RT access never faults.
    volatile const char* p = static_cast<volatile const char*>(mapped_view);
    const long page = sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096;
    volatile char sink = 0;
    for (std::size_t i = 0; i < size; i += static_cast<std::size_t>(page)) {
        sink = static_cast<char>(sink ^ p[i]);
    }
    if (size > 0) sink = static_cast<char>(sink ^ p[size - 1]);
    (void)sink;
    return 0;
}

std::uint64_t rt_now_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

}  // namespace axon
