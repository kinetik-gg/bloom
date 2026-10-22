#include "gpu_composite_offsets_private.hpp"
#include "gpu_operation_batch_private.hpp"
#include "gpu_translation_axis_private.hpp"
#include <bloom/render/processing_host_storage.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace bloom::render::operation_batch_detail {
namespace {
template <typename T> void pack(Job& job, const T& push) noexcept {
    static_assert(sizeof(T) <= sizeof(job.push));
    std::memcpy(job.push.data(), &push, sizeof(T));
    job.pushBytes = sizeof(T);
}
bool finite(const GpuAffineMatrix& matrix) noexcept {
    return std::isfinite(matrix.a) && std::isfinite(matrix.b) && std::isfinite(matrix.c) &&
           std::isfinite(matrix.d) && std::isfinite(matrix.tx) && std::isfinite(matrix.ty);
}
bool opacityValid(const float opacity) noexcept {
    return std::isfinite(opacity) && opacity >= 0 && opacity <= 1;
}
} // namespace
bool State::initialize() {
    if (initialized)
        return true;
    if (std::exchange(initializationAttempted, true))
        return false;
    if (control->maxComputeWorkGroupInvocations < kCompositeWorkgroupSizeX ||
        control->maxComputeWorkGroupSizeX < kCompositeWorkgroupSizeX) {
        fail(GpuOperationBatchCode::Unsupported, "batch compute workgroup size is unsupported");
        return false;
    }
    constexpr bool solidBindings[] = {true};
    constexpr bool gradientBindings[] = {true};
    constexpr bool translationBindings[] = {true, true, false, false, false};
    constexpr bool affineBindings[] = {true, true, false, false};
    constexpr bool overBindings[] = {true, true, true, false};
    std::string reason;
    if (!createCompositePipeline(*control, vulkan_detail::kSolidSpirvCode,
                                 vulkan_detail::kSolidSpirvByteCount, solidBindings, 1, 24, reason,
                                 pipelines[0]) ||
        !createCompositePipeline(*control, vulkan_detail::kTranslationOpacitySpirvCode,
                                 vulkan_detail::kTranslationOpacitySpirvByteCount,
                                 translationBindings, 5, 20, reason, pipelines[1]) ||
        !createCompositePipeline(*control, vulkan_detail::kAffineBilinearSpirvCode,
                                 vulkan_detail::kAffineBilinearSpirvByteCount, affineBindings, 4,
                                 20, reason, pipelines[2]) ||
        !createCompositePipeline(*control, vulkan_detail::kSourceOverSpirvCode,
                                 vulkan_detail::kSourceOverSpirvByteCount, overBindings, 4, 24,
                                 reason, pipelines[3]) ||
        !createCompositePipeline(*control, vulkan_detail::kGradientSpirvCode,
                                 vulkan_detail::kGradientSpirvByteCount, gradientBindings, 1, 96,
                                 reason, pipelines[4]))
        return false;
    const VkDevice device = static_cast<VkDevice>(*control->device);
    const auto* api = control->device.getDispatcher();
    const std::array poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 * kGpuOperationBatchCapacity},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kGpuOperationBatchCapacity}};
    const VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              nullptr,
                                              0,
                                              kGpuOperationBatchCapacity,
                                              static_cast<std::uint32_t>(poolSizes.size()),
                                              poolSizes.data()};
    const VkCommandPoolCreateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                              VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                              control->computeQueueFamily};
    const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
    if (api->vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptors) != VK_SUCCESS ||
        api->vkCreateCommandPool(device, &commandInfo, nullptr, &commandPool) != VK_SUCCESS ||
        api->vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
        return false;
    const VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                   nullptr, commandPool,
                                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    initialized = api->vkAllocateCommandBuffers(device, &allocateInfo, &command) == VK_SUCCESS;
    return initialized;
}

