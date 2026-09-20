// Focused, bounded tests for the capacity-aware resident preview admission policy.
//
// The pure cases run with no device: configured partition, the device-fits/device-pressure/unknown
// clamp, overflow saturation, and the 24x6000x4000 RGBA8 retention arithmetic. The optional native
// owner case creates one real device on this thread, resolves its allocation budget, and proves the
// effective lease/cache plan stays aligned and never overcommits half the resolved budget. It uses
// a compact arithmetic simulation (no 24 large GPU allocations, no simultaneous worker pressure).

#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/gpu_preview_resident_capacity.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

namespace {

using bloom::render::GpuAllocationBudgetSource;
using bloom::runtime::GpuResidentCapacity;
using bloom::runtime::GpuResidentCapacitySource;
using bloom::runtime::GpuResidentConfiguredBudgets;

constexpr std::uint64_t kMib = 1024ULL * 1024ULL;
constexpr std::uint64_t kGib = 1024ULL * kMib;

int failures = 0;

void expect(const bool condition, const std::string& label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

GpuResidentCapacity knownCapacity(const std::uint64_t bytes) {
    GpuResidentCapacity capacity;
    capacity.source = GpuResidentCapacitySource::MemoryBudget;
    capacity.availableBytes = bytes;
    capacity.deviceLocalBytes = bytes;
    return capacity;
}

void testConfiguredPartition() {
    const auto plan = bloom::runtime::gpuResidentCapacityPlanConfigured(2ULL * kGib);
    expect(plan.poolBytes == 2ULL * kGib, "the configured pool is the explicit ceiling");
    expect(plan.leaseBytes == (2ULL * kGib / 5ULL) * 3ULL, "the lease share is 3/5 of the ceiling");
    expect(plan.sceneCacheBytes == 2ULL * kGib / 5ULL, "the scene share is 1/5 of the ceiling");
    expect(plan.requestBytes == 2ULL * kGib - plan.leaseBytes - plan.sceneCacheBytes,
           "the request share completes the ceiling");
    expect(plan.cacheBytes == plan.leaseBytes - plan.leaseBytes / 5ULL,
           "the cache keeps 4/5 of the lease ledger");
    expect(plan.leaseBytes > plan.cacheBytes, "the lease ledger keeps in-flight headroom");
    expect(plan.cacheEntries >= 8 && plan.leaseEntries >= plan.cacheEntries,
           "the entry bounds are positive and aligned");
    expect(!plan.resolved, "a host-configured partition is not an owner-resolved plan");

    const auto zero = bloom::runtime::gpuResidentCapacityPlanConfigured(0);
    expect(zero.poolBytes == 0 && zero.cacheBytes == 0 && zero.leaseBytes == 0 &&
               zero.sceneCacheBytes == 0 && zero.requestBytes == 0,
           "a zero ceiling yields an all-zero plan with no floor");
}

void testCapacityClamp() {
    GpuResidentConfiguredBudgets configured;
    configured.leaseBytes = 3ULL * kGib;
    configured.sceneCacheBytes = kGib;
    configured.requestBytes = kGib;

    // Plenty of room: the configured plan is preserved exactly, never raised.
    const auto roomy =
        bloom::runtime::gpuResidentCapacityPlanFor(configured, knownCapacity(16ULL * kGib));
    expect(roomy.leaseBytes == configured.leaseBytes &&
               roomy.sceneCacheBytes == configured.sceneCacheBytes &&
               roomy.requestBytes == configured.requestBytes,
           "a device with room preserves the configured sub-budgets exactly");
    expect(roomy.cacheBytes == roomy.leaseBytes - roomy.leaseBytes / 5ULL,
           "the preserved plan keeps the cache/lease alignment");

    // Tight device: each ledger scales down proportionally and the sum stays within the pool.
    const auto tight =
        bloom::runtime::gpuResidentCapacityPlanFor(configured, knownCapacity(4ULL * kGib));
    expect(tight.poolBytes == 2ULL * kGib, "a tight device caps the pool at half its budget");
    expect(tight.leaseBytes + tight.sceneCacheBytes + tight.requestBytes <= tight.poolBytes,
           "the tight plan never exceeds the pool");
    expect(tight.leaseBytes + tight.sceneCacheBytes + tight.requestBytes > 0,
           "the tight plan still admits a resident route");
    expect(tight.cacheBytes <= tight.leaseBytes, "the tight plan keeps cache <= lease");

    // Unknown device: the small safe fallback, never a host-RAM assumption.
    const auto unknown =
        bloom::runtime::gpuResidentCapacityPlanFor(configured, GpuResidentCapacity{});
    expect(unknown.resolved, "the owner-resolved path is always marked resolved");
    expect(unknown.poolBytes == 256ULL * kMib, "an unknown device uses the safe fallback pool");
    expect(unknown.poolBytes <= configured.leaseBytes,
           "the fallback never exceeds the configured bound");
    expect(!unknown.capacity.isKnown(), "an unresolved device source is not known");

    // A KNOWN live budget reporting zero free bytes is not an unresolved device: it yields no
    // resident subset and must never receive the unknown fallback.
    const auto knownZero = bloom::runtime::gpuResidentCapacityPlanFor(configured, knownCapacity(0));
    expect(knownZero.capacity.isKnown(), "a known-zero budget is still a resolved device");
    expect(knownZero.poolBytes == 0 && knownZero.cacheBytes == 0,
           "a known-zero budget yields no resident subset, never the unknown fallback");

    // An explicit zero configured ceiling stays zero even on a large device.
    GpuResidentConfiguredBudgets none;
    const auto configuredZero =
        bloom::runtime::gpuResidentCapacityPlanFor(none, knownCapacity(16ULL * kGib));
    expect(configuredZero.resolved, "an explicit-zero owner plan is still resolved");
    expect(configuredZero.poolBytes == 0 && configuredZero.cacheBytes == 0,
           "an explicit zero ceiling is respected, never floored upward");
}

void testOverflowSaturation() {
    GpuResidentConfiguredBudgets saturated;
    saturated.leaseBytes = std::numeric_limits<std::uint64_t>::max();
    saturated.sceneCacheBytes = std::numeric_limits<std::uint64_t>::max();
    saturated.requestBytes = std::numeric_limits<std::uint64_t>::max();
    const auto plan =
        bloom::runtime::gpuResidentCapacityPlanFor(saturated, knownCapacity(16ULL * kGib));
    expect(plan.poolBytes <= 8ULL * kGib, "saturated budgets stay within half the device");
    expect(plan.leaseBytes + plan.sceneCacheBytes + plan.requestBytes <= plan.poolBytes,
           "saturated budgets scale without overflowing the pool");
    expect(plan.cacheBytes <= plan.leaseBytes, "saturated budgets keep cache <= lease");
}

// The portable fallback must itself be bounded; a rounded-up estimate can never exceed the caller's
// invariant. Tested directly even where the exact 128-bit path is compiled in.
void testPortableScaledShareClamped() {
    using bloom::runtime::gpu_resident_capacity_detail::scaledShareClamped;
    const std::uint64_t huge = std::numeric_limits<std::uint64_t>::max();
    expect(scaledShareClamped(3, 2, 5) <= std::min<std::uint64_t>(3, 2),
           "the clamped share never exceeds min(value, numerator)");
    expect(scaledShareClamped(huge, huge, huge) <= huge, "a saturated share stays bounded");
    expect(scaledShareClamped(huge, 8ULL * 1024ULL * 1024ULL * 1024ULL, huge) <=
               8ULL * 1024ULL * 1024ULL * 1024ULL,
           "a share is bounded by its numerator");
    expect(scaledShareClamped(0, 5, 5) == 0 && scaledShareClamped(5, 0, 5) == 0 &&
               scaledShareClamped(5, 5, 0) == 0,
           "degenerate inputs yield zero");
}

void testAllocationBudgetMapping() {
    bloom::render::GpuAllocationBudget actual;
    actual.source = GpuAllocationBudgetSource::MemoryBudget;
    actual.available_bytes = 5ULL * kGib;
    actual.device_local_bytes = 16ULL * kGib;
    const auto mapped = bloom::runtime::gpuResidentCapacityFromAllocationBudget(actual);
    expect(mapped.source == GpuResidentCapacitySource::MemoryBudget &&
               mapped.availableBytes == 5ULL * kGib && mapped.deviceLocalBytes == 16ULL * kGib,
           "a live memory budget maps to the MemoryBudget source");

    bloom::render::GpuAllocationBudget estimated;
    estimated.source = GpuAllocationBudgetSource::DeviceLocalEstimate;
    estimated.available_bytes = 16ULL * kGib;
    const auto estimate = bloom::runtime::gpuResidentCapacityFromAllocationBudget(estimated);
    expect(estimate.source == GpuResidentCapacitySource::DeviceLocalEstimate,
           "a nominal heap estimate is labelled as an estimate");

    const auto missing = bloom::runtime::gpuResidentCapacityFromAllocationBudget(
        bloom::render::GpuAllocationBudget{});
    expect(missing.source == GpuResidentCapacitySource::Unknown && !missing.isKnown(),
           "an unavailable budget maps to Unknown with zero availability");
}

void testSixKRetention() {
    const std::uint64_t frame6k = 6000ULL * 4000ULL * 4ULL;
    const std::uint64_t retention24 = 24ULL * frame6k;
    GpuResidentConfiguredBudgets configured;
    configured.leaseBytes = 12ULL * kGib * 3ULL / 5ULL;
    configured.sceneCacheBytes = 12ULL * kGib / 5ULL;
    configured.requestBytes = 12ULL * kGib - configured.leaseBytes - configured.sceneCacheBytes;

    const auto plan =
        bloom::runtime::gpuResidentCapacityPlanFor(configured, knownCapacity(16ULL * kGib));
    expect(plan.cacheBytes >= retention24,
           "24 6K RGBA8 frames are retained when the device budget allows it");
    // Compact simulation: the entry count the cache would need for 24 real frames is admitted.
    const std::uint64_t framesFitting = plan.cacheBytes / frame6k;
    expect(framesFitting >= 24, "the compact 6K frame simulation retains at least 24 frames");

    // A device that cannot host even the configured route still refuses honestly (LRU/typed).
    const auto tiny = bloom::runtime::gpuResidentCapacityPlanFor(configured, knownCapacity(kGib));
    expect(tiny.cacheBytes < retention24, "a small device does not admit the 24-frame retention");
}

struct TestOptions final {
    std::filesystem::path loaderPath;
    bool requireDevice = false;
};

TestOptions parseOptions(const int argc, char** argv) {
    TestOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            options.loaderPath = argv[++index];
        } else if (argument == "--require-device") {
            options.requireDevice = true;
        }
    }
    return options;
}

