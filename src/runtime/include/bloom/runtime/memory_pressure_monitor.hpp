#pragma once

#include <bloom/runtime/memory_budget_ledger.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

// Reacting to memory pressure between process samples. Reusable caches fill memory beyond the RAM
// headroom; when another process needs it back, waiting for the next five-second process sample
// (or for the OS to swap the caches out) is too late. The monitor samples the machine on its own
// thread at a short interval, wakes at once on a Linux PSI trigger, and hands any sample that
// leaves the machine short of the headroom to its owner, which applies it to the ledger so the
// caches evict right away. Freed cache memory is then returned to the OS.
namespace bloom::runtime {

// Makes freed large blocks go back to the OS. glibc's dynamic mmap threshold grows to the size of
// the largest freed block (up to 32 MiB), after which frame-sized buffers come from heap arenas
// and stay resident after free. A fixed threshold keeps every pixel/frame buffer an mmap that
// free() unmaps. Process-wide and idempotent; elsewhere a no-op (the macOS and Windows allocators
// already return large blocks to the OS on free).
void configureHostMemoryReturn() noexcept;
// Returns free heap pages to the OS after a bulk eviction (glibc malloc_trim). Not on the UI
// thread: it can walk large arenas. A no-op where the allocator has no such call.
void releaseFreedHostMemory() noexcept;
// Cumulative microseconds some task stalled on memory (Linux /proc/pressure/memory), if known.
[[nodiscard]] std::optional<std::uint64_t> memoryStallMicroseconds() noexcept;
// The process's resident set in bytes, if the platform reports it (diagnostics and tests).
[[nodiscard]] std::optional<std::size_t> processResidentBytes() noexcept;

class MemoryPressureMonitor final {
  public:
    using Sampler = std::function<MachineMemorySample()>;
    using StallSampler = std::function<std::optional<std::uint64_t>()>;
    // Called on the monitor thread with a sample that leaves the machine short of the headroom.
    // The owner applies it with MemoryBudgetLedger::applyPressureSample on the thread that owns
    // the registered caches' callbacks.
    using Deliver = std::function<void(const MachineMemorySample&)>;

    struct Options final {
        std::chrono::milliseconds interval{250};
        Sampler sampler = machineMemorySample;
        StallSampler stallSampler = memoryStallMicroseconds;
        // Stall share of wall time between two samples that counts as a memory stall.
        double stallFraction = 0.10;
        // Bytes released by evictions before the monitor returns free heap to the OS.
        std::size_t trimThresholdBytes = std::size_t{32} * 1024U * 1024U;
        // Use a Linux PSI trigger to wake before the interval elapses.
        bool psiTrigger = true;
    };

    MemoryPressureMonitor(MemoryBudgetLedger& ledger, Deliver deliver, Options options);
    MemoryPressureMonitor(MemoryBudgetLedger& ledger, Deliver deliver);
    ~MemoryPressureMonitor();
    MemoryPressureMonitor(const MemoryPressureMonitor&) = delete;
    MemoryPressureMonitor& operator=(const MemoryPressureMonitor&) = delete;

    // Joins the thread; no delivery starts after it returns.
    void stop() noexcept;
    // Samples now instead of at the next interval (a headroom change, for example).
    void wake() noexcept;
    [[nodiscard]] std::uint64_t deliveries() const noexcept { return deliveries_.load(); }
    [[nodiscard]] std::uint64_t trims() const noexcept { return trims_.load(); }
    [[nodiscard]] std::uint64_t samples() const noexcept { return samples_.load(); }

  private:
    void run();
    [[nodiscard]] bool waitForNextSample(int psiDescriptor);

    MemoryBudgetLedger& ledger_;
    Deliver deliver_;
    Options options_;
    std::mutex mutex_;
    std::condition_variable wakeCondition_;
    bool stopping_ = false;
    bool woken_ = false;
    std::atomic<std::uint64_t> deliveries_{0};
    std::atomic<std::uint64_t> trims_{0};
    std::atomic<std::uint64_t> samples_{0};
    std::thread thread_;
};

} // namespace bloom::runtime
