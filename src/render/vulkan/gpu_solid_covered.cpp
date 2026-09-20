#include "gpu_solid_private.hpp"

#include "gpu_path_coverage_private.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_path_coverage.hpp>

#include "shaders/solid_covered_spirv.inc"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// CoveredSolidV1: the native acceleration of the CPU fractional-Solid
// vector-coverage arm. The host builds a 256-entry premultiplied RGBA32F palette
// with the EXISTING CPU primitive coverageSolidRow() (one entry per coverage
// byte) followed by the exact separate Float32 opacity multiply and Rgba32f
// validation; the shader selects a palette entry from the packed R8 mask. This
// is O(256) host metadata, never a full-frame CPU render, and it preserves
// coverageSolidRow's Float64 fraction and its exact 0/255 endpoints. A bilinear
// translation of a filled bitmap is deliberately not used: it would produce
// different pixels for a fractional Solid.
//
// The mask and palette are host-visible storage buffers written and flushed
// before submission and retained until the producing fence is proved retired,
// exactly like the upload staging buffer. They are ordinary per-job resources;
// no second backend class exists.

namespace bloom::render {
namespace {

using vulkan_detail::DeviceAllocatorState;

constexpr std::uint32_t kWorkgroupSizeX = 256;
constexpr std::size_t kPaletteEntries = 256;
constexpr std::uint64_t kPaletteBytes =
    static_cast<std::uint64_t>(kPaletteEntries) * sizeof(Rgba32f);
constexpr std::uint64_t kMaxImageBytes = 256ULL * 1024ULL * 1024ULL;

struct CoveredPushConstants final {
    std::uint32_t width;
    std::uint32_t height;
};
static_assert(sizeof(CoveredPushConstants) == 8);

[[nodiscard]] std::uint64_t actualAllocationBytes(DeviceAllocatorState& state,
                                                  const VmaAllocation allocation) noexcept {
    if (allocation == VK_NULL_HANDLE) {
        return 0;
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(state.allocator, allocation, &info);
    return static_cast<std::uint64_t>(info.size);
}

// palette[c] is exactly the CPU coverage arm for coverage byte c: the existing
// coverageSolidRow() primitive at that sample, then the separate Float32 opacity
// multiply through Rgba32f::fromPremultiplied. Using coverageSolidRow() (not a
// float(c)/255 approximation) keeps its Float64 fraction and exact endpoints.
[[nodiscard]] ImageStatus buildCoveredPalette(const Rgba32f pixel, const float opacity,
                                              const std::span<Rgba32f> palette) noexcept {
    static_assert(kPaletteEntries == 256);
    if (palette.size() != kPaletteEntries) {
        return ImageError::codeOnly(ImageErrorCode::InvalidStorageSize);
    }
    std::array<std::uint8_t, kPaletteEntries> samples{};
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] = static_cast<std::uint8_t>(index);
    }
    if (const auto status = coverageSolidRow(samples, pixel, palette)) {
        return status;
    }
    for (auto& value : palette) {
        const auto faded =
            Rgba32f::fromPremultiplied(value.red() * opacity, value.green() * opacity,
                                       value.blue() * opacity, value.alpha() * opacity);
        if (!faded) {
            return ImageError::codeOnly(ImageErrorCode::InvalidPixel);
        }
        value = *faded.value();
    }
    return std::nullopt;
}

// Explicit portable little-endian packing: byte i occupies mask word i / 4 at
// bit shift (i % 4) * 8, which is exactly what solid_covered.comp shifts back
// out. This is host-endianness independent; a raw uint32 memcpy would not be.
void packCoverageLittleEndian(const std::span<const std::uint8_t> coverage,
                              const std::span<std::uint8_t> packed) noexcept {
    std::fill(packed.begin(), packed.end(), std::uint8_t{0});
    const std::size_t wholeWords = coverage.size() / 4;
    for (std::size_t word = 0; word < wholeWords; ++word) {
        const std::uint32_t value = static_cast<std::uint32_t>(coverage[word * 4 + 0]) |
                                    (static_cast<std::uint32_t>(coverage[word * 4 + 1]) << 8) |
                                    (static_cast<std::uint32_t>(coverage[word * 4 + 2]) << 16) |
                                    (static_cast<std::uint32_t>(coverage[word * 4 + 3]) << 24);
        packed[word * 4 + 0] = static_cast<std::uint8_t>(value & 0xFFu);
        packed[word * 4 + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
        packed[word * 4 + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
        packed[word * 4 + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
    }
    for (std::size_t index = wholeWords * 4; index < coverage.size(); ++index) {
        packed[index] = coverage[index];
    }
}

[[nodiscard]] bool createHostStorageBuffer(DeviceAllocatorState& state, const std::uint64_t bytes,
                                           GpuSolidUploadBuffer& out) noexcept {
    if (bytes == 0) {
        return false;
    }
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateBuffer(state.allocator, &bufferInfo, &allocationInfo, &buffer, &allocation,
                        nullptr) != VK_SUCCESS) {
        return false;
    }
    out.state = &state;
    out.buffer = buffer;
    out.allocation = allocation;
    out.bytes = bytes;
    out.armed = true;
    return true;
}

} // namespace

bool GpuSolid::Impl::createCoveredPipeline() {
    covered.attempted = true;
    covered.ready = false;
    const VkDevice rawDevice = static_cast<VkDevice>(*control->device);
    const auto* dispatcher = control->device.getDispatcher();

    vk::ShaderModuleCreateInfo shaderInfo{};
    shaderInfo.codeSize = vulkan_detail::kSolidCoveredSpirvByteCount;
    shaderInfo.pCode = vulkan_detail::kSolidCoveredSpirvCode;
    VkShaderModule rawShader = VK_NULL_HANDLE;
    if (dispatcher->vkCreateShaderModule(
            rawDevice, reinterpret_cast<const VkShaderModuleCreateInfo*>(&shaderInfo), nullptr,
            &rawShader) != VK_SUCCESS) {
        covered.reason = "the embedded CoveredSolidV1 shader module was rejected";
        return false;
    }
    covered.shaderModule = vk::raii::ShaderModule(control->device, rawShader);

    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = vk::DescriptorType::eStorageImage;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
    bindings[1].binding = 1;
    bindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = vk::ShaderStageFlagBits::eCompute;
    bindings[2].binding = 2;
    bindings[2].descriptorType = vk::DescriptorType::eStorageBuffer;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = vk::ShaderStageFlagBits::eCompute;
    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    VkDescriptorSetLayout rawLayout = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorSetLayout(
            rawDevice, reinterpret_cast<const VkDescriptorSetLayoutCreateInfo*>(&layoutInfo),
            nullptr, &rawLayout) != VK_SUCCESS) {
        covered.reason = "the CoveredSolidV1 descriptor set layout was rejected";
        return false;
    }
    covered.descriptorSetLayout = vk::raii::DescriptorSetLayout(control->device, rawLayout);

    vk::PushConstantRange pushRange{};
    pushRange.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pushRange.offset = 0;
    pushRange.size = static_cast<std::uint32_t>(sizeof(CoveredPushConstants));
    const vk::DescriptorSetLayout setLayout = *covered.descriptorSetLayout;
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout rawPipelineLayout = VK_NULL_HANDLE;
    if (dispatcher->vkCreatePipelineLayout(
            rawDevice, reinterpret_cast<const VkPipelineLayoutCreateInfo*>(&pipelineLayoutInfo),
            nullptr, &rawPipelineLayout) != VK_SUCCESS) {
        covered.reason = "the CoveredSolidV1 pipeline layout was rejected";
        return false;
    }
    covered.pipelineLayout = vk::raii::PipelineLayout(control->device, rawPipelineLayout);

    vk::ComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.stage.stage = vk::ShaderStageFlagBits::eCompute;
    pipelineInfo.stage.module = *covered.shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = *covered.pipelineLayout;
    VkPipeline rawPipeline = VK_NULL_HANDLE;
    if (dispatcher->vkCreateComputePipelines(
            rawDevice, VK_NULL_HANDLE, 1,
            reinterpret_cast<const VkComputePipelineCreateInfo*>(&pipelineInfo), nullptr,
            &rawPipeline) != VK_SUCCESS) {
        covered.reason = "the embedded CoveredSolidV1 compute pipeline was rejected";
        return false;
    }
    covered.pipeline = vk::raii::Pipeline(control->device, rawPipeline);

    std::array<vk::DescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1};
    poolSizes[1] = vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 2};
    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VkDescriptorPool rawPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorPool(
            rawDevice, reinterpret_cast<const VkDescriptorPoolCreateInfo*>(&poolInfo), nullptr,
            &rawPool) != VK_SUCCESS) {
        covered.reason = "the CoveredSolidV1 descriptor pool could not be created";
        return false;
    }
    covered.descriptorPool = vk::raii::DescriptorPool(control->device, rawPool);

    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *covered.descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &setLayout;
    VkDescriptorSet rawSet = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateDescriptorSets(
            rawDevice, reinterpret_cast<const VkDescriptorSetAllocateInfo*>(&allocateInfo),
            &rawSet) != VK_SUCCESS) {
        covered.reason = "the CoveredSolidV1 descriptor set could not be allocated";
        return false;
    }
    covered.descriptorSet =
        vk::raii::DescriptorSet(control->device, rawSet, *covered.descriptorPool);
    covered.ready = true;
    return true;
}

