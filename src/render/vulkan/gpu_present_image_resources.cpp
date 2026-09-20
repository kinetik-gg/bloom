#include "gpu_present_image_private.hpp"

#include "gpu_resident_display_private.hpp"

#include "shaders/viewer_present_spirv.inc"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace bloom::render {
namespace present_image_detail {
namespace {

using vulkan_detail::DeviceAllocatorState;

std::atomic<std::uint32_t> gDeviceResourceFault{UINT32_MAX};

[[nodiscard]] bool faultAt(const std::uint32_t stage) noexcept {
    return gDeviceResourceFault.load(std::memory_order_acquire) == stage;
}

[[nodiscard]] bool createBufferVma(DeviceAllocatorState& control, const std::uint64_t bytes,
                                   const VkBufferUsageFlags usage,
                                   const VmaAllocationCreateFlags flags, VkBuffer& outBuffer,
                                   VmaAllocation& outAllocation, VmaAllocationInfo& outInfo) {
    VkBufferCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    createInfo.size = bytes;
    createInfo.usage = usage;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = flags;
    return vmaCreateBuffer(control.allocator, &createInfo, &allocationInfo, &outBuffer,
                           &outAllocation, &outInfo) == VK_SUCCESS;
}

void destroyBufferVma(DeviceAllocatorState& control, VkBuffer& buffer,
                      VmaAllocation& allocation) noexcept {
    if (buffer != VK_NULL_HANDLE && allocation != VK_NULL_HANDLE) {
        vmaDestroyBuffer(control.allocator, buffer, allocation);
    }
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
}

[[nodiscard]] bool createOverlayImageVma(DeviceAllocatorState& control, const std::uint32_t width,
                                         const std::uint32_t height, VkImage& outImage,
                                         VmaAllocation& outAllocation) {
    VkImageCreateInfo createInfo;
    std::memset(&createInfo, 0, sizeof(createInfo));
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    createInfo.imageType = VK_IMAGE_TYPE_2D;
    createInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    createInfo.extent = VkExtent3D{.width = width, .height = height, .depth = 1};
    createInfo.mipLevels = 1;
    createInfo.arrayLayers = 1;
    createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    createInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    return vmaCreateImage(control.allocator, &createInfo, &allocationInfo, &outImage,
                          &outAllocation, nullptr) == VK_SUCCESS;
}

void destroyImageVma(DeviceAllocatorState& control, VkImage& image,
                     VmaAllocation& allocation) noexcept {
    if (image != VK_NULL_HANDLE && allocation != VK_NULL_HANDLE) {
        vmaDestroyImage(control.allocator, image, allocation);
    }
    image = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
}

} // namespace

PresentImagePipeline::~PresentImagePipeline() {
    // Single dependency-ordered teardown shared with the rebuild path: dependents (framebuffers,
    // views, pipeline, descriptor set, pipeline layout) are released before their dependencies, and
    // the overlay image view is dropped before the VMA-owned overlay image it references. The
    // params and staging buffers are host-visible and persistently mapped via
    // VMA_ALLOCATION_CREATE_MAPPED_BIT, so they must not be explicitly unmapped: vmaDestroyBuffer
    // releases their single creation mapping. The owning presenter only destroys this pipeline once
    // the previous frame's render fence is proven complete.
    resetDeviceResources();
}

void PresentImagePipeline::resetDeviceResources() noexcept {
    // Destroy dependents before their dependencies: framebuffers reference the render pass and the
    // image views; the graphics pipeline references the pipeline layout and render pass; the
    // descriptor set was allocated from the descriptor pool; the pipeline layout references the
    // descriptor set layout. Destroying a dependency first would leave the RAII destructor of the
    // dependent object freeing a handle whose owner is already gone.
    framebuffers.clear();
    swapchainViews.clear();
    pipeline = vk::raii::Pipeline{nullptr};
    renderPass = vk::raii::RenderPass{nullptr};
    descriptorSet = vk::raii::DescriptorSet{nullptr};
    descriptorPool = vk::raii::DescriptorPool{nullptr};
    pipelineLayout = vk::raii::PipelineLayout{nullptr};
    setLayout = vk::raii::DescriptorSetLayout{nullptr};
    sampler = vk::raii::Sampler{nullptr};
    fragmentShader = vk::raii::ShaderModule{nullptr};
    vertexShader = vk::raii::ShaderModule{nullptr};

    if (control != nullptr) {
        // paramsBuffer is host-visible and persistently mapped (VMA_ALLOCATION_CREATE_MAPPED_BIT):
        // it is mapped for its whole lifetime, so there is no explicit vmaMapMemory to balance and
        // vmaUnmapMemory must never be called for the creation mapping. vmaDestroyBuffer below
        // releases that mapping.
        paramsMapped = nullptr;
        destroyBufferVma(*control, paramsBuffer, paramsAllocation);
        destroyBufferVma(*control, stagedOverlayBuffer, stagedOverlayAllocation);
        overlayView = vk::raii::ImageView{nullptr};
        if (overlayImage != VK_NULL_HANDLE) {
            destroyImageVma(*control, overlayImage, overlayAllocation);
        }
    } else {
        paramsMapped = nullptr;
        paramsBuffer = VK_NULL_HANDLE;
        paramsAllocation = VK_NULL_HANDLE;
        stagedOverlayBuffer = VK_NULL_HANDLE;
        stagedOverlayAllocation = VK_NULL_HANDLE;
        overlayImage = VK_NULL_HANDLE;
        overlayAllocation = VK_NULL_HANDLE;
    }
    overlayWidth = 0;
    overlayHeight = 0;
    overlayToken = 0;
    stagedOverlayToken = 0;
    overlayRowBytes = 0;
    stagedOverlayPending = false;
    overlayImageInitialized = false;
    pipelineFormat = VK_FORMAT_UNDEFINED;
    builtSwapchain = VK_NULL_HANDLE;
    builtImage = VK_NULL_HANDLE;
    builtGeneration = 0;
    builtFormat = VK_FORMAT_UNDEFINED;
    builtFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    builtImageCount = 0;
    deviceResourcesReady = false;
}

bool PresentImagePipeline::ensureDeviceResources(
    const std::shared_ptr<DeviceAllocatorState>& deviceControl, std::string& message) {
    // A partial failure (e.g. the fragment shader or the uniform buffer) must not be treated as
    // ready merely because the vertex shader exists. If the device control changed, rebuild from
    // scratch so a later attempt fails closed or safely rebuilds.
    if (control != nullptr && control == deviceControl && deviceResourcesReady) {
        return true;
    }
    // Rebuild from scratch on a partial failure or a device change. The previous pipeline/render
    // pass/views/framebuffers and the uniform/overlay buffers are released in dependency order.
    // The pipeline is only created on the owner thread with no submission in flight.
    resetDeviceResources();
    control = deviceControl;
    const auto* dispatcher = deviceControl->device.getDispatcher();
    if (faultAt(0U)) {
        message = "injected present device-resource fault before the vertex shader";
        return false;
    }

    VkShaderModuleCreateInfo vertexInfo{};
    vertexInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertexInfo.codeSize = vulkan_detail::kViewerPresentVertByteCount;
    vertexInfo.pCode = vulkan_detail::kViewerPresentVertSpirv;
    VkShaderModule rawVertex = VK_NULL_HANDLE;
    if (dispatcher->vkCreateShaderModule(*deviceControl->device, &vertexInfo, nullptr,
                                         &rawVertex) != VK_SUCCESS) {
        message = "the viewer present vertex shader could not be created";
        return false;
    }
    vertexShader = vk::raii::ShaderModule(deviceControl->device, rawVertex);
    if (faultAt(1U)) {
        message = "injected present device-resource fault after the vertex shader";
        return false;
    }

    VkShaderModuleCreateInfo fragmentInfo = vertexInfo;
    fragmentInfo.codeSize = vulkan_detail::kViewerPresentFragByteCount;
    fragmentInfo.pCode = vulkan_detail::kViewerPresentFragSpirv;
    VkShaderModule rawFragment = VK_NULL_HANDLE;
    if (dispatcher->vkCreateShaderModule(*deviceControl->device, &fragmentInfo, nullptr,
                                         &rawFragment) != VK_SUCCESS) {
        message = "the viewer present fragment shader could not be created";
        return false;
    }
    fragmentShader = vk::raii::ShaderModule(deviceControl->device, rawFragment);

    const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    VkDescriptorSetLayout rawLayout = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorSetLayout(*deviceControl->device, &layoutInfo, nullptr,
                                                &rawLayout) != VK_SUCCESS) {
        message = "the viewer present descriptor layout could not be created";
        return false;
    }
    setLayout = vk::raii::DescriptorSetLayout(deviceControl->device, rawLayout);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &rawLayout;
    VkPipelineLayout rawPipelineLayout = VK_NULL_HANDLE;
    if (dispatcher->vkCreatePipelineLayout(*deviceControl->device, &pipelineLayoutInfo, nullptr,
                                           &rawPipelineLayout) != VK_SUCCESS) {
        message = "the viewer present pipeline layout could not be created";
        return false;
    }
    pipelineLayout = vk::raii::PipelineLayout(deviceControl->device, rawPipelineLayout);

