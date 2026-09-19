#include <bloom/render/gpu_solid.hpp>

#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "shaders/solid_spirv.inc"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace bloom::render {
namespace {

using vulkan_detail::DeviceAllocatorState;
using GpuRendererAccess = bloom::render::GpuRendererAccess;

constexpr std::uint32_t kWorkgroupSizeX = 256;
constexpr std::uint64_t kDrainTimeoutNanoseconds = 2ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr std::uint64_t kMaxImageBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::int32_t kMaxQuarantines = 4;

std::atomic<std::int32_t> g_quarantineCount{0};
std::atomic<bool> g_teardownIncomplete{false};

struct SolidPushConstants final {
    float pixel[4];
    std::uint32_t width;
    std::uint32_t height;
};
static_assert(sizeof(SolidPushConstants) == 24);

[[nodiscard]] GpuSolidDiagnostic makeDiagnostic(const GpuSolidDiagnosticCode code,
                                                std::string message) {
    return GpuSolidDiagnostic{code, std::move(message)};
}

[[nodiscard]] bool quarantineAllowed() noexcept {
    return g_quarantineCount.load() < kMaxQuarantines;
}

void noteQuarantine() noexcept {
    g_quarantineCount.fetch_add(1);
    g_teardownIncomplete.store(true);
}

} // namespace

struct GpuSolid::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return control != nullptr && control->owner == std::this_thread::get_id();
    }
    void fail(const GpuSolidDiagnosticCode code, std::string message) {
        jobState = GpuSolidJobState::Failure;
        jobDiagnostic = makeDiagnostic(code, std::move(message));
    }
    void clearJob() {
        jobState = GpuSolidJobState::Idle;
        jobDiagnostic = GpuSolidDiagnostic{};
        discardRequested.store(false);
        residentImage.reset();
    }
    void releaseResident() { residentImage.reset(); }
    [[nodiscard]] bool createPipeline();
    // Bounded owner-thread drain. Returns true when the submission is proved
    // retired.
    [[nodiscard]] bool drainAndRetire() noexcept;

    std::thread::id owner;
    std::shared_ptr<DeviceAllocatorState> control;
    GpuSolidBudgets budgets;
    std::uint32_t expectedGeneration = 0;

    vk::raii::ShaderModule shaderModule{nullptr};
    vk::raii::DescriptorSetLayout descriptorSetLayout{nullptr};
    vk::raii::PipelineLayout pipelineLayout{nullptr};
    vk::raii::Pipeline pipeline{nullptr};
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet descriptorSet{nullptr};
    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};

    std::unique_ptr<GpuImage> residentImage;

    GpuSolidJobState jobState = GpuSolidJobState::Idle;
    bool queueSubmitted = false;
    bool deviceLost = false;
    std::atomic<bool> discardRequested{false};
    GpuSolidDiagnostic jobDiagnostic;
    GpuSolidDiagnostic createDiagnostic;
};

GpuSolid::GpuSolid(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuSolid::GpuSolid(GpuSolid&& other) noexcept = default;
GpuSolid& GpuSolid::operator=(GpuSolid&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuSolid::~GpuSolid() { releaseImpl(); }

void GpuSolid::releaseImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->onOwnerThread()) {
        noteQuarantine();
        [[maybe_unused]] const auto* const quarantined = impl_.release();
        return;
    }
    if (!impl_->drainAndRetire()) {
        noteQuarantine();
        [[maybe_unused]] const auto* const quarantined = impl_.release();
        return;
    }
    impl_.reset();
}

GpuSolidJobState GpuSolid::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuSolidJobState::Idle;
}
const GpuSolidDiagnostic& GpuSolid::diagnostic() const noexcept {
    static const GpuSolidDiagnostic none{};
    if (impl_ != nullptr) {
        return impl_->jobDiagnostic;
    }
    return none;
}
bool GpuSolid::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto deviceState = GpuRendererAccess::state(device);
    return deviceState != nullptr && deviceState == impl_->control;
}

