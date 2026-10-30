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

// Caches fill, then a simulated pressure source (a fake MemAvailable and PSI) makes them give back
// only the deficit, oldest entries first, within a bounded time, and the freed pages leave the
// process. Plentiful memory with light stalls and full (stale) swap never evicts anything. The
// buffers are frame-sized and touched, as pixel storage is, so the resident set really holds them.
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

// A reusable cache registered with the ledger, holding real touched frame buffers. Newest frames
// are at the front; a smaller budget drops the oldest (least recently used) first.
class FrameCache final {
  public:
    explicit FrameCache(MemoryBudgetLedger& ledger) : ledger_(ledger) {
        ledger_.registerCache(
            this, 64 * gib, [this] { return bytes(); },
            [this](const std::size_t budget) {
                const std::lock_guard lock(mutex_);
                budget_ = budget;
                while (!frames_.empty() && frames_.size() * kFrameBytes > budget)
                    frames_.pop_back();
            });
    }
    ~FrameCache() { ledger_.unregisterCache(this); }
    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;
    void fill(const std::size_t count) {
        const std::lock_guard lock(mutex_);
        for (std::size_t index = 0; index < count; ++index) {
            Frame frame{next_++, std::make_unique<std::byte[]>(kFrameBytes)};
            std::memset(frame.pixels.get(), static_cast<int>(frame.id % 251) + 1, kFrameBytes);
            frames_.push_front(std::move(frame));
        }
    }
    [[nodiscard]] std::size_t bytes() const {
        const std::lock_guard lock(mutex_);
        return frames_.size() * kFrameBytes;
    }
    [[nodiscard]] std::size_t budget() const {
        const std::lock_guard lock(mutex_);
        return budget_;
    }
    // Ids of the retained frames, newest first.
    [[nodiscard]] std::vector<std::size_t> ids() const {
        const std::lock_guard lock(mutex_);
        std::vector<std::size_t> result;
        for (const auto& frame : frames_)
            result.push_back(frame.id);
        return result;
    }