    const std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
    }};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VkDescriptorPool rawPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorPool(*deviceControl->device, &poolInfo, nullptr, &rawPool) !=
        VK_SUCCESS) {
        message = "the viewer present descriptor pool could not be created";
        return false;
    }
    descriptorPool = vk::raii::DescriptorPool(deviceControl->device, rawPool);

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = rawPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &rawLayout;
    VkDescriptorSet rawSet = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateDescriptorSets(*deviceControl->device, &allocateInfo, &rawSet) !=
        VK_SUCCESS) {
        message = "the viewer present descriptor set could not be allocated";
        return false;
    }
    descriptorSet = vk::raii::DescriptorSet(deviceControl->device, rawSet, rawPool);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler rawSampler = VK_NULL_HANDLE;
    if (dispatcher->vkCreateSampler(*deviceControl->device, &samplerInfo, nullptr, &rawSampler) !=
        VK_SUCCESS) {
        message = "the viewer present sampler could not be created";
        return false;
    }
    sampler = vk::raii::Sampler(deviceControl->device, rawSampler);

    VmaAllocationInfo paramsInfo{};
    if (!createBufferVma(*deviceControl, sizeof(PresentImageUniforms),
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT,
                         paramsBuffer, paramsAllocation, paramsInfo) ||
        paramsInfo.pMappedData == nullptr) {
        message = "the viewer present uniform buffer could not be created";
        return false;
    }
    paramsMapped = paramsInfo.pMappedData;
    if (faultAt(2U)) {
        message = "injected present device-resource fault after the uniform buffer";
        return false;
    }
    deviceResourcesReady = true;
    return true;
}