bool GpuSolid::Impl::drainAndRetire() noexcept {
    if (!queueSubmitted) {
        return true;
    }
    if (deviceLost) {
        queueSubmitted = false;
        return true;
    }
    const VkFence rawFence = static_cast<VkFence>(*fence);
    const VkResult waited = control->device.getDispatcher()->vkWaitForFences(
        static_cast<VkDevice>(*control->device), 1, &rawFence, VK_TRUE, kDrainTimeoutNanoseconds);
    if (waited == VK_SUCCESS || waited == VK_ERROR_DEVICE_LOST) {
        queueSubmitted = false;
        deviceLost = deviceLost || waited == VK_ERROR_DEVICE_LOST;
        return true;
    }
    return false;
}

GpuSolid::Impl::~Impl() {
    assert(owner == std::this_thread::get_id());
    releaseResident();
}

bool GpuSolid::Impl::createPipeline() {
    const VkDevice rawDevice = static_cast<VkDevice>(*control->device);
    const auto* dispatcher = control->device.getDispatcher();

    vk::ShaderModuleCreateInfo shaderInfo{};
    shaderInfo.codeSize = vulkan_detail::kSolidSpirvByteCount;
    shaderInfo.pCode = vulkan_detail::kSolidSpirvCode;
    VkShaderModule rawShader = VK_NULL_HANDLE;
    if (dispatcher->vkCreateShaderModule(
            rawDevice, reinterpret_cast<const VkShaderModuleCreateInfo*>(&shaderInfo), nullptr,
            &rawShader) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuSolidDiagnosticCode::ShaderRejected,
                                          "the embedded SolidV1 shader module was rejected");
        return false;
    }
    shaderModule = vk::raii::ShaderModule(control->device, rawShader);

    vk::DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = vk::DescriptorType::eStorageImage;
    binding.descriptorCount = 1;
    binding.stageFlags = vk::ShaderStageFlagBits::eCompute;
    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VkDescriptorSetLayout rawLayout = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorSetLayout(
            rawDevice, reinterpret_cast<const VkDescriptorSetLayoutCreateInfo*>(&layoutInfo),
            nullptr, &rawLayout) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuSolidDiagnosticCode::ShaderRejected,
                                          "the SolidV1 descriptor set layout was rejected");
        return false;
    }
    descriptorSetLayout = vk::raii::DescriptorSetLayout(control->device, rawLayout);

    vk::PushConstantRange pushRange{};
    pushRange.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pushRange.offset = 0;
    pushRange.size = static_cast<std::uint32_t>(sizeof(SolidPushConstants));
    const vk::DescriptorSetLayout setLayout = *descriptorSetLayout;
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout rawPipelineLayout = VK_NULL_HANDLE;
    if (dispatcher->vkCreatePipelineLayout(
            rawDevice, reinterpret_cast<const VkPipelineLayoutCreateInfo*>(&pipelineLayoutInfo),
            nullptr, &rawPipelineLayout) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuSolidDiagnosticCode::ShaderRejected,
                                          "the SolidV1 pipeline layout was rejected");
        return false;
    }
    pipelineLayout = vk::raii::PipelineLayout(control->device, rawPipelineLayout);

    vk::ComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.stage.stage = vk::ShaderStageFlagBits::eCompute;
    pipelineInfo.stage.module = *shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = *pipelineLayout;
    VkPipeline rawPipeline = VK_NULL_HANDLE;
    if (dispatcher->vkCreateComputePipelines(
            rawDevice, VK_NULL_HANDLE, 1,
            reinterpret_cast<const VkComputePipelineCreateInfo*>(&pipelineInfo), nullptr,
            &rawPipeline) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuSolidDiagnosticCode::ShaderRejected,
                                          "the embedded SolidV1 compute pipeline was rejected");
        return false;
    }
    pipeline = vk::raii::Pipeline(control->device, rawPipeline);

    const vk::DescriptorPoolSize poolSize{vk::DescriptorType::eStorageImage, 1};
    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool rawPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorPool(
            rawDevice, reinterpret_cast<const VkDescriptorPoolCreateInfo*>(&poolInfo), nullptr,
            &rawPool) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                          "the SolidV1 descriptor pool could not be created");
        return false;
    }
    descriptorPool = vk::raii::DescriptorPool(control->device, rawPool);

    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &setLayout;
    VkDescriptorSet rawSet = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateDescriptorSets(
            rawDevice, reinterpret_cast<const VkDescriptorSetAllocateInfo*>(&allocateInfo),
            &rawSet) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                          "the SolidV1 descriptor set could not be allocated");
        return false;
    }
    descriptorSet = vk::raii::DescriptorSet(control->device, rawSet, *descriptorPool);

    vk::CommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    commandPoolInfo.queueFamilyIndex = control->computeQueueFamily;
    VkCommandPool rawCommandPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateCommandPool(
            rawDevice, reinterpret_cast<const VkCommandPoolCreateInfo*>(&commandPoolInfo), nullptr,
            &rawCommandPool) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                          "the SolidV1 command pool could not be created");
        return false;
    }
    commandPool = vk::raii::CommandPool(control->device, rawCommandPool);

    vk::CommandBufferAllocateInfo commandBufferInfo{};
    commandBufferInfo.commandPool = *commandPool;
    commandBufferInfo.level = vk::CommandBufferLevel::ePrimary;
    commandBufferInfo.commandBufferCount = 1;
    VkCommandBuffer rawCommandBuffer = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateCommandBuffers(
            rawDevice, reinterpret_cast<const VkCommandBufferAllocateInfo*>(&commandBufferInfo),
            &rawCommandBuffer) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                          "the SolidV1 command buffer could not be allocated");
        return false;
    }
    commandBuffer = vk::raii::CommandBuffer(control->device, rawCommandBuffer, *commandPool);

    vk::FenceCreateInfo fenceInfo{};
    VkFence rawFence = VK_NULL_HANDLE;
    if (dispatcher->vkCreateFence(rawDevice, reinterpret_cast<const VkFenceCreateInfo*>(&fenceInfo),
                                  nullptr, &rawFence) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                          "the SolidV1 fence could not be created");
        return false;
    }
    fence = vk::raii::Fence(control->device, rawFence);
    return true;
}