// Native owner bootstrap: resolve the real device budget and prove the effective lease/cache plan
// is aligned and never overcommits half of it. Compact arithmetic only; no large allocations.
void testNativeOwnerBootstrap(const TestOptions& options) {
    bloom::render::GpuDeviceCreationOptions createOptions;
    createOptions.loader_path = options.loaderPath;
    auto created = bloom::render::GpuDevice::create(createOptions);
    if (!created) {
        if (options.requireDevice) {
            expect(false, "a device was required but the probe returned Unavailable");
            return;
        }
        std::cout << "SKIP: no compatible Vulkan device available: " << created.diagnostic.message
                  << '\n';
        return;
    }
    auto& device = *created.device;
    expect(device.state() == bloom::render::GpuDeviceState::Ready,
           "the native owner device is Ready");

    const auto budget = device.availableAllocationBudget();
    expect(budget.source != GpuAllocationBudgetSource::Unavailable,
           "the owner resolved a real allocation budget");
    expect(budget.available_bytes <= budget.device_local_bytes,
           "the owner available budget never exceeds DEVICE_LOCAL capacity");
    std::cout << "GPU allocation budget: source=" << static_cast<int>(budget.source)
              << " available_bytes=" << budget.available_bytes
              << " device_local_bytes=" << budget.device_local_bytes << '\n';

    const auto capacity = bloom::runtime::gpuResidentCapacityFromAllocationBudget(budget);
    GpuResidentConfiguredBudgets configured;
    configured.leaseBytes = 6ULL * kGib;
    configured.sceneCacheBytes = 2ULL * kGib;
    configured.requestBytes = 2ULL * kGib;
    const auto plan = bloom::runtime::gpuResidentCapacityPlanFor(configured, capacity);
    expect(plan.leaseBytes >= plan.cacheBytes,
           "the native effective lease ledger covers the cache sublimit");
    expect(plan.leaseBytes + plan.sceneCacheBytes + plan.requestBytes <= plan.poolBytes,
           "the native plan never overcommits the shared resident pool");
    expect(plan.poolBytes <= capacity.availableBytes / 2ULL,
           "the native pool is at most half the resolved budget");

    // A foreign-thread query is rejected without touching the driver.
    GpuResidentCapacity foreignCapacity;
    std::thread worker([&device, &foreignCapacity]() {
        foreignCapacity = bloom::runtime::gpuResidentCapacityFromAllocationBudget(
            device.availableAllocationBudget());
    });
    worker.join();
    expect(foreignCapacity.source == GpuResidentCapacitySource::Unknown &&
               foreignCapacity.availableBytes == 0,
           "a foreign-thread budget query fails closed to Unknown");
}

} // namespace

int main(int argc, char** argv) {
    const TestOptions options = parseOptions(argc, argv);
    testConfiguredPartition();
    testCapacityClamp();
    testOverflowSaturation();
    testPortableScaledShareClamped();
    testAllocationBudgetMapping();
    testSixKRetention();
    testNativeOwnerBootstrap(options);

    if (failures != 0) {
        std::cerr << "FAIL: " << failures << " resident capacity expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: resident preview capacity policy\n";
    return 0;
}