bool State::prepare(const std::span<const GpuBatchOperation> operations,
                    const GpuOperationBatchBudgets& budgets, const std::uint64_t byteBudget) {
    if (operations.empty() || operations.size() > kGpuOperationBatchCapacity ||
        (operations.size() == 1 && !std::holds_alternative<GpuBatchGradient>(operations.front()))) {
        fail(GpuOperationBatchCode::InvalidRequest,
             "native batch requires two to eight operations, or one gradient");
        return false;
    }
    const auto resolve = [this](const GpuBatchImageRef& ref, const std::uint32_t index,
                                std::shared_ptr<const GpuImage>& pin) -> const GpuImageImpl* {
        if (ref.external) {
            if (ref.operation != kGpuBatchExternalImage || !ref.external->isValid())
                return nullptr;
            pin = ref.external;
            const auto* image = gpuImageImpl(*pin);
            return compositeImageBelongsTo(image, control) &&
                           image->generation == expectedGeneration &&
                           !image->submissionUnretired.load(std::memory_order_acquire)
                       ? image
                       : nullptr;
        }
        return ref.operation < index ? jobs[ref.operation].image.get() : nullptr;
    };
    for (std::uint32_t index = 0; index < operations.size(); ++index) {
        auto& job = jobs[index];
        count = index + 1; // Cleanup includes a partially prepared current operation.
        job.kind = operations[index].index();
        std::optional<ImageWindow> window;
        std::optional<ImageWindow> display;
        core::PixelAspectRatio aspect = core::PixelAspectRatio::square();
        std::uint64_t requestedMetadata = 0;
        if (const auto* solid = std::get_if<GpuBatchSolid>(&operations[index])) {
            window = solid->parameters.dataWindow;
            display = solid->parameters.displayWindow;
            aspect = solid->parameters.pixelAspect;
            const auto& pixel = solid->parameters.pixel;
            pack(job, SolidPushConstants{{pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()},
                                         window->extent().width(),
                                         window->extent().height()});
        } else if (const auto* gradient = std::get_if<GpuBatchGradient>(&operations[index])) {
            window = gradient->dataWindow;
            display = gradient->displayWindow;
            aspect = gradient->pixelAspect;
            struct alignas(16) GradientPush final {
                std::array<float, 4> first, second, endpoints, transform, extent;
                std::array<std::int32_t, 4> flags;
            };
            static_assert(sizeof(GradientPush) == 96);
            constexpr float arithmeticLimit = std::numeric_limits<float>::max() / 2.0F;
            const auto finiteArray = [](const auto& values) {
                return std::ranges::all_of(values, [](const float v) {
                    return std::isfinite(v) &&
                           std::fabs(v) <= std::numeric_limits<float>::max() / 2.0F;
                });
            };
            const auto aspectValue = static_cast<float>(aspect.numerator()) /
                                     static_cast<float>(aspect.denominator());
            const auto safeGeometry = [&] {
                if (!std::isfinite(aspectValue) || aspectValue <= 0) return false;
                const long double limit = static_cast<long double>(arithmeticLimit);
                const long double minNormal =
                    static_cast<long double>(std::numeric_limits<float>::min());
                const long double physicalAspect = static_cast<long double>(aspectValue);
                const long double scaleX =
                    static_cast<long double>(gradient->inverseHorizontalScale);
                const long double scaleY =
                    static_cast<long double>(gradient->inverseVerticalScale);
                const long double sx = static_cast<long double>(gradient->endpoints[0]);
                const long double sy = static_cast<long double>(gradient->endpoints[1]);
                const long double dx = (static_cast<long double>(gradient->endpoints[2]) - sx) *
                                       physicalAspect;
                const long double dy = static_cast<long double>(gradient->endpoints[3]) - sy;
                const long double span2 = dx * dx + dy * dy;
                if (span2 > limit || (span2 > 0 && span2 < minNormal)) return false;
                const long double farX =
                    (static_cast<long double>(window->extent().width()) - 0.5L) * scaleX;
                const long double farY =
                    (static_cast<long double>(window->extent().height()) - 0.5L) * scaleY;
                const long double px = std::max(
                    std::abs((0.5L * scaleX - sx) * physicalAspect),
                    std::abs((farX - sx) * physicalAspect));
                const long double py = std::max(
                    std::abs(0.5L * scaleY - sy), std::abs(farY - sy));
                const long double offset2 = px * px + py * py;
                const long double projection = px * std::abs(dx) + py * std::abs(dy);
                return offset2 <= limit && projection <= limit &&
                       (span2 == 0 ||
                        (gradient->radial ? std::sqrt(offset2 / span2) : projection / span2) <=
                            limit);
            };
            if (!finiteArray(gradient->first) || !finiteArray(gradient->second) ||
                !std::ranges::all_of(gradient->endpoints, [](const float value) {
                    return std::isfinite(value);
                }) ||
                gradient->first[3] < 0 || gradient->first[3] > 1 ||
                gradient->second[3] < 0 || gradient->second[3] > 1 ||
                !std::isfinite(gradient->inverseHorizontalScale) ||
                !std::isfinite(gradient->inverseVerticalScale) ||
                gradient->inverseHorizontalScale <= 0 ||
                gradient->inverseVerticalScale <= 0 ||
                !std::isfinite(gradient->exactWidth) || gradient->exactWidth <= 0 ||
                !std::isfinite(gradient->exactHeight) || gradient->exactHeight <= 0 ||
                gradient->repeat > 2 ||
                window->originX() != 0 || window->originY() != 0 || !safeGeometry()) {
                fail(GpuOperationBatchCode::Unsupported,
                     "gradient values exceed the native Float32 arithmetic capability");
                return false;
            }
            pack(job, GradientPush{gradient->first, gradient->second, gradient->endpoints,
                                   {gradient->inverseHorizontalScale,
                                    gradient->inverseVerticalScale, aspectValue,
                                    static_cast<float>(gradient->repeat)},
                                   {static_cast<float>(window->extent().width()),
                                    static_cast<float>(window->extent().height()),
                                    gradient->exactWidth, gradient->exactHeight},
                                   {gradient->radial ? 1 : 0, 0, 0, 0}});
        } else {
            const GpuBatchImageRef* source = nullptr;
            float opacity = 1;
            if (const auto* translation = std::get_if<GpuBatchTranslation>(&operations[index])) {
                source = &translation->source;
                window = translation->outputWindow;
                opacity = translation->opacity;
                if (!std::isfinite(translation->translationX) ||
                    !std::isfinite(translation->translationY))
                    window.reset();
            } else if (const auto* affine = std::get_if<GpuBatchAffine>(&operations[index])) {
                source = &affine->source;
                window = affine->outputWindow;
                opacity = affine->opacity;
                if (!finite(affine->matrix))
                    window.reset();
            } else if (const auto* over = std::get_if<GpuBatchSourceOver>(&operations[index])) {
                source = &over->source;
                job.destination = resolve(over->destination, index, job.destinationPin);
                if (job.destination) {
                    window = job.destination->dataWindow;
                    display = job.destination->displayWindow;
                    aspect = job.destination->pixelAspect;
                }
            }
            job.source = source ? resolve(*source, index, job.sourcePin) : nullptr;
            if (!job.source || !window || !job.source->dataWindow || !opacityValid(opacity) ||
                (job.kind == 3 && !job.destination)) {
                fail(GpuOperationBatchCode::InvalidRequest,
                     "batch input is non-topological, foreign, pending, or has invalid fields");
                return false;
            }
            if (job.kind != 3) {
                display = job.source->displayWindow;
                aspect = job.source->pixelAspect;
            }
            if (const auto* affine = std::get_if<GpuBatchAffine>(&operations[index]);
                affine && affine->outputDescriptor) {
                if (affine->outputDescriptor->dataWindow() != *window) {
                    fail(GpuOperationBatchCode::InvalidRequest,
                         "batch affine output descriptor disagrees with its data window");
                    return false;
                }
                display = affine->outputDescriptor->displayWindow();
                aspect = affine->outputDescriptor->pixelAspect();
            }
            requestedMetadata = sizeof(std::uint32_t);
            const auto width = window->extent().width();
            const auto height = window->extent().height();
            if (job.kind == 1)
                requestedMetadata +=
                    (static_cast<std::uint64_t>(width) + height) * sizeof(GpuAxisSample);
            else if (job.kind == 2)
                requestedMetadata += sizeof(GpuAffineMapGpu);
            if (requestedMetadata > budgets.maxMetadataBytes) {
                fail(GpuOperationBatchCode::OverBudget,
                     "batch metadata exceeds the operation ceiling");
                return false;
            }
            if (job.kind == 3) {
                const auto sourceWindow = *job.source->dataWindow;
                std::int32_t offsetX = 0;
                std::int32_t offsetY = 0;
                if (!compositeSourceOffset(window->originX(), sourceWindow.originX(), width,
                                           offsetX) ||
                    !compositeSourceOffset(window->originY(), sourceWindow.originY(), height,
                                           offsetY)) {
                    fail(GpuOperationBatchCode::InvalidRequest,
                         "source-over offset is not representable");
                    return false;
                }
                pack(job, CompositeSourceOverPush{width, height, job.source->width,
                                                  job.source->height, offsetX, offsetY});
            } else {
                pack(job, CompositeTranslationPush{width, height, job.source->width,
                                                   job.source->height, opacity});
            }
        }
        std::uint64_t imageBytes = 0;
        const auto width = window->extent().width();
        const auto height = window->extent().height();
        const auto support = querySolidImageSupport(*control, width, height);
        const auto limits = control->physicalDevice.getProperties().limits;
        if (!support.supported ||
            compositeDispatchGroupCount(width) > limits.maxComputeWorkGroupCount[0] ||
            height > limits.maxComputeWorkGroupCount[1]) {
            fail(GpuOperationBatchCode::Unsupported,
                 "batch image or dispatch extent is unsupported");
            return false;
        }
        const auto remaining = byteBudget - std::min(byteBudget, statistics.peakAllocationBytes);
        if (!display || compositeMultiplyOverflows(width, height, imageBytes) ||
            compositeMultiplyOverflows(imageBytes, sizeof(Rgba32f), imageBytes) ||
            imageBytes > budgets.maxImageBytes || imageBytes > support.maxImageBytes ||
            imageBytes > remaining || requestedMetadata > remaining - imageBytes) {
            fail(GpuOperationBatchCode::OverBudget,
                 "batch image extent or requested bytes exceed capacity");
            return false;
        }
        job.image = std::make_unique<GpuImageImpl>();
        job.image->state = control;
        job.image->dataWindow = window;
        job.image->displayWindow = display;
        job.image->pixelAspect = aspect;
        job.image->generation = expectedGeneration;
        if (!createResidentImage(*control, width, height, *job.image, true)) {
            fail(GpuOperationBatchCode::AllocationFailed, "batch output backing was refused");
            return false;
        }
        const auto buffer = [this, &job](const std::size_t slot, const std::uint64_t bytes,
                                         const bool host, const void* data) {
            return createCompositeBuffer(control, bytes, host, data, job.metadata[slot], true);
        };
        const std::uint32_t zero = 0;
        if (job.kind != 0 && job.kind != 4 && !buffer(2, sizeof(zero), true, &zero)) {
            fail(GpuOperationBatchCode::AllocationFailed, "batch status backing was refused");
            return false;
        }
        if (const auto* translation = std::get_if<GpuBatchTranslation>(&operations[index])) {
            const auto axisX = composite_detail::prepareProcessingTranslationAxis(
                width, job.source->width, translation->translationX, control->processingPhase);
            const auto axisY = composite_detail::prepareProcessingTranslationAxis(
                height, job.source->height, translation->translationY, control->processingPhase);
            if (!buffer(0, axisX.size() * sizeof(GpuAxisSample), false, axisX.data()) ||
                !buffer(1, axisY.size() * sizeof(GpuAxisSample), false, axisY.data())) {
                fail(GpuOperationBatchCode::AllocationFailed,
                     "batch translation metadata was refused");
                return false;
            }
        } else if (const auto* affine = std::get_if<GpuBatchAffine>(&operations[index])) {
            const auto map = packAffineMap(prepareAffineMatrixMap(affine->matrix, *window));
            if (!buffer(0, sizeof(map), false, &map)) {
                fail(GpuOperationBatchCode::AllocationFailed, "batch affine metadata was refused");
                return false;
            }
        }
        const auto actualImage = compositeImageAllocationBytes(*job.image);
        if (compositeAddOverflows(statistics.imageBytes, actualImage, statistics.imageBytes) ||
            compositeAddOverflows(statistics.peakAllocationBytes, actualImage,
                                  statistics.peakAllocationBytes)) {
            fail(GpuOperationBatchCode::OverBudget, "batch allocation accounting overflowed");
            return false;
        }
        for (const auto& metadata : job.metadata) {
            std::uint64_t bytes = 0;
            if (compositeAddOverflows(compositeBufferAllocationBytes(metadata),
                                      metadata.stagingBytes, bytes) ||
                compositeAddOverflows(statistics.peakAllocationBytes, bytes,
                                      statistics.peakAllocationBytes)) {
                fail(GpuOperationBatchCode::OverBudget, "batch metadata accounting overflowed");
                return false;
            }
        }
        if (statistics.peakAllocationBytes > byteBudget) {
            fail(GpuOperationBatchCode::OverBudget,
                 "actual whole-batch backing exceeds granted headroom");
            return false;
        }
    }
    return true;
}
} // namespace bloom::render::operation_batch_detail