GpuSolidCreateResult GpuSolid::create(GpuDevice& device, const GpuSolidBudgets& budgets) {
    if (budgets.maxImageBytes == 0 || budgets.maxImageBytes > kMaxImageBytes) {
        return {nullptr, makeDiagnostic(GpuSolidDiagnosticCode::InvalidArgument,
                                        "the SolidV1 budget is out of range")};
    }
    if (device.state() != GpuDeviceState::Ready) {
        return {nullptr, makeDiagnostic(GpuSolidDiagnosticCode::DeviceUnavailable,
                                        "the GPU device is not Ready")};
    }
    if (GpuRendererAccess::owner(device) != std::this_thread::get_id()) {
        return {nullptr, makeDiagnostic(GpuSolidDiagnosticCode::WrongThread,
                                        "the SolidV1 pipeline must be created on the device owner "
                                        "thread")};
    }
    if (!quarantineAllowed()) {
        return {nullptr, makeDiagnostic(GpuSolidDiagnosticCode::DeviceUnavailable,
                                        "too many undrained GPU generations are quarantined")};
    }
    auto control = GpuRendererAccess::state(device);
    if (control == nullptr) {
        return {nullptr, makeDiagnostic(GpuSolidDiagnosticCode::DeviceUnavailable,
                                        "the GPU device exposes no renderer state")};
    }
    auto impl = std::make_unique<Impl>();
    impl->owner = std::this_thread::get_id();
    impl->control = std::move(control);
    impl->budgets = budgets;
    impl->expectedGeneration = impl->control->generation;
    if (!impl->createPipeline()) {
        return {nullptr, impl->createDiagnostic};
    }
    return {std::unique_ptr<GpuSolid>(new GpuSolid(std::move(impl))), GpuSolidDiagnostic{}};
}