void setPresentDeviceResourceFaultForTesting(const std::uint32_t stage) noexcept {
    gDeviceResourceFault.store(stage, std::memory_order_release);
}

void clearPresentDeviceResourceFaultForTesting() noexcept {
    gDeviceResourceFault.store(UINT32_MAX, std::memory_order_release);
}

bool PresentImagePipeline::ensureSwapchainResources(
    presentation_detail::SwapchainResources& resources, std::string& message) {
    if (control == nullptr || !deviceResourcesReady) {
        message = "the present pipeline has no ready device resources";
        return false;
    }
    const VkSwapchainKHR handle = *resources.swapchain;
    // The offscreen test target has no swapchain handle, so the first target image and the
    // configurable finalLayout are part of the render-pass/pipeline cache key as well.
    const VkImage firstImage = resources.images.empty() ? VK_NULL_HANDLE : resources.images.front();
    // A real swapchain keeps stable image handles across frames, so the cached views and
    // framebuffers stay valid. The offscreen test target has no swapchain handle and VMA may
    // recycle a destroyed image's raw handle, so it must rebuild views/framebuffers every
    // frame; only the format/layout-keyed render pass and pipeline are shared. The swapchain
    // generation counter (not the raw handles) is the identity: a destroyed generation can be
    // recycled with identical bits.
    const bool offscreenTarget = handle == VK_NULL_HANDLE;
    if (!offscreenTarget && builtSwapchain == handle && builtGeneration == resources.generation &&
        builtImage == firstImage && builtFormat == resources.format &&
        builtFinalLayout == resources.finalLayout && builtImageCount == resources.images.size() &&
        *pipeline) {
        return true;
    }
    const auto* dispatcher = control->device.getDispatcher();
    if (builtFormat != resources.format || builtFinalLayout != resources.finalLayout ||
        !*pipeline) {
        VkAttachmentDescription color;
        std::memset(&color, 0, sizeof(color));
        color.format = resources.format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = resources.finalLayout;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &color;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        VkRenderPass rawRenderPass = VK_NULL_HANDLE;
        if (dispatcher->vkCreateRenderPass(*control->device, &renderPassInfo, nullptr,
                                           &rawRenderPass) != VK_SUCCESS) {
            message = "the viewer present render pass could not be created";
            return false;
        }
        renderPass = vk::raii::RenderPass(control->device, rawRenderPass);

        VkPipelineShaderStageCreateInfo stages[2];
        std::memset(stages, 0, sizeof(stages));
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = *vertexShader;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = *fragmentShader;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0F;
        VkPipelineMultisampleStateCreateInfo multisample;
        std::memset(&multisample, 0, sizeof(multisample));
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blendAttachment.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blendAttachment;
        const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = *pipelineLayout;
        pipelineInfo.renderPass = rawRenderPass;
        pipelineInfo.subpass = 0;
        VkPipeline rawPipeline = VK_NULL_HANDLE;
        if (dispatcher->vkCreateGraphicsPipelines(*control->device, VK_NULL_HANDLE, 1,
                                                  &pipelineInfo, nullptr,
                                                  &rawPipeline) != VK_SUCCESS) {
            message = "the viewer present graphics pipeline could not be created";
            return false;
        }
        pipeline = vk::raii::Pipeline(control->device, rawPipeline);
        builtFormat = resources.format;
        builtFinalLayout = resources.finalLayout;
    }

    // A generation replacement/retirement invalidates every cached view, framebuffer, and the
    // image identity before the old generation is forgotten: two recreates before the next present
    // would otherwise recycle old handles.
    swapchainViews.clear();
    framebuffers.clear();
    swapchainViews.reserve(resources.images.size());
    framebuffers.reserve(resources.images.size());
    for (const VkImage image : resources.images) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = resources.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView rawView = VK_NULL_HANDLE;
        if (dispatcher->vkCreateImageView(*control->device, &viewInfo, nullptr, &rawView) !=
            VK_SUCCESS) {
            message = "a swapchain image view could not be created";
            return false;
        }
        swapchainViews.emplace_back(control->device, rawView);
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = *renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &rawView;
        framebufferInfo.width = resources.width;
        framebufferInfo.height = resources.height;
        framebufferInfo.layers = 1;
        VkFramebuffer rawFramebuffer = VK_NULL_HANDLE;
        if (dispatcher->vkCreateFramebuffer(*control->device, &framebufferInfo, nullptr,
                                            &rawFramebuffer) != VK_SUCCESS) {
            message = "a swapchain framebuffer could not be created";
            return false;
        }
        framebuffers.emplace_back(control->device, rawFramebuffer);
    }
    builtSwapchain = handle;
    builtImage = firstImage;
    builtGeneration = resources.generation;
    builtImageCount = static_cast<std::uint32_t>(resources.images.size());
    return true;
}