  private:
    struct Frame final {
        std::size_t id = 0;
        std::unique_ptr<std::byte[]> pixels;
    };
    MemoryBudgetLedger& ledger_;
    mutable std::mutex mutex_;
    std::deque<Frame> frames_;
    std::size_t next_ = 0;
    std::size_t budget_ = 0;
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

MemoryPressureMonitor::Options fastOptions() {
    MemoryPressureMonitor::Options options;
    options.interval = 20ms;
    options.psiTrigger = false;
    options.counterSampler = {};
    return options;
}

// Available memory below the headroom: the caches give back only the deficit to headroom plus
// margin, oldest frames first, and the resident set drops by what they released.
void deficitOnlyOldestFirst() {
    configureHostMemoryReturn();
    provokeDynamicMmapThreshold();
    MemoryBudgetLedger ledger(64 * gib, 64 * gib);
    ledger.setRamHeadroom(2 * gib);
    FrameCache cache(ledger);
    std::atomic<std::size_t> base{40 * gib};
    // What the machine reports: a base level plus whatever the cache has released since filling.
    const auto available = [&] { return base.load() + (48 * kFrameBytes - cache.bytes()); };
    static_cast<void>(ledger.applyPressureSample({.availableBytes = base.load()}));
    cache.fill(48); // 672 MiB, frames 0 (oldest) to 47 (newest)
    const auto resident = processResidentBytes();
    auto options = fastOptions();
    options.sampler = [&] { return MachineMemorySample{.availableBytes = available()}; };
    MemoryPressureMonitor monitor(
        ledger, [&](const MachineMemorySample& sample) { ledger.applyPressureSample(sample); },
        options);
    std::this_thread::sleep_for(100ms);
    expect(cache.bytes() == 48 * kFrameBytes && monitor.deliveries() == 0,
           "a machine above the headroom evicts nothing");

    // Another program takes memory: 1.9 GiB available against a 2 GiB headroom. The deficit to
    // headroom plus the 256 MiB margin is 358 MiB: about 26 of the 48 frames, the oldest ones.
    const auto pressured = std::chrono::steady_clock::now();
    base = gib + 9 * gib / 10;
    const auto restore = 2 * gib + 256 * mib;
    expect(waitUntil([&] { return available() >= restore; }, 2000ms),
           "the caches give back the deficit within two seconds");
    const auto elapsed = std::chrono::steady_clock::now() - pressured;
    std::this_thread::sleep_for(100ms);
    const auto ids = cache.ids();
    const auto evicted = 48 - ids.size();
    const auto deficitFrames = (restore - (gib + 9 * gib / 10) + kFrameBytes - 1) / kFrameBytes;
    expect(evicted >= deficitFrames && evicted <= deficitFrames + 1,
           "only the deficit (plus one partial frame) is evicted, never the whole cache");
    expect(!ids.empty() && ids.front() == 47 && ids.back() == evicted,
           "the oldest frames went first; the newest stay resident");
    expect(waitUntil([&] { return monitor.trims() > 0; }, 2000ms),
           "the monitor returns the freed memory to the OS");
    const auto after = processResidentBytes();
    if (resident && after) {
        const auto dropped = *resident > *after ? *resident - *after : 0;
        std::cout << "PRESSURE_RSS before=" << *resident / mib << "MiB after=" << *after / mib
                  << "MiB dropped=" << dropped / mib << "MiB evictedFrames=" << evicted
                  << " evictedMs="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << '\n';
        expect(dropped >= evicted * kFrameBytes * 9 / 10,
               "at least 90% of the evicted frame bytes leave the resident set");
    }
    monitor.stop();
}

// A larger RAM headroom applies at once against the last sample, and also evicts only the
// deficit, oldest first.
void headroomChangeEvictsDeficit() {
    configureHostMemoryReturn();
    MemoryBudgetLedger ledger(64 * gib, 64 * gib);
    ledger.setRamHeadroom(2 * gib);
    FrameCache cache(ledger);
    static_cast<void>(ledger.applyPressureSample({.availableBytes = 6 * gib}));
    cache.fill(96); // 1344 MiB
    auto options = fastOptions();
    options.sampler = [&] {
        return MachineMemorySample{.availableBytes = 6 * gib + (96 * kFrameBytes - cache.bytes())};
    };
    MemoryPressureMonitor monitor(
        ledger, [&](const MachineMemorySample& sample) { ledger.applyPressureSample(sample); },
        options);
    // 6.2 GiB headroom against 6 GiB available: margin max(256 MiB, 6.2/8 GiB) = 794 MiB, so the
    // deficit to headroom plus margin is 998 MiB of the 1344 MiB held.
    const auto headroom = 6 * gib + gib / 5;
    ledger.setRamHeadroom(headroom);
    const auto kept = cache.bytes();
    const auto deficit = headroom + memoryPressureMarginBytes(headroom) - 6 * gib;
    expect(kept < 96 * kFrameBytes && 96 * kFrameBytes - kept >= deficit &&
               96 * kFrameBytes - kept < deficit + kFrameBytes,
           "a larger headroom evicts at once, but only the deficit");
    expect(!cache.ids().empty() && cache.ids().front() == 95,
           "the newest frames stay after a headroom change");
    monitor.wake();
    expect(waitUntil([&] { return monitor.trims() > 0; }, 2000ms),
           "the freed memory is returned to the OS within one interval");
    monitor.stop();
}

// The owner's machine: 38 GB available against a 6 GB headroom, steady light PSI stalls (some and
// full around 3.5%), swap full of stale zram pages and a little swap-in. None of it is caused by
// these caches: nothing is evicted and the caches keep growing. A severe "full" stall counts only
// once available memory is inside headroom plus margin, and then only the deficit is evicted.
void lightStallsAndStaleSwapNeverEvict() {
    MemoryBudgetLedger ledger(64 * gib, 64 * gib);
    ledger.setRamHeadroom(6 * gib);
    FrameCache cache(ledger);
    std::atomic<std::size_t> base{38 * gib};
    std::atomic<std::size_t> filled{0};
    // What the machine reports: a base level plus whatever the cache released since filling.
    const auto available = [&] { return base.load() + (filled.load() - cache.bytes()); };
    std::atomic<std::uint64_t> stall{0};
    std::atomic<std::uint64_t> stallPerSample{700}; // 3.5% of a 20 ms interval
    std::atomic<std::uint64_t> swapIn{0};
    auto options = fastOptions();
    options.sampler = [&] {
        return MachineMemorySample{.availableBytes = available(),
                                   .swapTotalBytes = 3 * gib,
                                   .swapUsedBytes = 3 * gib};
    };
    options.counterSampler = [&] {
        stall += stallPerSample.load();
        swapIn += 32;
        return MemoryPressureCounters{.someStallMicroseconds = stall.load(),
                                      .fullStallMicroseconds = stall.load(),
                                      .swapInPages = swapIn.load(),
                                      .swapOutPages = 0};
    };
    MemoryPressureMonitor monitor(
        ledger, [&](const MachineMemorySample& sample) { ledger.applyPressureSample(sample); },
        options);
    static_cast<void>(ledger.applyPressureSample(options.sampler()));
    cache.fill(16);
    filled = 16 * kFrameBytes;
    std::this_thread::sleep_for(200ms);
    static_cast<void>(ledger.applyPressureSample(options.sampler()));
    expect(cache.bytes() == 16 * kFrameBytes && monitor.deliveries() == 0,
           "light stalls and full stale swap with plenty available evict nothing");
    expect(cache.budget() > cache.bytes(), "and the caches may keep growing");

    // Severe full stalls (half of every interval) while memory is still plentiful: still nothing.
    stallPerSample = 10'000;
    std::this_thread::sleep_for(200ms);
    expect(cache.bytes() == 16 * kFrameBytes && monitor.deliveries() == 0,
           "a severe stall with plenty available is not these caches' doing");

    // Severe stalls with available memory inside headroom plus margin (6 GiB + 768 MiB): the caches
    // give back the deficit to headroom plus margin, oldest first, and no more.
    base = 6 * gib + 600 * mib;
    const auto floor = 6 * gib + memoryPressureMarginBytes(6 * gib);
    expect(waitUntil([&] { return cache.bytes() < 16 * kFrameBytes; }, 2000ms),
           "a severe stall inside the margin evicts");
    std::this_thread::sleep_for(150ms);
    const auto evicted = 16 * kFrameBytes - cache.bytes();
    const auto deficit = floor - (6 * gib + 600 * mib);
    expect(evicted >= deficit && evicted < deficit + kFrameBytes,
           "only the deficit to headroom plus margin, then the caches hold");
    expect(cache.ids().front() == 15, "the newest frames stay");
    monitor.stop();
}

} // namespace

int main() {
    deficitOnlyOldestFirst();
    headroomChangeEvictsDeficit();
    lightStallsAndStaleSwapNeverEvict();
    if (failures == 0)
        std::cout << "memory pressure monitor: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
