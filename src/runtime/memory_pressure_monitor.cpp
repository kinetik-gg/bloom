#include <bloom/runtime/memory_pressure_monitor.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace bloom::runtime {
namespace {

#if defined(__GLIBC__)
// Blocks at or above this size are mmapped and unmapped on free. Small enough that a quarter-size
// 2K preview frame qualifies, large enough that ordinary small-object churn stays on the heap.
inline constexpr int kFixedMmapThresholdBytes = 1024 * 1024;
#endif

#if defined(__linux__)
// Wake when tasks stall on memory for 100 ms within a 2 s window. Unprivileged PSI triggers need a
// window that is a multiple of two seconds.
inline constexpr std::string_view kPsiTrigger = "some 100000 2000000";

[[nodiscard]] int openPsiTrigger() noexcept {
    const int descriptor = ::open("/proc/pressure/memory", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0)
        return -1;
    if (::write(descriptor, kPsiTrigger.data(), kPsiTrigger.size() + 1) < 0) {
        ::close(descriptor);
        return -1;
    }
    return descriptor;
}
#endif

} // namespace

void configureHostMemoryReturn() noexcept {
#if defined(__GLIBC__)
    static const bool configured = [] {
        // Setting either value disables glibc's dynamic adjustment of both.
        static_cast<void>(mallopt(M_MMAP_THRESHOLD, kFixedMmapThresholdBytes));
        static_cast<void>(mallopt(M_TRIM_THRESHOLD, 2 * kFixedMmapThresholdBytes));
        return true;
    }();
    static_cast<void>(configured);
#endif
}

void releaseFreedHostMemory() noexcept {
#if defined(__GLIBC__)
    static_cast<void>(malloc_trim(0));
#endif
}

MemoryPressureCounters memoryPressureCounters() noexcept {
    MemoryPressureCounters counters;
#if defined(__linux__)
    try {
        const auto parse = [](const std::string_view text) -> std::optional<std::uint64_t> {
            std::uint64_t value = 0;
            if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{})
                return std::nullopt;
            return value;
        };
        std::ifstream pressure("/proc/pressure/memory");
        std::string line;
        while (std::getline(pressure, line)) {
            const auto at = line.find("total=");
            if (at == std::string::npos)
                continue;
            const auto total = parse(std::string_view(line).substr(at + 6));
            if (line.starts_with("some "))
                counters.someStallMicroseconds = total;
            else if (line.starts_with("full "))
                counters.fullStallMicroseconds = total;
        }
        std::ifstream vmstat("/proc/vmstat");
        while (std::getline(vmstat, line)) {
            if (line.starts_with("pswpin "))
                counters.swapInPages = parse(std::string_view(line).substr(7));
            else if (line.starts_with("pswpout "))
                counters.swapOutPages = parse(std::string_view(line).substr(8));
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // Best effort: an unreadable counter is reported as unknown.
    }
#endif
    return counters;
}

std::optional<std::size_t> processResidentBytes() noexcept {
#if defined(__linux__)
    try {
        std::ifstream statm("/proc/self/statm");
        std::uint64_t size = 0, resident = 0;
        if (!(statm >> size >> resident))
            return std::nullopt;
        const auto page = ::sysconf(_SC_PAGE_SIZE);
        if (page <= 0)
            return std::nullopt;
        return static_cast<std::size_t>(resident) * static_cast<std::size_t>(page);
    } catch (...) {
        return std::nullopt;
    }
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0)
        return std::nullopt;
    return static_cast<std::size_t>(counters.WorkingSetSize);
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), // NOLINT(*-reinterpret-cast)
                  &count) != KERN_SUCCESS)
        return std::nullopt;
    return static_cast<std::size_t>(info.resident_size);
#else
    return std::nullopt;
#endif
}

MemoryPressureMonitor::MemoryPressureMonitor(MemoryBudgetLedger& ledger, Deliver deliver)
    : MemoryPressureMonitor(ledger, std::move(deliver), Options{}) {}

MemoryPressureMonitor::MemoryPressureMonitor(MemoryBudgetLedger& ledger, Deliver deliver,
                                             Options options)
    : ledger_(ledger), deliver_(std::move(deliver)), options_(std::move(options)) {
    configureHostMemoryReturn();
    thread_ = std::thread([this] { run(); });
}

MemoryPressureMonitor::~MemoryPressureMonitor() { stop(); }

void MemoryPressureMonitor::stop() noexcept {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wakeCondition_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
        thread_.join();
}