void PresentImagePipeline::invalidateDisplayView() noexcept {
    displayView = vk::raii::ImageView{nullptr};
    displayImage = VK_NULL_HANDLE;
    displayGeneration = 0;
}

bool PresentImagePipeline::ensureDisplayView(DeviceAllocatorState& /*deviceControl*/,
                                             const GpuDisplayImageImpl& display,
                                             const std::shared_ptr<const GpuDisplayImage>& pin,
                                             const bool previousRenderComplete,
                                             std::string& message) {
    if (control == nullptr) {
        message = "the present pipeline has no device";
        return false;
    }
    if (displayImage == display.image && displayGeneration == display.generation && *displayView) {
        // The cached view already references this image/generation. Refresh the strong pin when the
        // previous frame is proven complete, without recreating the view for an unchanged frame.
        if (previousRenderComplete) {
            pinnedInput = pin;
        }
        return true;
    }
    // A new view must be created. The old view may still be sampled by an in-flight frame, so it is
    // only released once the previous render fence is proven complete; otherwise the view is
    // retained (and replaced) after its pin is refreshed below. Creating a new view first and then
    // committing view+pin together keeps the cache identity consistent on every path.
    vk::raii::ImageView newView{nullptr};
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = display.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView rawView = VK_NULL_HANDLE;
    if (control->device.getDispatcher()->vkCreateImageView(*control->device, &viewInfo, nullptr,
                                                           &rawView) != VK_SUCCESS) {
        message = "a display image view could not be created";
        return false;
    }
    newView = vk::raii::ImageView(control->device, rawView);
    // Commit the new view and its strong pin together. The pin keeps the source VkImage alive for
    // as long as this cached view survives, even if the caller releases its own reference.
    displayView = std::move(newView);
    displayImage = display.image;
    displayGeneration = display.generation;
    pinnedInput = pin;
    return true;
}