GpuSolidDiagnostic GpuSolid::Impl::beginCoveredJob(
    const GpuSolidParameters& base, const std::span<const std::uint8_t> hostCoverage,
    const float opacity, const std::uint64_t byteBudget, const VkBuffer residentMaskBuffer,
    const std::uint64_t residentMaskBytes, std::shared_ptr<void> residentOwner) {
    Impl& impl = *this;
    const bool usingResident = residentMaskBuffer != VK_NULL_HANDLE;
    if (!impl.onOwnerThread()) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::WrongThread,
                                  "beginCovered must run on the device owner thread");
    }
    if (impl.deviceLost) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::DeviceLost,
                                  "the device was lost; this generation must not be reused");
    }
    if (impl.queueSubmitted || impl.jobState == GpuSolidJobState::Pending) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::Busy, "one job is already in flight");
    }
    if (!std::isfinite(opacity) || opacity < 0.0F || opacity > 1.0F) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::InvalidArgument,
                                  "the covered opacity must be finite and within [0, 1]");
    }
    const std::uint32_t width = base.dataWindow.extent().width();
    const std::uint32_t height = base.dataWindow.extent().height();
    if (width == 0 || height == 0) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::InvalidArgument,
                                  "the data window is empty");
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (!usingResident && hostCoverage.size() != pixels) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::InvalidArgument,
                                  "the coverage byte count must equal width*height");
    }
    if (pixels > std::numeric_limits<std::uint64_t>::max() / sizeof(Rgba32f)) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::OverBudget,
                                  "the covered resident image byte count overflows");
    }
    const std::uint64_t imageBytes = pixels * sizeof(Rgba32f);
    if (imageBytes > impl.budgets.maxImageBytes || imageBytes > kMaxImageBytes) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::OverBudget,
                                  "the resident image exceeds the configured byte budget");
    }
    if (imageBytes > byteBudget) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::OverBudget,
                                  "the resident image exceeds the requested byte budget");
    }
    const std::uint64_t maskBytes = ((pixels + 3ULL) / 4ULL) * 4ULL;
    if (usingResident && residentMaskBytes != maskBytes) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::InvalidArgument,
                                  "the resident coverage mask does not match the data window");
    }
    const std::uint64_t retained = imageBytes + maskBytes + kPaletteBytes;
    if (retained < imageBytes || retained > byteBudget) {
        return gpuSolidDiagnostic(
            GpuSolidDiagnosticCode::OverBudget,
            "the image plus coverage mask and palette exceed the requested byte budget");
    }
    if (maskBytes == 0 || maskBytes > impl.control->maxStorageBufferRange) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::OverBudget,
                                  "the coverage mask exceeds the device storage buffer range");
    }
    if (impl.control->generation != impl.expectedGeneration) {
        impl.deviceLost = true;
        return gpuSolidDiagnostic(
            GpuSolidDiagnosticCode::DeviceLost,
            "the device generation changed; this pipeline must not be reused");
    }
    const SolidImageSupport support = querySolidImageSupport(*impl.control, width, height);
    if (!support.supported) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::Unsupported, support.reason);
    }
    if (imageBytes > support.maxImageBytes) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::OverBudget,
                                  "the resident image exceeds the device resource limit");
    }

    impl.clearJob();

    // O(256) host metadata: the entire coverage result space, built from the
    // existing CPU primitive. A failed palette means some coverage sample is not
    // representable, so the CPU vector arm must handle this layer instead.
    std::vector<Rgba32f> palette(kPaletteEntries, Rgba32f::transparent());
    if (const auto status = buildCoveredPalette(base.pixel, opacity, palette)) {
        (void)status;
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::InvalidArgument,
                                  "the covered palette is not representable as RGBA32F");
    }

    auto resident = std::make_unique<GpuImageImpl>();
    resident->state = impl.control;
    resident->dataWindow = base.dataWindow;
    resident->displayWindow = base.displayWindow;
    resident->pixelAspect = base.pixelAspect;
    resident->generation = impl.control->generation;
    if (!createResidentImage(*impl.control, width, height, *resident)) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                  "the covered resident image could not be allocated");
    }
    GpuImageImpl* const residentRaw = resident.get();
    impl.residentImage = std::make_unique<GpuImage>(makeGpuImage(std::move(resident)));

    if (!impl.covered.ready) {
        if (impl.covered.attempted) {
            impl.releaseResident();
            return gpuSolidDiagnostic(GpuSolidDiagnosticCode::Unsupported, impl.covered.reason);
        }
        if (!impl.createCoveredPipeline()) {
            impl.releaseResident();
            return gpuSolidDiagnostic(GpuSolidDiagnosticCode::Unsupported, impl.covered.reason);
        }
    }

    if (!createHostStorageBuffer(*impl.control, kPaletteBytes, impl.coveredPalette)) {
        impl.releaseResident();
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                  "the covered palette buffer could not be allocated");
    }
    if (usingResident) {
        // Bind the producer's already-resident packed mask directly. The shared
        // owner keeps it alive until this submission's fence is proved retired; no
        // host read or re-upload happens.
        impl.coveredMask.buffer = residentMaskBuffer;
        impl.coveredMask.bytes = residentMaskBytes;
        impl.coveredMask.armed = false;
        impl.coveredMask.owner = std::move(residentOwner);
    } else {
        if (!createHostStorageBuffer(*impl.control, maskBytes, impl.coveredMask)) {
            impl.coveredPalette.release();
            impl.releaseResident();
            return gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                      "the covered mask buffer could not be allocated");
        }
    }

    VmaAllocationInfo paletteInfo{};
    vmaGetAllocationInfo(impl.control->allocator, impl.coveredPalette.allocation, &paletteInfo);
    if (paletteInfo.pMappedData == nullptr) {
        impl.coveredPalette.release();
        impl.coveredMask.release();
        impl.releaseResident();
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                  "a covered input buffer could not be mapped");
    }
    std::memcpy(paletteInfo.pMappedData, palette.data(), static_cast<std::size_t>(kPaletteBytes));
    if (!usingResident) {
        VmaAllocationInfo maskInfo{};
        vmaGetAllocationInfo(impl.control->allocator, impl.coveredMask.allocation, &maskInfo);
        if (maskInfo.pMappedData == nullptr) {
            impl.coveredPalette.release();
            impl.coveredMask.release();
            impl.releaseResident();
            return gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                      "a covered input buffer could not be mapped");
        }
        packCoverageLittleEndian(
            hostCoverage, std::span<std::uint8_t>(static_cast<std::uint8_t*>(maskInfo.pMappedData),
                                                  static_cast<std::size_t>(maskBytes)));
    }
    if (vmaFlushAllocation(impl.control->allocator, impl.coveredPalette.allocation, 0,
                           VK_WHOLE_SIZE) != VK_SUCCESS ||
        (!usingResident && vmaFlushAllocation(impl.control->allocator, impl.coveredMask.allocation, 0,
                                         VK_WHOLE_SIZE) != VK_SUCCESS)) {
        impl.coveredPalette.release();
        impl.coveredMask.release();
        impl.releaseResident();
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                  "a covered input buffer could not be flushed");
    }

    // Authoritative budget against ACTUAL VMA allocation sizes (allocator rounding).
    const std::uint64_t actualImage = actualAllocationBytes(*impl.control, residentRaw->allocation);
    const std::uint64_t actualPalette =
        actualAllocationBytes(*impl.control, impl.coveredPalette.allocation);
    const std::uint64_t actualMask =
        usingResident ? residentMaskBytes
                 : actualAllocationBytes(*impl.control, impl.coveredMask.allocation);
    const std::uint64_t actualRetained = actualImage + actualPalette + actualMask;
    if (actualImage > impl.budgets.maxImageBytes || actualRetained < actualImage ||
        actualRetained > byteBudget) {
        impl.coveredPalette.release();
        impl.coveredMask.release();
        impl.releaseResident();
        return gpuSolidDiagnostic(
            GpuSolidDiagnosticCode::OverBudget,
            "actual VMA allocation sizes exceed the configured or requested byte budget");
    }
    impl.lastJobBytes = actualRetained;

    const VkDevice rawDevice = static_cast<VkDevice>(*impl.control->device);
    const auto* dispatcher = impl.control->device.getDispatcher();
    const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*impl.commandBuffer);
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    if (dispatcher->vkResetFences(rawDevice, 1, &rawFence) != VK_SUCCESS ||
        dispatcher->vkResetCommandBuffer(rawCommandBuffer, 0) != VK_SUCCESS) {
        impl.coveredPalette.release();
        impl.coveredMask.release();
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                  "the CoveredSolidV1 fence or command buffer could not be reset");
        return impl.jobDiagnostic;
    }

    vk::DescriptorImageInfo imageDescriptor{};
    imageDescriptor.imageView = residentRaw->view;
    imageDescriptor.imageLayout = vk::ImageLayout::eGeneral;
    vk::DescriptorBufferInfo paletteDescriptor{};
    paletteDescriptor.buffer = impl.coveredPalette.buffer;
    paletteDescriptor.offset = 0;
    paletteDescriptor.range = kPaletteBytes;
    vk::DescriptorBufferInfo maskDescriptor{};
    maskDescriptor.buffer = impl.coveredMask.buffer;
    maskDescriptor.offset = 0;
    maskDescriptor.range = maskBytes;
    std::array<vk::WriteDescriptorSet, 3> writes{};
    writes[0].dstSet = *impl.covered.descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = vk::DescriptorType::eStorageImage;
    writes[0].pImageInfo = &imageDescriptor;
    writes[1].dstSet = *impl.covered.descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[1].pBufferInfo = &paletteDescriptor;
    writes[2].dstSet = *impl.covered.descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[2].pBufferInfo = &maskDescriptor;
    dispatcher->vkUpdateDescriptorSets(rawDevice, static_cast<std::uint32_t>(writes.size()),
                                       reinterpret_cast<const VkWriteDescriptorSet*>(writes.data()),
                                       0, nullptr);

    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    if (dispatcher->vkBeginCommandBuffer(
            rawCommandBuffer, reinterpret_cast<const VkCommandBufferBeginInfo*>(&beginInfo)) !=
        VK_SUCCESS) {
        impl.coveredPalette.release();
        impl.coveredMask.release();
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                  "the CoveredSolidV1 command buffer could not begin");
        return impl.jobDiagnostic;
    }

    VkImageMemoryBarrier toGeneral{};
    toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneral.srcAccessMask = 0;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image = residentRaw->image;
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toGeneral);

    std::array<VkBufferMemoryBarrier, 2> inputBarriers{};
    const VkAccessFlags inputSourceAccess =
        VK_ACCESS_HOST_WRITE_BIT | (usingResident ? VK_ACCESS_SHADER_WRITE_BIT : 0U);
    const VkPipelineStageFlags inputSourceStage =
        VK_PIPELINE_STAGE_HOST_BIT |
        (usingResident ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : static_cast<VkPipelineStageFlags>(0U));
    for (std::size_t index = 0; index < inputBarriers.size(); ++index) {
        inputBarriers[index].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        inputBarriers[index].srcAccessMask = inputSourceAccess;
        inputBarriers[index].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        inputBarriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        inputBarriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        inputBarriers[index].offset = 0;
        inputBarriers[index].size = VK_WHOLE_SIZE;
    }
    inputBarriers[0].buffer = impl.coveredPalette.buffer;
    inputBarriers[1].buffer = impl.coveredMask.buffer;
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, inputSourceStage,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                                     static_cast<std::uint32_t>(inputBarriers.size()),
                                     inputBarriers.data(), 0, nullptr);

    impl.commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, *impl.covered.pipeline);
    impl.commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                          *impl.covered.pipelineLayout, 0,
                                          {*impl.covered.descriptorSet}, {});
    CoveredPushConstants push{};
    push.width = width;
    push.height = height;
    impl.commandBuffer.pushConstants(
        *impl.covered.pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0,
        static_cast<std::uint32_t>(sizeof(CoveredPushConstants)), &push);
    const std::uint64_t groupCount =
        (static_cast<std::uint64_t>(width) + kWorkgroupSizeX - 1U) / kWorkgroupSizeX;
    impl.commandBuffer.dispatch(static_cast<std::uint32_t>(groupCount), height, 1);

    VkImageMemoryBarrier toRead = toGeneral;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toRead);

    if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
        impl.coveredPalette.release();
        impl.coveredMask.release();
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                  "the CoveredSolidV1 command buffer could not end");
        return impl.jobDiagnostic;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &rawCommandBuffer;
    const VkResult submitted = dispatcher->vkQueueSubmit(
        static_cast<VkQueue>(*impl.control->computeQueue), 1, &submit, rawFence);
    if (submitted == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.coveredPalette.release();
        impl.coveredMask.release();
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceLost, "the device was lost during submission");
        return impl.jobDiagnostic;
    }
    if (submitted != VK_SUCCESS) {
        impl.coveredPalette.release();
        impl.coveredMask.release();
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                  "the CoveredSolidV1 dispatch could not submit");
        return impl.jobDiagnostic;
    }
    impl.queueSubmitted = true;
    impl.jobState = GpuSolidJobState::Pending;
    impl.jobDiagnostic = GpuSolidDiagnostic{};
    return {};
}