GpuSolidDiagnostic GpuSolid::begin(const GpuSolidParameters& parameters,
                                   const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return makeDiagnostic(GpuSolidDiagnosticCode::DeviceUnavailable,
                              "the SolidV1 pipeline is not initialized");
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return makeDiagnostic(GpuSolidDiagnosticCode::WrongThread,
                              "begin must run on the device owner thread");
    }
    if (impl.deviceLost) {
        return makeDiagnostic(GpuSolidDiagnosticCode::DeviceLost,
                              "the device was lost; this generation must not be reused");
    }
    if (impl.queueSubmitted || impl.jobState == GpuSolidJobState::Pending) {
        return makeDiagnostic(GpuSolidDiagnosticCode::Busy, "one job is already in flight");
    }
    const std::uint32_t width = parameters.dataWindow.extent().width();
    const std::uint32_t height = parameters.dataWindow.extent().height();
    if (width == 0 || height == 0) {
        return makeDiagnostic(GpuSolidDiagnosticCode::InvalidArgument, "the data window is empty");
    }
    const std::uint64_t required = static_cast<std::uint64_t>(width) * height * sizeof(Rgba32f);
    const std::uint64_t allowed =
        byteBudget < impl.budgets.maxImageBytes ? byteBudget : impl.budgets.maxImageBytes;
    if (required > allowed) {
        return makeDiagnostic(GpuSolidDiagnosticCode::OverBudget,
                              "the resident image exceeds the configured or requested byte budget");
    }
    if (impl.control->generation != impl.expectedGeneration) {
        impl.deviceLost = true;
        return makeDiagnostic(GpuSolidDiagnosticCode::DeviceLost,
                              "the device generation changed; this pipeline must not be reused");
    }
    const SolidImageSupport support = querySolidImageSupport(*impl.control, width, height);
    if (!support.supported) {
        return makeDiagnostic(GpuSolidDiagnosticCode::Unsupported, support.reason);
    }
    if (required > support.maxImageBytes) {
        return makeDiagnostic(GpuSolidDiagnosticCode::OverBudget,
                              "the resident image exceeds the device resource limit");
    }

    impl.clearJob();
    auto resident = std::make_unique<GpuImageImpl>();
    resident->state = impl.control;
    resident->dataWindow = parameters.dataWindow;
    resident->displayWindow = parameters.displayWindow;
    resident->pixelAspect = parameters.pixelAspect;
    resident->generation = impl.control->generation;
    if (!createResidentImage(*impl.control, width, height, *resident)) {
        return makeDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                              "the resident image could not be allocated");
    }
    GpuImageImpl* const residentRaw = resident.get();
    impl.residentImage = std::make_unique<GpuImage>(makeGpuImage(std::move(resident)));

    const VkDevice rawDevice = static_cast<VkDevice>(*impl.control->device);
    const auto* dispatcher = impl.control->device.getDispatcher();
    const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*impl.commandBuffer);
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    if (dispatcher->vkResetFences(rawDevice, 1, &rawFence) != VK_SUCCESS ||
        dispatcher->vkResetCommandBuffer(rawCommandBuffer, 0) != VK_SUCCESS) {
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                  "the SolidV1 fence or command buffer could not be reset");
        return impl.jobDiagnostic;
    }

    vk::DescriptorImageInfo imageInfo{};
    imageInfo.imageView = residentRaw->view;
    imageInfo.imageLayout = vk::ImageLayout::eGeneral;
    vk::WriteDescriptorSet write{};
    write.dstSet = *impl.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eStorageImage;
    write.pImageInfo = &imageInfo;
    dispatcher->vkUpdateDescriptorSets(
        rawDevice, 1, reinterpret_cast<const VkWriteDescriptorSet*>(&write), 0, nullptr);

    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    if (dispatcher->vkBeginCommandBuffer(
            rawCommandBuffer, reinterpret_cast<const VkCommandBufferBeginInfo*>(&beginInfo)) !=
        VK_SUCCESS) {
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                  "the SolidV1 command buffer could not begin");
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

    impl.commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, *impl.pipeline);
    impl.commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *impl.pipelineLayout, 0,
                                          {*impl.descriptorSet}, {});
    SolidPushConstants push{};
    push.pixel[0] = parameters.pixel.red();
    push.pixel[1] = parameters.pixel.green();
    push.pixel[2] = parameters.pixel.blue();
    push.pixel[3] = parameters.pixel.alpha();
    push.width = width;
    push.height = height;
    impl.commandBuffer.pushConstants(*impl.pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0,
                                     static_cast<std::uint32_t>(sizeof(SolidPushConstants)), &push);
    const std::uint64_t groupCount =
        (static_cast<std::uint64_t>(width) + kWorkgroupSizeX - 1U) / kWorkgroupSizeX;
    const auto groups = static_cast<std::uint32_t>(groupCount);
    impl.commandBuffer.dispatch(groups, height, 1);

    VkImageMemoryBarrier toRead = toGeneral;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toRead);

    if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                  "the SolidV1 command buffer could not end");
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
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceLost, "the device was lost during submission");
        return impl.jobDiagnostic;
    }
    if (submitted != VK_SUCCESS) {
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                  "the SolidV1 dispatch could not submit");
        return impl.jobDiagnostic;
    }
    impl.queueSubmitted = true;
    impl.jobState = GpuSolidJobState::Pending;
    impl.jobDiagnostic = GpuSolidDiagnostic{};
    return {};
}

