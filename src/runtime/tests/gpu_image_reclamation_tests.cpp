// Real VMA image lifetime checks, including the production preview owner pump.
#include "gpu_image_private.hpp"
#include "gpu_image_reclamation_private.hpp"
#include "gpu_preview_display_service_private.hpp"
#include "gpu_resident_display_private.hpp"

#include <atomic>
#include <bloom/render/gpu_device.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

namespace {
using namespace bloom::render;
namespace detail = bloom::render::image_detail;
int failures = 0;
void expect(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
template <typename Predicate> bool await(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}
std::uint64_t allocationBytes(GpuDevice& device) {
    auto state = GpuRendererAccess::state(device);
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(state->allocator, &stats);
    return stats.total.statistics.allocationBytes;
}
GpuImage makeProcess(GpuDevice& device) {
    auto impl = std::make_unique<GpuImageImpl>();
    impl->state = GpuRendererAccess::state(device);
    impl->generation = impl->state->generation;
    if (!createResidentImage(*impl->state, 3, 2, *impl)) {
        return makeGpuImage(nullptr);
    }
    return makeGpuImage(std::move(impl));
}
GpuDisplayImage makeDisplay(GpuDevice& device) {
    auto impl = std::make_unique<GpuDisplayImageImpl>();
    impl->state = GpuRendererAccess::state(device);
    impl->generation = impl->state->generation;
    if (!createDisplayImage(*impl->state, 3, 2, *impl)) {
        return makeGpuDisplayImage(nullptr);
    }
    return makeGpuDisplayImage(std::move(impl));
}
template <typename Make> void checkFamily(GpuDevice& device, Make make) {
    const auto baseline = allocationBytes(device);
    const auto before = device.imageReclamationStats();
    auto state = GpuRendererAccess::state(device);
    const auto pendingBefore = state->pendingImageReclaims.load();
    auto first = make(device);
    expect(first.isValid(), "real native image allocated");
    const auto bytes = first.allocationBytes();
    expect(bytes > 0 && allocationBytes(device) == baseline + bytes,
           "cached allocation charge agrees with actual VMA allocation bytes");
    detail::setImageAdmissionLimitForTests(1);
    auto refused = make(device);
    expect(!refused.isValid() && allocationBytes(device) == baseline + bytes,
           "bounded admission refuses before any native allocation");
    expect(device.imageReclamationStats().admissionRefusals == before.admissionRefusals + 1,
           "capacity refusal is observable");
    std::thread([last = std::move(first)] {}).join();
    auto pending = device.imageReclamationStats();
    expect(pending.retiringImages == before.retiringImages + 1 &&
               pending.retiringAllocationBytes == before.retiringAllocationBytes + bytes &&
               allocationBytes(device) == baseline + bytes,
           "foreign last ownership remains pending and charged");
    expect(state->pendingImageReclaims.load() == pendingBefore + 1,
           "final release publishes exactly one exact-device pending count");
    std::thread([&device] { device.reclaimReleasedImages(); }).join();
    expect(device.imageReclamationStats().retiringImages == pending.retiringImages,
           "foreign drain cannot destroy native images");
    device.reclaimReleasedImages();
    auto after = device.imageReclamationStats();
    expect(after.retiringImages == before.retiringImages && after.liveImages == before.liveImages &&
               after.ownerReclaims == before.ownerReclaims + 1 &&
               allocationBytes(device) == baseline,
           "exact owner reclaims once and restores VMA bytes");
    device.reclaimReleasedImages();
    expect(device.imageReclamationStats().ownerReclaims == after.ownerReclaims,
           "repeated drain cannot destroy twice");
    auto recovered = make(device);
    expect(recovered.isValid(), "admission recovers after owner reclaim");
    detail::setImageAdmissionLimitForTests(0);
    recovered = make(device);
    expect(recovered.isValid() &&
               device.imageReclamationStats().liveImages == before.liveImages + 1,
           "owner move-assignment replacement releases old image exactly once");
    auto replacement = make(device);
    const auto replacementBytes = recovered.allocationBytes() + replacement.allocationBytes();
    std::thread([old = std::move(recovered), next = std::move(replacement)]() mutable {
        old = std::move(next);
    }).join();
    expect(device.imageReclamationStats().retiringImages == before.retiringImages + 2 &&
               device.imageReclamationStats().retiringAllocationBytes == replacementBytes,
           "foreign move replacement and final destructor retain both images");
    device.reclaimReleasedImages();
    expect(allocationBytes(device) == baseline, "both replacement images eventually reclaimed");
    expect(state->pendingImageReclaims.load() == pendingBefore,
           "native destruction clears pending exactly once; ordinary releases never decrement");
}
void checkOwners(const GpuDeviceCreationOptions& options, GpuDevice& first) {
    const auto before = first.imageReclamationStats();
    auto firstImage = makeProcess(first);
    std::atomic<bool> ready{false};
    std::atomic<bool> drain{false};
    std::atomic<bool> secondOk{false};
    std::thread secondOwner([&] {
        auto created = GpuDevice::create(options);
        if (!created) {
            ready.store(true);
            return;
        }
        const auto baseline = allocationBytes(*created.device);
        auto image = makeDisplay(*created.device);
        std::thread([last = std::move(image)] {}).join();
        ready.store(true);
        if (!await([&] { return drain.load(); })) {
            return;
        }
        created.device->reclaimReleasedImages();
        secondOk.store(created.device->ownershipEpoch() != first.ownershipEpoch() &&
                       allocationBytes(*created.device) == baseline);
    });
    std::thread([last = std::move(firstImage)] {}).join();
    expect(await([&] { return ready.load(); }), "owner setup meets bounded deadline");
    const auto processPending = GpuDevice::processImageReclamationStats().retiringImages;
    first.reclaimReleasedImages();
    expect(first.imageReclamationStats().retiringImages == before.retiringImages &&
               GpuDevice::processImageReclamationStats().retiringImages + 1 == processPending,
           "first exact owner leaves second device orphan intact");
    drain.store(true);
    secondOwner.join();
    expect(secondOk.load(), "second device owner reclaims its own exact generation");
}
void checkRecycledThreadIdentity(GpuDevice& device) {
    auto state = GpuRendererAccess::state(device);
    const auto baseline = allocationBytes(device);
    auto image = makeProcess(device);
    const auto bytes = image.allocationBytes();
    const auto originalToken = state->ownerLifetimeToken;
    state->ownerLifetimeToken = originalToken + 1;
    image = makeGpuImage(nullptr);
    device.reclaimReleasedImages();
    expect(allocationBytes(device) == baseline + bytes &&
               device.imageReclamationStats().retiringImages == 1,
           "matching thread id with wrong thread lifetime cannot destroy or reclaim image");
    state->ownerLifetimeToken = originalToken;
    device.reclaimReleasedImages();
    expect(allocationBytes(device) == baseline, "original exact thread lifetime reclaims image");
}
void checkDestroyCharge(const GpuDeviceCreationOptions& options, GpuDevice& competing) {
    const auto baseline = GpuDevice::processImageReclamationStats();
    detail::setImageAdmissionLimitForTests(1);
    detail::setImageDestroyPauseForTests(true);
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<bool> ready{false};
    std::thread owner([&] {
        auto created = GpuDevice::create(options);
        if (!created) {
            ready.store(true);
            return;
        }
        auto image = makeProcess(*created.device);
        bytes.store(image.allocationBytes());
        std::thread([last = std::move(image)] {}).join();
        ready.store(true);
        created.device->reclaimReleasedImages();
    });
    expect(await([&] { return ready.load(); }), "owner setup meets bounded deadline");
    expect(await([&] { return detail::imageDestroyPausedForTests() || bytes.load() == 0; }),
           "pre-destroy pause meets bounded deadline");
    expect(bytes.load() > 0, "paused owner creates a real native image");
    const auto paused = GpuDevice::processImageReclamationStats();
    auto refused = makeProcess(competing);
    expect(
        !refused.isValid() && paused.retiringImages == baseline.retiringImages + 1 &&
            paused.retiringAllocationBytes == baseline.retiringAllocationBytes + bytes.load(),
        "competing admission remains refused and native bytes charged before actual destruction");
    detail::setImageDestroyPauseForTests(false);
    owner.join();
    auto recovered = makeProcess(competing);
    expect(recovered.isValid(),
           "competing admission recovers only after native destruction finishes");
    detail::setImageAdmissionLimitForTests(0);
}
void checkUnprovedRetention(const GpuDeviceCreationOptions& options) {
    auto created = GpuDevice::create(options);
    if (!created) {
        expect(false, "unproved-retention device created");
        return;
    }
    const auto baseline = allocationBytes(*created.device);
    auto process = makeProcess(*created.device);
    auto display = makeDisplay(*created.device);
    if (!process.isValid() || !display.isValid()) {
        expect(false, "unproved-retention real images allocated");
        return;
    }
    const auto bytes = process.allocationBytes() + display.allocationBytes();
    // Retention-only seam: no submission success, retirement, or pixel parity is fabricated.
    // Clearing these flags needs the oracle's exact source proof in the following G2 slice.
    auto* processImpl = const_cast<GpuImageImpl*>(gpuImageImpl(process));
    auto* displayImpl = const_cast<GpuDisplayImageImpl*>(gpuDisplayImageImpl(display));
    processImpl->submissionUnretired = true;
    displayImpl->submissionUnretired = true;
    std::thread([last = std::move(process)] {}).join();
    std::thread([last = std::move(display)] {}).join();
    const auto pending = created.device->imageReclamationStats();
    created.device->reclaimReleasedImages();
    const auto retained = created.device->imageReclamationStats();
    expect(retained.retiringImages == 2 && retained.retiringAllocationBytes == bytes &&
               retained.ownerReclaims == pending.ownerReclaims &&
               allocationBytes(*created.device) == baseline + bytes &&
               GpuRendererAccess::state(*created.device)->pendingImageReclaims.load() == 2,
           "unproved source flags prevent both image families from reclaiming real allocations");
    const auto beforeClose = GpuDevice::processImageReclamationStats();
    created.device.reset();
    expect(GpuDevice::processImageReclamationStats().closedOwnerRetiringAllocationBytes ==
               beforeClose.closedOwnerRetiringAllocationBytes + bytes,
           "unproved source images remain charged in bounded closed-owner quarantine");
}
void checkShutdown(const GpuDeviceCreationOptions& options) {
    auto old = GpuDevice::create(options);
    auto next = GpuDevice::create(options);
    expect(old && next, "shutdown devices created");
    if (!old || !next) {
        return;
    }
    auto survivor = makeProcess(*old.device);
    auto late = makeDisplay(*old.device);
    const auto bytes = late.allocationBytes();
    const auto before = GpuDevice::processImageReclamationStats();
    *old.device = std::move(*next.device);
    expect(survivor.isValid() && late.isValid(),
           "facade move replacement preserves surviving images");
    survivor = makeGpuImage(nullptr); // Old generation allocator is still co-owned on its owner.
    std::thread([last = std::move(late)] {}).join();
    old.device->reclaimReleasedImages();
    const auto pending = GpuDevice::processImageReclamationStats();
    expect(pending.closedOwnerRetiringImages == before.closedOwnerRetiringImages + 1 &&
               pending.closedOwnerRetiringAllocationBytes ==
                   before.closedOwnerRetiringAllocationBytes + bytes,
           "late closed-generation release remains bounded and observable after move replacement");
    auto closing = GpuDevice::create(options);
    if (!closing) {
        expect(false, "destructor device created");
        return;
    }
    auto final = makeProcess(*closing.device);
    const auto finalBytes = final.allocationBytes();
    closing.device.reset();
    expect(final.isValid(), "facade destructor preserves surviving image validity");
    std::thread([last = std::move(final)] {}).join();
    expect(GpuDevice::processImageReclamationStats().closedOwnerRetiringAllocationBytes ==
               pending.closedOwnerRetiringAllocationBytes + finalBytes,
           "late release after destructor remains explicitly charged");
}
} // namespace
int main(int argc, char** argv) {
    GpuDeviceCreationOptions options;
    bool strict = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--loader" && i + 1 < argc) {
            options.loader_path = argv[++i];
        } else if (arg == "--require-device") {
            strict = true;
        } else {
            return 2;
        }
    }
    auto created = GpuDevice::create(options);
    if (!created) {
        std::cout << "SKIP: native image owner reclamation device unavailable\n";
        return strict ? 1 : 77;
    }
    checkFamily(*created.device, makeProcess);
    checkFamily(*created.device, makeDisplay);
    checkOwners(options, *created.device);
    checkDestroyCharge(options, *created.device);
    checkRecycledThreadIdentity(*created.device);
    auto core = std::make_shared<bloom::runtime::detail::PreviewDisplayServiceCore>();
    core->device = std::move(created.device);
    const auto baseline = allocationBytes(*core->device);
    auto pumpImage = makeProcess(*core->device);
    std::thread([last = std::move(pumpImage)] {}).join();
    bloom::runtime::detail::pumpServicePresentation(core);
    expect(allocationBytes(*core->device) == baseline,
           "production preview owner pump reclaims final image release without presentation work");
    checkShutdown(options);
    checkUnprovedRetention(options);
    auto cleanState = GpuRendererAccess::state(*core->device);
    const auto cleanScans = cleanState->imageReclamationScans.load();
    for (int repeat = 0; repeat < 5; ++repeat) {
        core->device->reclaimReleasedImages();
    }
    expect(cleanState->pendingImageReclaims.load() == 0 &&
               cleanState->imageReclamationScans.load() == cleanScans &&
               GpuDevice::processImageReclamationStats().closedOwnerRetiringImages >= 4,
           "unrelated closed retiring generations do not make a clean owner scan global stores");
    if (failures != 0) {
        return 1;
    }
    std::cout << "PASS: native image owner reclamation, capacity, devices, pump and shutdown\n";
    return 0;
}