GpuSolidDiagnostic GpuSolid::beginCovered(const GpuSolidParameters& base,
                                          const std::span<const std::uint8_t> coverage,
                                          const float opacity, const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::DeviceUnavailable,
                                  "the SolidV1 pipeline is not initialized");
    }
    return impl_->beginCoveredJob(base, coverage, opacity, byteBudget, VK_NULL_HANDLE, 0, nullptr);
}

GpuSolidDiagnostic GpuSolid::beginCoveredResident(const GpuSolidParameters& base,
                                                  const GpuPathCoverage& coverage,
                                                  const float opacity,
                                                  const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::DeviceUnavailable,
                                  "the SolidV1 pipeline is not initialized");
    }
    // Owner first: never read the producer's state or mask from a foreign thread.
    if (!impl_->onOwnerThread()) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::WrongThread,
                                  "beginCoveredResident must run on the device owner thread");
    }
    if (impl_->deviceLost) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::DeviceLost,
                                  "the device was lost; this generation must not be reused");
    }
    auto mask = gpuPathCoverageMask(coverage);
    if (mask == nullptr || mask->state == nullptr || mask->buffer == VK_NULL_HANDLE) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::InvalidArgument,
                                  "the resident coverage exposes no mask buffer");
    }
    // Exact device and generation identity, then readiness and dimensions. Dimensions alone would
    // allow binding a foreign-device VkBuffer.
    if (mask->state != impl_->control) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::InvalidArgument,
                                  "the resident coverage belongs to a different device");
    }
    if (mask->generation != impl_->expectedGeneration) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::InvalidArgument,
                                  "the resident coverage belongs to a stale device generation");
    }
    if (coverage.state() != GpuPathCoverageJobState::Ready) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::InvalidArgument,
                                  "resident coverage must be Ready before it is consumed");
    }
    if (mask->width != base.dataWindow.extent().width() ||
        mask->height != base.dataWindow.extent().height()) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::InvalidArgument,
                                  "the resident coverage does not match the data window");
    }
    return impl_->beginCoveredJob(base, {}, opacity, byteBudget, mask->buffer, mask->bytes,
                                  std::static_pointer_cast<void>(mask));
}

} // namespace bloom::render
