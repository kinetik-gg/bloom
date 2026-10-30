#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/memory_pressure_monitor.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

// Caches fill, then a simulated pressure source (a fake MemAvailable) makes them shrink within a
// bounded time and the freed pages leave the process. The buffers are frame-sized and touched, as
// pixel storage is, so the resident set really holds them.
namespace {
using namespace bloom::runtime;
using namespace std::chrono_literals;
constexpr std::size_t mib = std::size_t{1024} * 1024;
constexpr std::size_t gib = 1024 * mib;
constexpr std::size_t kFrameBytes = 14 * mib; // A 2048x858 half-float RGBA frame.
int failures = 0;

void expect(const bool value, const std::string_view message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// A reusable cache registered with the ledger, holding real touched frame buffers.
class FrameCache final {
  public:
    explicit FrameCache(MemoryBudgetLedger& ledger) : ledger_(ledger) {
        ledger_.registerCache(
            this, 64 * gib,
            [this] {
                const std::lock_guard lock(mutex_);
                return frames_.size() * kFrameBytes;
            },
            [this](const std::size_t bytes) {
                const std::lock_guard lock(mutex_);
                while (!frames_.empty() && frames_.size() * kFrameBytes > bytes)
                    frames_.pop_back();
            });
    }
    ~FrameCache() { ledger_.unregisterCache(this); }
    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;
    void fill(const std::size_t count) {
        const std::lock_guard lock(mutex_);
        for (std::size_t index = 0; index < count; ++index) {
            auto frame = std::make_unique<std::byte[]>(kFrameBytes);
            std::memset(frame.get(), static_cast<int>(index % 251) + 1, kFrameBytes);
            frames_.push_front(std::move(frame));
        }
    }
    [[nodiscard]] std::size_t bytes() const {
        const std::lock_guard lock(mutex_);
        return frames_.size() * kFrameBytes;
    }

  private:
    MemoryBudgetLedger& ledger_;
    mutable std::mutex mutex_;
    std::deque<std::unique_ptr<std::byte[]>> frames_;
};

template <class Predicate> bool waitUntil(Predicate done, const std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(5ms);
    return done();
}

// glibc's dynamic threshold: freeing a 30 MiB block would move later 14 MiB frames onto the heap,
// where free() keeps them resident. configureHostMemoryReturn fixes the threshold first.
void provokeDynamicMmapThreshold() {
    auto block = std::make_unique<std::byte[]>(30 * mib);
    std::memset(block.get(), 1, 30 * mib);
    block.reset();
}

void pressureEvictsAndReturnsMemory() {
    configureHostMemoryReturn();
    provokeDynamicMmapThreshold();
    MemoryBudgetLedger ledger(64 * gib, 64 * gib);
    ledger.setRamHeadroom(2 * gib);
    std::atomic<std::size_t> available{40 * gib};
    std::atomic<std::size_t> swapped{0};
    FrameCache cache(ledger);
    static_cast<void>(ledger.applyPressureSample({.availableBytes = available.load()}));
    cache.fill(48); // 672 MiB
    const auto resident = processResidentBytes();
    MemoryPressureMonitor::Options options;
    options.interval = 20ms;
    options.psiTrigger = false;
    options.stallSampler = {};
    options.sampler = [&] {
        return MachineMemorySample{.availableBytes = available.load(),
                                   .processSwappedBytes = swapped.load()};
    };
    MemoryPressureMonitor monitor(
        ledger, [&](const MachineMemorySample& sample) { ledger.applyPressureSample(sample); },
        options);
    std::this_thread::sleep_for(100ms);
    expect(cache.bytes() == 48 * kFrameBytes, "no pressure, no eviction");
    expect(monitor.deliveries() == 0, "a machine above the headroom delivers nothing");

    // Another process takes memory: 1.5 GiB available, 0.5 GiB short of the headroom. The cache
    // must give back the deficit plus the 512 MiB margin: 1 GiB of 672 MiB, so everything.
    const auto pressured = std::chrono::steady_clock::now();
    available = gib + gib / 2;
    expect(waitUntil([&] { return cache.bytes() == 0; }, 2000ms),
           "the cache evicts within two seconds of the pressure sample");
    const auto evictedAfter = std::chrono::steady_clock::now() - pressured;
    expect(waitUntil([&] { return monitor.trims() > 0; }, 2000ms),
           "the monitor returns the freed memory to the OS");
    const auto after = processResidentBytes();
    if (resident && after) {
        const auto dropped = *resident > *after ? *resident - *after : 0;
        std::cout << "PRESSURE_RSS before=" << *resident / mib << "MiB after=" << *after / mib
                  << "MiB dropped=" << dropped / mib << "MiB evictedMs="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(evictedAfter).count()
                  << '\n';
        expect(dropped >= 48 * kFrameBytes * 9 / 10,
               "at least 90% of the evicted frame bytes leave the resident set");
    }

    // Our own pages in swap: plenty available, but the caches' swapped pages are not.
    available = 40 * gib;
    std::this_thread::sleep_for(60ms);
    static_cast<void>(ledger.applyPressureSample({.availableBytes = available.load()}));
    cache.fill(16);
    swapped = 39 * gib;
    expect(waitUntil([&] { return cache.bytes() == 0; }, 2000ms),
           "a cache swapped out under memory pressure is evicted, not kept in swap");
    swapped = 0;
    monitor.stop();
}

void headroomChangeEvictsAtOnce() {
    configureHostMemoryReturn();
    MemoryBudgetLedger ledger(64 * gib, 64 * gib);
    ledger.setRamHeadroom(2 * gib);
    FrameCache cache(ledger);
    static_cast<void>(ledger.applyPressureSample({.availableBytes = 6 * gib}));
    cache.fill(32); // 448 MiB
    const auto resident = processResidentBytes();
    MemoryPressureMonitor::Options options;
    options.interval = 20ms;
    options.psiTrigger = false;
    options.stallSampler = {};
    options.sampler = [] { return MachineMemorySample{.availableBytes = 6 * gib}; };
    MemoryPressureMonitor monitor(
        ledger, [&](const MachineMemorySample& sample) { ledger.applyPressureSample(sample); },
        options);
    // The owner raises the headroom preference above what is available.
    ledger.setRamHeadroom(8 * gib);
    expect(cache.bytes() == 0, "a larger headroom evicts synchronously, before any new sample");
    monitor.wake();
    expect(waitUntil([&] { return monitor.trims() > 0; }, 2000ms),
           "the freed memory is returned to the OS within one interval");
    const auto after = processResidentBytes();
    if (resident && after) {
        const auto dropped = *resident > *after ? *resident - *after : 0;
        std::cout << "HEADROOM_RSS before=" << *resident / mib << "MiB after=" << *after / mib
                  << "MiB dropped=" << dropped / mib << "MiB\n";
        expect(dropped >= 32 * kFrameBytes * 9 / 10,
               "the headroom change returns the evicted bytes to the OS");
    }
    monitor.stop();
}

void stallStopsGrowth() {
    MemoryBudgetLedger ledger(64 * gib, 64 * gib);
    ledger.setRamHeadroom(2 * gib);
    std::size_t budget = 0;
    ledger.registerCache(
        &budget, 64 * gib, [] { return std::size_t{0}; },
        [&](const std::size_t bytes) { budget = bytes; });
    std::atomic<std::uint64_t> stall{0};
    std::atomic<std::size_t> available{gib};
    MemoryPressureMonitor::Options options;
    options.interval = 20ms;
    options.psiTrigger = false;
    options.sampler = [&] { return MachineMemorySample{.availableBytes = available.load()}; };
    // Half of every interval spent stalled on memory.
    options.stallSampler = [&] { return std::optional<std::uint64_t>{stall += 10'000}; };
    std::atomic<bool> sawStall{false};
    MemoryPressureMonitor monitor(
        ledger,
        [&](const MachineMemorySample& sample) {
            if (sample.memoryStall)
                sawStall = true;
            ledger.applyPressureSample(sample);
        },
        options);
    expect(waitUntil([&] { return sawStall.load(); }, 2000ms),
           "PSI stall time beyond the fraction marks the sample as stalled");
    monitor.stop();
    ledger.unregisterCache(&budget);
}

} // namespace

int main() {
    pressureEvictsAndReturnsMemory();
    headroomChangeEvictsAtOnce();
    stallStopsGrowth();
    if (failures == 0)
        std::cout << "memory pressure monitor: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