void MemoryPressureMonitor::wake() noexcept {
    {
        const std::lock_guard lock(mutex_);
        woken_ = true;
    }
    wakeCondition_.notify_all();
}

bool MemoryPressureMonitor::waitForNextSample(const int psiDescriptor) {
#if defined(__linux__)
    if (psiDescriptor >= 0) {
        // Wait on the PSI trigger in short slices so stop() and wake() stay prompt without a
        // second descriptor; the slices are bounded by the sampling interval.
        constexpr auto slice = std::chrono::milliseconds(25);
        const auto deadline = std::chrono::steady_clock::now() + options_.interval;
        while (true) {
            {
                const std::lock_guard lock(mutex_);
                if (stopping_)
                    return false;
                if (std::exchange(woken_, false))
                    return true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return true;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            pollfd entry{.fd = psiDescriptor, .events = POLLPRI, .revents = 0};
            const int ready =
                ::poll(&entry, 1, static_cast<int>(std::min(remaining, slice).count()));
            if (ready > 0 && (entry.revents & POLLPRI) != 0)
                return true;
            if (ready > 0 && (entry.revents & (POLLERR | POLLNVAL)) != 0)
                break; // The trigger went away; fall back to the timed wait.
        }
    }
#else
    static_cast<void>(psiDescriptor);
#endif
    std::unique_lock lock(mutex_);
    wakeCondition_.wait_for(lock, options_.interval, [this] { return stopping_ || woken_; });
    woken_ = false;
    return !stopping_;
}

void MemoryPressureMonitor::run() {
    int psiDescriptor = -1;
#if defined(__linux__)
    if (options_.psiTrigger)
        psiDescriptor = openPsiTrigger();
#endif
    std::uint64_t pageBytes = options_.pageBytes;
#if defined(__linux__)
    if (pageBytes == 0)
        pageBytes = static_cast<std::uint64_t>(std::max(::sysconf(_SC_PAGE_SIZE), 4096L));
#endif
    if (pageBytes == 0)
        pageBytes = 4096;
    std::optional<MemoryPressureCounters> last;
    auto lastAt = std::chrono::steady_clock::now();
    std::size_t releasedPending = 0;
    while (waitForNextSample(psiDescriptor)) {
        try {
            auto sample = options_.sampler ? options_.sampler() : MachineMemorySample{};
            samples_.fetch_add(1, std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now();
            if (options_.counterSampler) {
                const auto counters = options_.counterSampler();
                if (last && now > lastAt) {
                    const auto micros =
                        std::chrono::duration<double, std::micro>(now - lastAt).count();
                    const auto delta = [](const std::optional<std::uint64_t>& current,
                                          const std::optional<std::uint64_t>& previous) {
                        return current && previous && *current >= *previous
                                   ? static_cast<double>(*current - *previous)
                                   : 0.0;
                    };
                    const auto swapped = (delta(counters.swapInPages, last->swapInPages) +
                                          delta(counters.swapOutPages, last->swapOutPages)) *
                                         static_cast<double>(pageBytes);
                    const bool stalled =
                        delta(counters.someStallMicroseconds, last->someStallMicroseconds) >=
                            options_.someStallFraction * micros ||
                        delta(counters.fullStallMicroseconds, last->fullStallMicroseconds) >=
                            options_.fullStallFraction * micros ||
                        swapped >= static_cast<double>(options_.swapBytesPerSecond) * micros / 1e6;
                    sample.memoryStall = sample.memoryStall || stalled;
                }
                last = counters;
                lastAt = now;
            }
            if (ledger_.pressureDeficit(sample) > 0 && deliver_) {
                deliveries_.fetch_add(1, std::memory_order_relaxed);
                deliver_(sample);
            }
            // Evictions applied on any thread since the last pass (this delivery, a previous one,
            // a headroom change or a process sample): give the freed pages back to the OS.
            const auto released = ledger_.takeReleasedBytes();
            releasedPending = released > std::numeric_limits<std::size_t>::max() - releasedPending
                                  ? std::numeric_limits<std::size_t>::max()
                                  : releasedPending + released;
            if (releasedPending >= options_.trimThresholdBytes) {
                releaseFreedHostMemory();
                trims_.fetch_add(1, std::memory_order_relaxed);
                releasedPending = 0;
            }
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // One failed sample must not end pressure monitoring; the next interval retries.
        }
    }
#if defined(__linux__)
    if (psiDescriptor >= 0)
        ::close(psiDescriptor);
#endif
}

} // namespace bloom::runtime
