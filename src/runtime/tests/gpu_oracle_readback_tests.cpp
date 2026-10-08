// Real production images/submissions; timeout seams hide observation, never fake retirement.
#include "gpu_composite_native_support.hpp"
#include "gpu_image_private.hpp"
#include "gpu_oracle_readback_private.hpp"
#include "gpu_preview_display_service_private.hpp"
#include "gpu_resident_display_private.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
using namespace bloom::render;
namespace oracle = bloom::render::oracle_detail;
namespace proof = bloom::render::composite_proof;
constexpr std::uint64_t budget = 1024ULL * 1024ULL;
std::atomic<int> failures{0};
void expect(bool condition, std::string_view message) {
    if (!condition) {
        failures.fetch_add(1);
        std::cerr << "FAIL: " << message << '\n';
    }
}
template <typename Predicate> bool await(Predicate&& predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}
std::uint64_t bytes(GpuDevice& device) {
    const auto state = GpuRendererAccess::state(device);
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(state->allocator, &stats);
    return stats.total.statistics.allocationBytes;
}
GpuImage process(GpuDevice& device) {
    auto producer = GpuSolid::create(device);
    if (!producer) {
        return makeGpuImage(nullptr);
    }
    auto result = proof::solid(*producer.solid, {0.25, 0.5, 0.75, 1.0}, proof::window(0, 0, 8, 4),
                               proof::window(0, 0, 8, 4), bloom::core::PixelAspectRatio::square());
    return result ? std::move(*result) : makeGpuImage(nullptr);
}
GpuDisplayImage display(GpuDevice& device) {
    auto source = std::make_shared<const GpuImage>(process(device));
    auto producer = GpuResidentDisplay::create(device);
    if (!source->isValid() || !producer ||
        producer.display->begin(source, budget).code != GpuResidentDisplayDiagnosticCode::None) {
        return makeGpuDisplayImage(nullptr);
    }
    GpuResidentDisplayPollResult polled = GpuResidentDisplayPollResult::Pending;
    const bool completed = await([&] {
        polled = producer.display->poll();
        return polled != GpuResidentDisplayPollResult::Pending;
    });
    if (!completed || polled != GpuResidentDisplayPollResult::Ready) {
        return makeGpuDisplayImage(nullptr);
    }
    return producer.display->takeImage();
}
auto fullProcess(const GpuImage& source, std::uint64_t limit = budget) {
    return readbackResidentImage(source, limit);
}
auto fullDisplay(const GpuDisplayImage& source, std::uint64_t limit = budget) {
    return readbackResidentDisplayImage(source, limit);
}
auto sparseDisplay(const GpuDisplayImage& source, std::uint64_t limit = budget) {
    const std::array<ImagePixelCoordinate, 2> coordinates{{{0, 0}, {7, 3}}};
    return readbackResidentDisplayImageSparse(source, coordinates, limit);
}
template <typename Make, typename Read> void family(GpuDevice& device, Make make, Read read) {
    const auto initial = bytes(device);
    auto image = make(device);
    expect(image.isValid(), "production source image exists");
    if (!image.isValid()) {
        return;
    }
    const auto sourceBytes = image.allocationBytes();
    const auto baseline = bytes(device);
    const auto state = GpuRendererAccess::state(device);
    const auto successful = read(image, budget);
    expect(successful.hasValue() && !successful.pixels.empty(),
           "initial real pixel oracle succeeds");
    std::thread([&] {
        expect(!read(image, budget).hasValue(),
               "foreign thread readback refuses without native work");
    }).join();
    oracle::setAdmissionRefusedForTests(true);
    expect(!read(image, budget).hasValue() && bytes(device) == baseline &&
               oracle::stats(state.get()).reservations == 0,
           "pre-allocation admission refusal returns no native allocation");
    oracle::setAdmissionRefusedForTests(false);
    expect(!read(image, 0).hasValue() && bytes(device) == baseline,
           "requested byte budget refuses before allocation");
    // Sparse payload is smaller than native allocation granularity on this device. Test actual
    // allocation refusal when observed; never assume the device's granularity.
    for (int repeat = 0; repeat < 3; ++repeat) {
        oracle::setTimeoutForTests(true);
        oracle::setRetirementDeferredForTests(true);
        const auto unknown = read(image, budget);
        const auto retained = oracle::stats(state.get());
        expect(!unknown.hasValue() && unknown.pixels.empty() && retained.quarantined == 1 &&
                   retained.stagingBytes > 0 && bytes(device) == baseline + retained.stagingBytes,
               "real submitted timeout publishes no pixels and charges actual VMA staging");
        expect(!read(image, budget).hasValue() &&
                   oracle::stats(state.get()).stagingBytes == retained.stagingBytes &&
                   bytes(device) == baseline + retained.stagingBytes,
               "unproved retry neither overwrites quarantine nor allocates");
        std::thread([&device] { device.reclaimReleasedImages(); }).join();
        expect(oracle::stats(state.get()).quarantined == 1, "foreign owner cannot retire oracle");
        oracle::setRetirementDeferredForTests(false);
        expect(await([&] {
                   device.reclaimReleasedImages();
                   return oracle::stats(state.get()).reservations == 0;
               }),
               "production owner pump proves real fence retirement");
        expect(bytes(device) == baseline,
               "real retirement destroys exactly the staging allocation");
        const auto retry = read(image, budget);
        expect(retry.hasValue() && retry.pixels == successful.pixels,
               "recovered exact source retries with unchanged pixels");
        const auto actualBytes = retained.stagingBytes;
        if (repeat == 0 && actualBytes > successful.pixels.size() * sizeof(successful.pixels[0])) {
            expect(!read(image, actualBytes - 1).hasValue() && bytes(device) == baseline &&
                       oracle::stats(state.get()).reservations == 0,
                   "actual VMA staging size enforces budget and returns admission");
        }
    }
    oracle::setTimeoutForTests(true);
    expect(!read(image, budget).hasValue(), "retry-only fixture genuinely submits before timeout");
    expect(state->device.getDispatcher()->vkDeviceWaitIdle(static_cast<VkDevice>(*state->device)) ==
               VK_SUCCESS,
           "retry-only fixture establishes genuine driver completion");
    const auto directRetry = read(image, budget);
    expect(directRetry.hasValue() && directRetry.pixels == successful.pixels &&
               oracle::stats(state.get()).reservations == 0 && bytes(device) == baseline,
           "same-source readback itself retires prior oracle before checking the source pin");
    oracle::setTimeoutForTests(true);
    oracle::setRetirementDeferredForTests(true);
    expect(!read(image, budget).hasValue(), "last-release fixture submits real oracle");
    const auto retainedBytes = oracle::stats(state.get()).stagingBytes;
    std::thread([last = std::move(image)] {}).join();
    expect(device.imageReclamationStats().retiringImages == 1 &&
               device.imageReclamationStats().retiringAllocationBytes == sourceBytes &&
               bytes(device) == baseline + retainedBytes,
           "foreign final release keeps exact source and staging alive");
    device.reclaimReleasedImages();
    expect(device.imageReclamationStats().retiringImages == 1,
           "owner pump retains image while retirement observation is unproved");
    oracle::setRetirementDeferredForTests(false);
    expect(await([&] {
               device.reclaimReleasedImages();
               return oracle::stats(state.get()).reservations == 0 &&
                      device.imageReclamationStats().retiringImages == 0;
           }),
           "owner pump retires oracle and exact orphan source without manual proof helper");
    expect(bytes(device) == initial, "source plus staging return to actual VMA baseline");
}
void twoOwners(const GpuDeviceCreationOptions& options, GpuDevice& first) {
    auto image = process(first);
    oracle::setTimeoutForTests(true);
    oracle::setRetirementDeferredForTests(true);
    expect(!fullProcess(image).hasValue(), "first owner has real quarantined transfer");
    std::atomic<bool> ready{false};
    std::atomic<bool> finish{false};
    std::thread second([&] {
        auto created = GpuDevice::create(options);
        if (!created) {
            expect(false, "second real owner device exists");
            ready.store(true);
            return;
        }
        auto secondImage = process(*created.device);
        oracle::setTimeoutForTests(true);
        oracle::setRetirementDeferredForTests(true);
        expect(!fullProcess(secondImage).hasValue() && oracle::stats().quarantined == 2,
               "two owner threads can retain same-family submissions without overwrite");
        ready.store(true);
        expect(await([&] { return finish.load(); }),
               "second owner receives bounded release signal");
        expect(oracle::stats(GpuRendererAccess::state(*created.device).get()).quarantined == 1,
               "first owner drain cannot release second owner's quarantine");
        oracle::setRetirementDeferredForTests(false);
        expect(await([&] {
                   created.device->reclaimReleasedImages();
                   return oracle::stats(GpuRendererAccess::state(*created.device).get())
                              .reservations == 0;
               }),
               "second owner retires only its real fence");
        expect(fullProcess(secondImage).hasValue(), "second owner admission recovers");
    });
    expect(await([&] { return ready.load(); }), "two-owner setup meets bounded deadline");
    auto third = GpuDevice::create(options);
    expect(static_cast<bool>(third), "third allocator exists for aggregate ceiling test");
    if (third) {
        auto thirdImage = process(*third.device);
        const auto before = bytes(*third.device);
        expect(thirdImage.isValid() && !fullProcess(thirdImage).hasValue() &&
                   bytes(*third.device) == before &&
                   oracle::stats().reservations == oracle::kCapacity,
               "aggregate two-slot ceiling refuses third owner before staging allocation");
    }
    oracle::setRetirementDeferredForTests(false);
    expect(await([&] {
               first.reclaimReleasedImages();
               return oracle::stats(GpuRendererAccess::state(first).get()).reservations == 0;
           }),
           "first owner retires own submission");
    finish.store(true);
    second.join();
    expect(fullProcess(image).hasValue() && oracle::stats().reservations == 0,
           "both exact owners recover independently");
}
void exactProof(GpuDevice& device) {
    auto image = process(device);
    auto* source = const_cast<GpuImageImpl*>(gpuImageImpl(image));
    expect(source != nullptr, "exact proof source exists");
    if (source == nullptr) {
        return;
    }
    oracle::setTimeoutForTests(true);
    expect(!fullProcess(image).hasValue(), "exact proof fixture submits real work");
    const auto state = GpuRendererAccess::state(device);
    expect(state->device.getDispatcher()->vkDeviceWaitIdle(static_cast<VkDevice>(*state->device)) ==
               VK_SUCCESS,
           "real device completion established for stale-proof rejection");
    const auto token = source->oracleReadbackToken;
    const auto slot = source->reclamationSlot;
    source->oracleReadbackToken = token + 1;
    device.reclaimReleasedImages();
    expect(source->submissionUnretired.load() && oracle::stats(state.get()).quarantined == 1,
           "stale oracle token cannot clear exact source pin even with completed fence");
    source->oracleReadbackToken = token;
    source->reclamationSlot = slot + 1;
    device.reclaimReleasedImages();
    expect(source->submissionUnretired.load() && oracle::stats(state.get()).quarantined == 1,
           "reused source slot identity cannot clear pin");
    source->reclamationSlot = slot;
    const auto lifetime = state->ownerLifetimeToken;
    state->ownerLifetimeToken = lifetime + 1;
    device.reclaimReleasedImages();
    expect(source->submissionUnretired.load() && oracle::stats(state.get()).quarantined == 1,
           "recycled thread identity cannot query native retirement");
    state->ownerLifetimeToken = lifetime;
    device.reclaimReleasedImages();
    expect(!source->submissionUnretired.load() && oracle::stats(state.get()).reservations == 0 &&
               fullProcess(image).hasValue(),
           "exact proof restores source admission only once");
}
void closedOwner(const GpuDeviceCreationOptions& options) {
    auto created = GpuDevice::create(options);
    expect(static_cast<bool>(created), "closing owner fixture creates real device");
    if (!created) {
        return;
    }
    auto image = process(*created.device);
    const auto state = GpuRendererAccess::state(*created.device);
    oracle::setTimeoutForTests(true);
    expect(!fullProcess(image).hasValue(), "closing owner submits real oracle");
    expect(state->device.getDispatcher()->vkDeviceWaitIdle(static_cast<VkDevice>(*state->device)) ==
               VK_SUCCESS,
           "real fence completion precedes facade close");
    created.device.reset();
    expect(state->imageOwnerClosed && oracle::stats(state.get()).reservations == 0,
           "facade close drains exact oracle before closing admission");
    expect(image.isValid() && !fullProcess(image).hasValue(),
           "surviving image remains valid while closed owner refuses new staging allocation");
    image = makeGpuImage(nullptr);
    created = GpuDevice::create(options);
    expect(static_cast<bool>(created), "unproved closing owner creates real device");
    if (!created) {
        return;
    }
    auto retained = process(*created.device);
    const auto retainedSourceBytes = retained.allocationBytes();
    const auto retainedState = GpuRendererAccess::state(*created.device);
    const auto before = bytes(*created.device);
    oracle::setTimeoutForTests(true);
    oracle::setRetirementDeferredForTests(true);
    expect(!fullProcess(retained).hasValue(), "unproved close owns actual submission");
    const auto stagingBytes = oracle::stats(retainedState.get()).stagingBytes;
    std::thread([last = std::move(retained)] {}).join();
    const auto closedBefore = GpuDevice::processImageReclamationStats();
    created.device.reset();
    oracle::setRetirementDeferredForTests(false);
    oracle::drain(*retainedState);
    VmaTotalStatistics native{};
    vmaCalculateStatistics(retainedState->allocator, &native);
    expect(retainedState->imageOwnerClosed && oracle::stats(retainedState.get()).quarantined == 1 &&
               native.total.statistics.allocationBytes == before + stagingBytes &&
               GpuDevice::processImageReclamationStats().closedOwnerRetiringAllocationBytes ==
                   closedBefore.closedOwnerRetiringAllocationBytes + retainedSourceBytes,
           "closed owner never reopens and retains bounded exact source plus actual staging bytes");
}
} // namespace
int main(int argc, char** argv) try {
    const auto parsed = proof::parseOptions(argc, argv);
    if (!parsed.valid) {
        return 2;
    }
    GpuDeviceCreationOptions options;
    options.loader_path = parsed.loader_path;
    auto created = GpuDevice::create(options);
    if (!created) {
        std::cout << "SKIP: native oracle device unavailable\n";
        return parsed.require_device ? 1 : 77;
    }
    expect(!fullProcess(makeGpuImage(nullptr)).hasValue() &&
               !fullDisplay(makeGpuDisplayImage(nullptr)).hasValue() &&
               !sparseDisplay(makeGpuDisplayImage(nullptr)).hasValue(),
           "all empty oracle sources fail closed");
    family(*created.device, process, fullProcess);
    family(*created.device, display, fullDisplay);
    family(*created.device, display, sparseDisplay);
    twoOwners(options, *created.device);
    exactProof(*created.device);
    auto core = std::make_shared<bloom::runtime::detail::PreviewDisplayServiceCore>();
    core->device = std::move(created.device);
    auto image = process(*core->device);
    oracle::setTimeoutForTests(true);
    expect(!fullProcess(image).hasValue(), "preview pump fixture creates real unknown submission");
    std::thread([last = std::move(image)] {}).join();
    expect(await([&] {
               bloom::runtime::detail::pumpServicePresentation(core);
               return oracle::stats().reservations == 0 &&
                      core->device->imageReclamationStats().retiringImages == 0;
           }),
           "actual preview service owner pump recovers oracle and foreign release");
    closedOwner(options);
    if (failures.load() != 0) {
        return 1;
    }
    std::cout << "PASS: real oracle full/sparse retirement, source lifetime, two owners and pump\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: oracle fixture exception: " << error.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "FAIL: unknown oracle fixture exception\n";
    return 1;
}