bool PresentImagePipeline::ensureOverlay(DeviceAllocatorState& /*deviceControl*/,
                                         const GpuPresentOverlay& overlay, std::string& message) {
    if (control == nullptr) {
        message = "the present pipeline has no device";
        return false;
    }
    constexpr std::uint64_t kMaxOverlayBytes = 64ULL * 1024ULL * 1024ULL;
    const bool requested = overlay.token != 0U && overlay.pixels != nullptr && overlay.width > 0U &&
                           overlay.height > 0U;
    if (overlay.token != 0U && !requested) {
        message = "an overlay token was supplied without valid premultiplied RGBA8 pixels";
        return false;
    }
    if (!requested) {
        stagedOverlayPending = false;
        overlayToken = 0;
        return true;
    }
    if (static_cast<std::uint64_t>(overlay.width) * overlay.height * 4ULL > kMaxOverlayBytes) {
        message = "the overlay exceeds the bounded resident overlay budget";
        return false;
    }
    const std::uint64_t rowBytes =
        overlay.rowStrideBytes != 0U ? overlay.rowStrideBytes : overlay.width * 4U;
    if (rowBytes < static_cast<std::uint64_t>(overlay.width) * 4ULL) {
        message = "the overlay row stride is smaller than one RGBA8 row";
        return false;
    }
    if (overlay.token == overlayToken && *overlayView && !stagedOverlayPending &&
        overlayWidth == overlay.width && overlayHeight == overlay.height) {
        return true;
    }
    // Free the previous staged buffer: the caller acquires only after the previous render fence is
    // known complete, so no submission still references it.
    destroyBufferVma(*control, stagedOverlayBuffer, stagedOverlayAllocation);
    stagedOverlayPending = false;

    const bool needsImage = overlayImage == VK_NULL_HANDLE || overlayWidth != overlay.width ||
                            overlayHeight != overlay.height;
    if (needsImage) {
        overlayView = vk::raii::ImageView{nullptr};
        if (overlayImage != VK_NULL_HANDLE) {
            destroyImageVma(*control, overlayImage, overlayAllocation);
        }
        VkImage rawOverlay = VK_NULL_HANDLE;
        if (!createOverlayImageVma(*control, overlay.width, overlay.height, rawOverlay,
                                   overlayAllocation)) {
            message = "the overlay image could not be created";
            return false;
        }
        overlayImage = rawOverlay;
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = rawOverlay;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView rawView = VK_NULL_HANDLE;
        if (control->device.getDispatcher()->vkCreateImageView(*control->device, &viewInfo, nullptr,
                                                               &rawView) != VK_SUCCESS) {
            message = "the overlay image view could not be created";
            return false;
        }
        overlayView = vk::raii::ImageView(control->device, rawView);
        overlayWidth = overlay.width;
        overlayHeight = overlay.height;
    }

    const std::uint64_t stagingBytes = rowBytes * overlay.height;
    VmaAllocationInfo stagingInfo{};
    if (!createBufferVma(*control, stagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT,
                         stagedOverlayBuffer, stagedOverlayAllocation, stagingInfo) ||
        stagingInfo.pMappedData == nullptr) {
        message = "the overlay staging buffer could not be created";
        return false;
    }
    auto* const destination = static_cast<std::uint8_t*>(stagingInfo.pMappedData);
    for (std::uint32_t row = 0; row < overlay.height; ++row) {
        std::memcpy(destination + static_cast<std::size_t>(row) * rowBytes,
                    overlay.pixels + static_cast<std::size_t>(row) * rowBytes,
                    static_cast<std::size_t>(overlay.width) * 4U);
    }
    vmaFlushAllocation(control->allocator, stagedOverlayAllocation, 0, VK_WHOLE_SIZE);
    stagedOverlayToken = overlay.token;
    overlayRowBytes = static_cast<std::uint32_t>(rowBytes);
    stagedOverlayPending = true;
    return true;
}

} // namespace present_image_detail
} // namespace bloom::render
