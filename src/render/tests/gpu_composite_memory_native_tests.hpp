#pragma once

#include "gpu_composite_native_support.hpp"
#include "gpu_composite_private.hpp"

#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/gpu_blend.hpp>

namespace bloom::render::composite_proof {
namespace memory_proof {

struct Observations final {
    std::size_t flushes = 0;
    std::size_t invalidates = 0;
    std::size_t noncoherent = 0;
    bool validRanges = true;
    bool failFlush = false;
    std::size_t failFlushAt = 0;
    bool failInvalidate = false;
    VkDeviceSize lastBytes = 0;
    VkMemoryPropertyFlags observedProperties = 0;
};
inline Observations observations;

inline VkResult observe(const bool invalidate, VmaAllocator allocator, VmaAllocation allocation,
                        const VkDeviceSize offset, const VkDeviceSize bytes) noexcept {
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(allocator, allocation, &info);
    VkMemoryPropertyFlags properties = 0;
    vmaGetAllocationMemoryProperties(allocator, allocation, &properties);
    observations.validRanges &= offset == 0 && bytes != 0 && bytes <= info.size &&
                                info.pMappedData != nullptr &&
                                (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    observations.lastBytes = bytes;
    observations.observedProperties |= properties;
    if ((properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
        ++observations.noncoherent;
    }
    if (invalidate) {
        ++observations.invalidates;
        if (observations.failInvalidate) {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }
        return vmaInvalidateAllocation(allocator, allocation, offset, bytes);
    }
    ++observations.flushes;
    if (observations.failFlush || observations.flushes == observations.failFlushAt) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    return vmaFlushAllocation(allocator, allocation, offset, bytes);
}

struct HookScope final {
    HookScope() { composite_detail::mappedMemoryHook.store(&observe); }
    ~HookScope() { composite_detail::mappedMemoryHook.store(nullptr); }
};

template <typename Operator, typename Begin>
void testConsumer(Expectations& expectations, Operator& operation, Begin begin,
                  const std::string& name) {
    const auto flushBefore = observations.flushes;
    const auto stagingBefore = composite_detail::metadataStagingLiveBytes.load();
    auto started = begin();
    expectations.expect(static_cast<int>(started.code) == 0, name + ": begins");
    if (static_cast<int>(started.code) != 0) {
        return;
    }
    if (name == "translation status") {
        expectations.expect(composite_detail::metadataStagingLiveBytes.load() > stagingBefore,
                            "translation retains both staged axes through owning dispatch");
    }
    using Poll = decltype(operation.poll());
    auto result = Poll::Pending;
    while (result == Poll::Pending) {
        result = operation.poll();
    }
    expectations.expect(result == Poll::Ready, name + ": completes");
    expectations.expect(composite_detail::metadataStagingLiveBytes.load() == stagingBefore,
                        name + ": releases staging at proven completion before image take");
    expectations.expect(observations.flushes > flushBefore, name + ": initializes with flush");
    expectations.expect(observations.invalidates == 1 && observations.lastBytes == 4,
                        name + ": invalidates exactly the status word before publication");
    expectations.expect(operation.takeImage().isValid(), name + ": publishes after visibility");

    observations.failFlush = true;
    started = begin();
    observations.failFlush = false;
    expectations.expect(composite_detail::metadataStagingLiveBytes.load() == stagingBefore,
                        name + ": pre-submit failure immediately frees all staging");
    expectations.expect(static_cast<int>(started.code) != 0,
                        name + ": flush failure rejects initialization");
    expectations.expect(!operation.hasUnretiredSubmission() && !operation.takeImage().isValid(),
                        name + ": flush failure submits and publishes nothing");

    started = begin();
    expectations.expect(static_cast<int>(started.code) == 0,
                        name + ": retries after flush failure");
    if (static_cast<int>(started.code) != 0) {
        return;
    }
    observations.failInvalidate = true;
    result = Poll::Pending;
    while (result == Poll::Pending) {
        result = operation.poll();
    }
    observations.failInvalidate = false;
    expectations.expect(result == Poll::Failure, name + ": invalidate failure rejects output");
    expectations.expect(!operation.hasUnretiredSubmission() && !operation.takeImage().isValid(),
                        name + ": failed visibility publishes no image after retirement");
}

inline void testPresentationBootstrap(Expectations& expectations,
                                      const GpuDeviceCreationOptions& original) {
    auto options = original;
    options.request_presentation = true;
    options.presentation_platform = GpuPresentationPlatform::Wayland;
    auto device = GpuDevice::create(options);
    expectations.expect(static_cast<bool>(device), "presentation dependency bootstrap succeeds");
    if (!device) {
        return;
    }
    const auto status = device.device->presentationStatus();
    expectations.expect(!status.present_fences ||
                            (status.surface_extension && status.swapchain_extension),
                        "present-fence prerequisites are enabled");
    std::cout << "PRESENTATION: availability=" << static_cast<int>(status.availability)
              << " present_fences=" << status.present_fences
              << " present_wait=" << status.present_wait << '\n';
}

inline void run(Expectations& expectations, GpuDevice& device) {
    const auto control = GpuRendererAccess::state(device);
    HookScope hook;
    observations = {};
    const std::uint32_t initial = 42;
    for (const bool direct : {true, false}) {
        CompositeBuffer buffer;
        const auto before = observations.flushes;
        expectations.expect(
            createCompositeBuffer(control, sizeof(initial), direct, &initial, buffer),
            "mapped initialization succeeds");
        expectations.expect(observations.flushes == before + 1 && observations.lastBytes == 4,
                            "direct/staged initialization flushes the allocation-relative word");
        expectations.expect(buffer.state == control && !buffer.state.owner_before(control) &&
                                !control.owner_before(buffer.state),
                            "metadata shares the genuine allocator owner");
        expectations.expect(direct ? buffer.stagingBytes == 0 : buffer.stagingBytes >= 4,
                            "only staged initialization retains actual allocation bytes");
        const VkBuffer originalBuffer = buffer.buffer;
        observations.failFlush = true;
        expectations.expect(
            !createCompositeBuffer(control, sizeof(initial), direct, &initial, buffer) &&
                buffer.buffer == originalBuffer && buffer.armed,
            "failed preparation leaves existing output ownership untouched");
        observations.failFlush = false;
        buffer.release();
        observations.failFlush = true;
        expectations.expect(
            !createCompositeBuffer(control, sizeof(initial), direct, &initial, buffer),
            "initialization flush fails closed");
        expectations.expect(!buffer.armed, "failed flush publishes no buffer");
        observations.failFlush = false;
    }
    auto uploader = GpuImageUpload::create(device);
    auto image = makeImage(window(0, 0, 5, 3), window(0, 0, 5, 3), PixelAspectRatio::square(),
                           checker(5, 3));
    auto resident =
        image && uploader
            ? upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*image)))
            : std::nullopt;
    expectations.expect(resident.has_value(), "memory visibility consumer source uploads");
    if (!resident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*resident));
    const auto sourceWindow = source->dataWindow();
    expectations.expect(sourceWindow.has_value(), "consumer source has a data window");
    if (!sourceWindow) {
        return;
    }
    const auto outputWindow = *sourceWindow;
    auto composite = GpuComposite::create(device);
    auto affine = GpuAffine::create(device);
    auto blend = GpuBlend::create(device);
    auto transform = LayerTransform::create({}, outputWindow, 1.0, 1.0);
    expectations.expect(composite && affine && blend && transform,
                        "memory visibility consumers initialize");
    if (!composite || !affine || !blend || !transform) {
        return;
    }
    // Fail after the first staged axis, then after both axes at the mapped status initialization.
    for (const std::size_t boundary : {2U, 3U}) {
        VmaTotalStatistics before{};
        vmaCalculateStatistics(control->allocator, &before);
        const auto stagingBefore = composite_detail::metadataStagingLiveBytes.load();
        observations.failFlushAt = observations.flushes + boundary;
        const auto refused =
            composite.composite->beginTranslation({source, outputWindow, 0.25, 0.5, 1.0F}, kBudget);
        observations.failFlushAt = 0;
        VmaTotalStatistics after{};
        vmaCalculateStatistics(control->allocator, &after);
        // The first request lazily builds pipeline state, which has no VMA allocations.
        expectations.expect(
            refused.code == GpuCompositeDiagnosticCode::AllocationFailed &&
                !composite.composite->hasUnretiredSubmission() &&
                after.total.statistics.allocationCount == before.total.statistics.allocationCount &&
                after.total.statistics.allocationBytes == before.total.statistics.allocationBytes &&
                composite_detail::metadataStagingLiveBytes.load() == stagingBefore,
            "partial-axis/status failure immediately restores VMA and staging baseline");
    }
    observations.invalidates = 0;
    testConsumer(
        expectations, *composite.composite,
        [&] {
            return composite.composite->beginTranslation({source, outputWindow, 0.25, 0.5, 1.0F},
                                                         kBudget);
        },
        "translation status");
    observations.invalidates = 0;
    testConsumer(
        expectations, *composite.composite,
        [&] { return composite.composite->beginSourceOver({source, source}, kBudget); },
        "source-over status");
    observations.invalidates = 0;
    testConsumer(
        expectations, *affine.affine,
        [&] {
            return affine.affine->beginAffine({source, outputWindow, *transform.value()}, kBudget);
        },
        "affine status");
    observations.invalidates = 0;
    testConsumer(
        expectations, *blend.blend,
        [&] {
            return blend.blend->beginBlend({source, source, core::BlendMode::Multiply}, kBudget);
        },
        "blend status");
    expectations.expect(observations.validRanges,
                        "every actual flush/invalidate uses mapped bounds");
    std::cout << "MEMORY: observed " << observations.flushes << " flushes and "
              << observations.noncoherent << " noncoherent allocation accesses; memory flags union="
              << observations.observedProperties << "; "
              << (observations.noncoherent == 0 ? "noncoherent execution untested"
                                                : "noncoherent memory exercised")
              << '\n';
}

} // namespace memory_proof
} // namespace bloom::render::composite_proof