GpuSolidPollResult GpuSolid::poll() {
    if (impl_ == nullptr) {
        return GpuSolidPollResult::Failure;
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return GpuSolidPollResult::WrongThread;
    }
    if (impl.jobState == GpuSolidJobState::Ready) {
        return GpuSolidPollResult::Ready;
    }
    const bool pending = impl.jobState == GpuSolidJobState::Pending;
    // A live submission can outlive a Failure job state (an unexpected fence
    // result). Keep querying the fence in that case so a later successful owner
    // poll retires it without publishing a frame.
    if (!pending && !impl.queueSubmitted) {
        return GpuSolidPollResult::Failure;
    }
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    const VkResult status = impl.control->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*impl.control->device), rawFence);
    if (status == VK_NOT_READY) {
        return pending ? GpuSolidPollResult::Pending : GpuSolidPollResult::Failure;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.queueSubmitted = false;
        if (pending) {
            impl.fail(GpuSolidDiagnosticCode::DeviceLost, "the device was lost while polling");
        }
        return GpuSolidPollResult::Failure;
    }
    if (status != VK_SUCCESS) {
        // Unknown status: the fence is NOT proved signalled; keep queueSubmitted so
        // the submission stays retired-only-on-known-completion.
        if (pending) {
            impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                      "the SolidV1 fence returned an unexpected status; the "
                      "submission is not retired");
        }
        return GpuSolidPollResult::Failure;
    }
    impl.queueSubmitted = false;
    if (!pending) {
        // Retired after an already-published failure; nothing to publish.
        return GpuSolidPollResult::Failure;
    }
    if (impl.discardRequested.load()) {
        impl.clearJob();
        impl.fail(GpuSolidDiagnosticCode::Cancelled, "the SolidV1 job was cancelled");
        return GpuSolidPollResult::Failure;
    }
    if (impl.residentImage == nullptr) {
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable, "the resident image is missing");
        return GpuSolidPollResult::Failure;
    }
    impl.jobState = GpuSolidJobState::Ready;
    impl.jobDiagnostic = GpuSolidDiagnostic{};
    return GpuSolidPollResult::Ready;
}

const GpuImage* GpuSolid::image() const noexcept {
    if (impl_ == nullptr || impl_->jobState != GpuSolidJobState::Ready) {
        return nullptr;
    }
    return impl_->residentImage.get();
}

GpuImage GpuSolid::takeImage() noexcept {
    if (impl_ == nullptr || impl_->residentImage == nullptr) {
        return GpuImage{};
    }
    GpuImage taken = std::move(*impl_->residentImage);
    impl_->residentImage.reset();
    impl_->clearJob();
    return taken;
}

GpuImageReadback GpuSolid::readback() noexcept {
    if (impl_ == nullptr || impl_->residentImage == nullptr) {
        GpuImageReadback result;
        result.code = GpuImageReadbackCode::DeviceUnavailable;
        result.message = "no resident image to read back";
        return result;
    }
    return readbackResidentImage(*impl_->residentImage, impl_->budgets.maxImageBytes);
}

void GpuSolid::cancel() noexcept {
    if (impl_ != nullptr) {
        impl_->discardRequested.store(true);
    }
}

bool GpuSolid::teardownDrainIncomplete() noexcept { return g_teardownIncomplete.load(); }

} // namespace bloom::render
