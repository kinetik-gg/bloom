#include "gpu_presentation_target_private.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::render {
namespace {

// Bounded, non-destroying process quarantine. A target whose native submission could not be proven
// retired is held as a raw, never-deleted Impl pointer (swapchain, per-image resources, and the
// shared device allocator control) rather than destroyed. The slots are fixed-size and the failure
// path allocates nothing; the fuse is latched and create() then fails closed so no further
// generations can accrue. No shared_ptr/vector destructor can run Vulkan teardown at process exit.
std::mutex gTargetQuarantineMutex;
constexpr std::size_t kMaxQuarantinedTargets = 8;
std::array<void*, kMaxQuarantinedTargets> gTargetQuarantine{};
std::size_t gTargetQuarantineCount = 0;
std::atomic_bool gTargetTeardownDrainIncomplete{false};
std::atomic_bool gTargetCreationFused{false};

using presentation_detail::SwapchainResources;

struct PlannedSwapchain final {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent{};
    std::uint32_t imageCount = 0;
    VkSurfaceTransformFlagBitsKHR preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    GpuPresentationRetirement retirement = GpuPresentationRetirement::None;
};

// Validates the borrowed surface on the owner thread and plans the swapchain. Never touches the
// surface on an epoch/thread mismatch: validateBorrowedSurface rejects those before the driver.
[[nodiscard]] bool
planSwapchain(GpuDevice& device, const GpuPresentationTargetDescription& description,
              const std::shared_ptr<vulkan_detail::DeviceAllocatorState>& control,
              PlannedSwapchain& planned, GpuPresentationTargetCode& code, std::string& message) {
    const GpuSurfaceSupportResult support = device.validateBorrowedSurface(description.surface);
    if (support.status != GpuSurfaceSupport::Supported) {
        switch (support.status) {
        case GpuSurfaceSupport::WrongEpoch:
            code = GpuPresentationTargetCode::WrongEpoch;
            break;
        case GpuSurfaceSupport::WrongThread:
            code = GpuPresentationTargetCode::WrongThread;
            break;
        case GpuSurfaceSupport::UnsupportedByQueue:
            code = GpuPresentationTargetCode::UnsupportedSurface;
            break;
        case GpuSurfaceSupport::PresentationUnavailable:
        case GpuSurfaceSupport::InvalidArgument:
            code = GpuPresentationTargetCode::PresentationUnavailable;
            break;
        case GpuSurfaceSupport::DriverUnavailable:
        case GpuSurfaceSupport::Supported:
            code = GpuPresentationTargetCode::DriverUnavailable;
            break;
        }
        message = support.message;
        return false;
    }

    const GpuPresentationStatus status = device.presentationStatus();
    if (status.availability != GpuPresentationAvailability::Ready) {
        code = GpuPresentationTargetCode::PresentationUnavailable;
        message = status.detail.empty() ? "GPU presentation is not ready" : status.detail;
        return false;
    }
    if (status.present_fences) {
        planned.retirement = GpuPresentationRetirement::PresentFence;
    } else if (status.present_wait) {
        planned.retirement = GpuPresentationRetirement::PresentWait;
    } else {
        code = GpuPresentationTargetCode::NoRetirementMechanism;
        message =
            "no supported presentation-retirement mechanism; the CPU path remains the fallback";
        return false;
    }

    const auto* dispatcher = control->instance.getDispatcher();
    const VkSurfaceKHR surface =
        presentation_detail::toSurfaceHandle(description.surface.surface_bits);
    const VkPhysicalDevice physical = static_cast<VkPhysicalDevice>(*control->physicalDevice);
    if (dispatcher == nullptr || dispatcher->vkGetPhysicalDeviceSurfaceCapabilitiesKHR == nullptr ||
        dispatcher->vkGetPhysicalDeviceSurfaceFormatsKHR == nullptr ||
        dispatcher->vkGetPhysicalDeviceSurfacePresentModesKHR == nullptr) {
        code = GpuPresentationTargetCode::DriverUnavailable;
        message = "the driver exposes no surface capability queries";
        return false;
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    if (dispatcher->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities) !=
        VK_SUCCESS) {
        code = GpuPresentationTargetCode::DriverUnavailable;
        message = "surface capabilities could not be queried";
        return false;
    }
    std::uint32_t formatCount = 0;
    if (dispatcher->vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount,
                                                         nullptr) != VK_SUCCESS ||
        formatCount == 0U) {
        code = GpuPresentationTargetCode::UnsupportedSurface;
        message = "the surface offered no formats";
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (dispatcher->vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount,
                                                         formats.data()) != VK_SUCCESS) {
        code = GpuPresentationTargetCode::DriverUnavailable;
        message = "surface formats could not be enumerated";
        return false;
    }
    formats.resize(formatCount);
    std::uint32_t modeCount = 0;
    if (dispatcher->vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount,
                                                              nullptr) != VK_SUCCESS ||
        modeCount == 0U) {
        code = GpuPresentationTargetCode::UnsupportedSurface;
        message = "the surface offered no presentation modes";
        return false;
    }
    std::vector<VkPresentModeKHR> modes(modeCount);
    if (dispatcher->vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount,
                                                              modes.data()) != VK_SUCCESS) {
        code = GpuPresentationTargetCode::DriverUnavailable;
        message = "surface presentation modes could not be enumerated";
        return false;
    }
    modes.resize(modeCount);

    if (!presentation_detail::selectFormat(formats, planned.format, planned.colorSpace, message)) {
        code = GpuPresentationTargetCode::UnsupportedSurface;
        return false;
    }
    planned.presentMode = presentation_detail::selectPresentMode(modes);
    // The swapchain requests COLOR_ATTACHMENT | TRANSFER_DST; both must be advertised, not only the
    // clear's transfer destination.
    const VkImageUsageFlags kRequiredUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((capabilities.supportedUsageFlags & kRequiredUsage) != kRequiredUsage) {
        code = GpuPresentationTargetCode::UnsupportedSurface;
        message = "the surface does not support color-attachment and transfer-destination "
                  "swapchain images";
        return false;
    }
    planned.preTransform =
        (capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0U
            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
            : capabilities.currentTransform;
    planned.compositeAlpha = [&capabilities] {
        if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0U) {
            return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        }
        for (std::uint32_t bit = 1U; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1U) {
            if ((capabilities.supportedCompositeAlpha & bit) != 0U) {
                return static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
            }
        }
        return static_cast<VkCompositeAlphaFlagBitsKHR>(
            capabilities.supportedCompositeAlpha & ~(capabilities.supportedCompositeAlpha - 1U));
    }();
    planned.extent =
        presentation_detail::computeExtent(capabilities, description.width, description.height);
    planned.imageCount = presentation_detail::computeImageCount(capabilities);
    if (planned.extent.width == 0U || planned.extent.height == 0U) {
        code = GpuPresentationTargetCode::UnsupportedSurface;
        message = "the surface extent is empty";
        return false;
    }
    return true;
}

} // namespace

struct GpuPresentationTarget::Impl final {
    std::thread::id owner;
    // Non-owning: the target is created from, and must not outlive, this device. The header and
    // README state that the owner retires the target before destroying the device generation.
    GpuDevice* device = nullptr;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    GpuPresentationEpoch epoch;
    std::uint64_t surfaceBits = 0;
    GpuPresentationTargetDescription description;
    std::unique_ptr<SwapchainResources> resources;
    GpuPresentationTargetInfo info;
    bool deviceLost = false;
    bool retiring = false;
    bool retired = false;
    GpuPresentationTargetCode lastCode = GpuPresentationTargetCode::Ok;
    std::string lastMessage;
};

GpuPresentationTarget::GpuPresentationTarget(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GpuPresentationTarget::GpuPresentationTarget(GpuPresentationTarget&& other) noexcept
    : impl_(std::move(other.impl_)) {}

GpuPresentationTarget& GpuPresentationTarget::operator=(GpuPresentationTarget&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

GpuPresentationTarget::~GpuPresentationTarget() { releaseImpl(); }

void GpuPresentationTarget::releaseImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    const bool foreignThread = std::this_thread::get_id() != impl_->owner;
    bool proven = false;
    if (!foreignThread) {
        if (impl_->retired || impl_->resources == nullptr) {
            proven = true;
        } else if (!impl_->deviceLost && impl_->control != nullptr) {
            // Bounded owner-thread drain: beginRetire legally discards an acquired-unpresented
            // image, then non-blocking polling only, never a queue/device idle wait. If the
            // presentation engine cannot be proven done, the whole impl is retained.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (std::chrono::steady_clock::now() < deadline) {
                const GpuPresentationTargetCode code = beginRetire();
                if (code == GpuPresentationTargetCode::Retired) {
                    proven = true;
                    break;
                }
                if (code == GpuPresentationTargetCode::DeviceLost) {
                    break;
                }
                std::this_thread::yield();
            }
        }
    }
    if (proven) {
        impl_.reset();
        return;
    }
    // A foreign-thread teardown, a device-lost target, or an unproven submission never touches
    // Vulkan: the whole generation is retained as a raw released pointer in a fixed slot. This
    // path allocates nothing and destroys nothing.
    Impl* const retained = impl_.release();
    {
        std::lock_guard lock(gTargetQuarantineMutex);
        if (gTargetQuarantineCount < kMaxQuarantinedTargets) {
            gTargetQuarantine[gTargetQuarantineCount] = retained;
            ++gTargetQuarantineCount;
        }
        // Else: the bounded cache is full. The raw impl is deliberately not tracked and not
        // destroyed; the fuse below makes create() fail closed so no more generations accrue.
    }
    gTargetTeardownDrainIncomplete.store(true, std::memory_order_release);
    gTargetCreationFused.store(true, std::memory_order_release);
}

bool GpuPresentationTarget::teardownDrainIncomplete() noexcept {
    return gTargetTeardownDrainIncomplete.load(std::memory_order_acquire);
}

GpuPresentationTargetResult
GpuPresentationTarget::create(GpuDevice& device,
                              const GpuPresentationTargetDescription& description) {
    if (gTargetCreationFused.load(std::memory_order_acquire)) {
        return {nullptr, GpuPresentationTargetCode::PresentationUnavailable,
                "a previous presentation target could not be retired and was quarantined; no "
                "further targets are created in this process"};
    }
    const std::thread::id owner = GpuRendererAccess::owner(device);
    if (owner == std::thread::id{} || std::this_thread::get_id() != owner) {
        return {nullptr, GpuPresentationTargetCode::WrongThread,
                "a presentation target may only be created on the device owner thread"};
    }
    auto control = GpuRendererAccess::state(device);
    if (control == nullptr) {
        return {nullptr, GpuPresentationTargetCode::DriverUnavailable,
                "the device has no live allocator state"};
    }

    PlannedSwapchain planned;
    GpuPresentationTargetCode code = GpuPresentationTargetCode::PresentationUnavailable;
    std::string message;
    if (!planSwapchain(device, description, control, planned, code, message)) {
        return {nullptr, code, std::move(message)};
    }

    std::unique_ptr<SwapchainResources> resources;
    try {
        resources = std::make_unique<SwapchainResources>(
            control->device, control->physicalDevice,
            presentation_detail::toSurfaceHandle(description.surface.surface_bits),
            control->computeQueueFamily, control->presentQueueFamily, planned.extent,
            planned.imageCount, planned.format, planned.colorSpace, planned.presentMode,
            planned.retirement, planned.preTransform, planned.compositeAlpha, VK_NULL_HANDLE,
            message);
    } catch (...) {
        return {nullptr, GpuPresentationTargetCode::DriverUnavailable,
                "the swapchain could not be created"};
    }
    if (resources == nullptr || !resources->valid) {
        return {nullptr, GpuPresentationTargetCode::DriverUnavailable,
                message.empty() ? "the swapchain could not be created" : std::move(message)};
    }

    auto impl = std::make_unique<Impl>();
    impl->owner = owner;
    impl->device = &device;
    impl->control = std::move(control);
    impl->epoch = description.surface.epoch;
    impl->surfaceBits = description.surface.surface_bits;
    impl->description = description;
    impl->info.format = [&] {
        switch (planned.format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
            return GpuPresentationFormat::Bgra8Unorm;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return GpuPresentationFormat::Bgra8Srgb;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return GpuPresentationFormat::Rgba8Unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return GpuPresentationFormat::Rgba8Srgb;
        default:
            return GpuPresentationFormat::Unknown;
        }
    }();
    impl->info.srgb_nonlinear = planned.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    impl->info.width = planned.extent.width;
    impl->info.height = planned.extent.height;
    impl->info.image_count = static_cast<std::uint32_t>(resources->images.size());
    impl->info.retirement = planned.retirement;
    impl->resources = std::move(resources);
    impl->lastCode = GpuPresentationTargetCode::Ok;
    return {std::unique_ptr<GpuPresentationTarget>(new GpuPresentationTarget(std::move(impl))),
            GpuPresentationTargetCode::Ok,
            {}};
}

GpuPresentationTargetCode GpuPresentationTarget::acquire() {
    if (impl_ == nullptr) {
        return GpuPresentationTargetCode::PresentationUnavailable;
    }
    if (std::this_thread::get_id() != impl_->owner) {
        return GpuPresentationTargetCode::WrongThread;
    }
    if (impl_->deviceLost) {
        return GpuPresentationTargetCode::DeviceLost;
    }
    if (impl_->retired || impl_->retiring || impl_->resources == nullptr) {
        return impl_->retired ? GpuPresentationTargetCode::Retired
                              : GpuPresentationTargetCode::RetirePending;
    }
    std::string message;
    const GpuPresentationTargetCode code =
        presentation_detail::acquireImage(*impl_->resources, impl_->control->device, message);
    if (code == GpuPresentationTargetCode::DeviceLost) {
        impl_->deviceLost = true;
    }
    impl_->lastCode = code;
    impl_->lastMessage = std::move(message);
    return code;
}

GpuPresentationTargetCode GpuPresentationTarget::present(GpuClearColor color) {
    if (impl_ == nullptr) {
        return GpuPresentationTargetCode::PresentationUnavailable;
    }
    if (std::this_thread::get_id() != impl_->owner) {
        return GpuPresentationTargetCode::WrongThread;
    }
    if (impl_->deviceLost) {
        return GpuPresentationTargetCode::DeviceLost;
    }
    if (impl_->retiring || impl_->retired || impl_->resources == nullptr) {
        return impl_->retired ? GpuPresentationTargetCode::Retired
                              : GpuPresentationTargetCode::RetirePending;
    }
    std::string message;
    const GpuPresentationTargetCode code = presentation_detail::presentImage(
        *impl_->resources, impl_->control->device, impl_->control->presentQueue, color, message);
    if (code == GpuPresentationTargetCode::DeviceLost) {
        impl_->deviceLost = true;
    }
    impl_->lastCode = code;
    impl_->lastMessage = std::move(message);
    return code;
}

GpuPresentationTargetCode GpuPresentationTarget::pollRetirement() {
    if (impl_ == nullptr) {
        return GpuPresentationTargetCode::PresentationUnavailable;
    }
    if (std::this_thread::get_id() != impl_->owner) {
        return GpuPresentationTargetCode::WrongThread;
    }
    if (impl_->deviceLost) {
        return GpuPresentationTargetCode::DeviceLost;
    }
    if (impl_->retired) {
        return GpuPresentationTargetCode::Retired;
    }
    if (impl_->resources == nullptr) {
        impl_->retired = true;
        impl_->lastCode = GpuPresentationTargetCode::Retired;
        return GpuPresentationTargetCode::Retired;
    }
    std::string message;
    const GpuPresentationTargetCode code = presentation_detail::pollPresent(
        *impl_->resources, impl_->control->device, impl_->retiring, message);
    if (code == GpuPresentationTargetCode::DeviceLost) {
        impl_->deviceLost = true;
        impl_->lastCode = code;
        impl_->lastMessage = message.empty() ? "the device was lost during presentation" : message;
        return code;
    }
    if (impl_->retiring && code == GpuPresentationTargetCode::Retired) {
        impl_->resources.reset();
        impl_->retired = true;
        impl_->lastCode = GpuPresentationTargetCode::Retired;
        impl_->lastMessage.clear();
        return GpuPresentationTargetCode::Retired;
    }
    impl_->lastCode =
        impl_->retiring ? GpuPresentationTargetCode::RetirePending : GpuPresentationTargetCode::Ok;
    impl_->lastMessage = std::move(message);
    return impl_->lastCode;
}

GpuPresentationTargetCode
GpuPresentationTarget::recreate(const GpuPresentationTargetDescription& description) {
    if (impl_ == nullptr) {
        return GpuPresentationTargetCode::PresentationUnavailable;
    }
    if (std::this_thread::get_id() != impl_->owner) {
        return GpuPresentationTargetCode::WrongThread;
    }
    if (impl_->deviceLost) {
        return GpuPresentationTargetCode::DeviceLost;
    }
    if (impl_->retiring || impl_->retired || impl_->resources == nullptr) {
        return impl_->retired ? GpuPresentationTargetCode::Retired
                              : GpuPresentationTargetCode::RetirePending;
    }
    if (impl_->resources->presentOutstanding || impl_->resources->renderInFlight ||
        impl_->resources->acquiredIndex.has_value()) {
        impl_->lastCode = GpuPresentationTargetCode::NotReady;
        impl_->lastMessage = "the previous swapchain generation is still in flight";
        return impl_->lastCode;
    }

    PlannedSwapchain planned;
    GpuPresentationTargetCode code = GpuPresentationTargetCode::PresentationUnavailable;
    std::string message;
    if (impl_->device == nullptr ||
        !planSwapchain(*impl_->device, description, impl_->control, planned, code, message)) {
        impl_->lastCode = code;
        impl_->lastMessage = std::move(message);
        return code;
    }

    std::unique_ptr<SwapchainResources> resources;
    try {
        resources = std::make_unique<SwapchainResources>(
            impl_->control->device, impl_->control->physicalDevice,
            presentation_detail::toSurfaceHandle(description.surface.surface_bits),
            impl_->control->computeQueueFamily, impl_->control->presentQueueFamily, planned.extent,
            planned.imageCount, planned.format, planned.colorSpace, planned.presentMode,
            planned.retirement, planned.preTransform, planned.compositeAlpha,
            *impl_->resources->swapchain, message);
    } catch (...) {
        impl_->lastCode = GpuPresentationTargetCode::DriverUnavailable;
        impl_->lastMessage = "the replacement swapchain could not be created";
        return impl_->lastCode;
    }
    if (resources == nullptr || !resources->valid) {
        impl_->lastCode = GpuPresentationTargetCode::DriverUnavailable;
        impl_->lastMessage =
            message.empty() ? "the replacement swapchain could not be created" : std::move(message);
        return impl_->lastCode;
    }
    impl_->resources = std::move(resources);
    impl_->description = description;
    impl_->info.width = planned.extent.width;
    impl_->info.height = planned.extent.height;
    impl_->info.image_count = static_cast<std::uint32_t>(impl_->resources->images.size());
    impl_->info.retirement = planned.retirement;
    impl_->lastCode = GpuPresentationTargetCode::Ok;
    impl_->lastMessage.clear();
    return impl_->lastCode;
}

GpuPresentationTargetCode GpuPresentationTarget::beginRetire() {
    if (impl_ == nullptr) {
        return GpuPresentationTargetCode::PresentationUnavailable;
    }
    if (std::this_thread::get_id() != impl_->owner) {
        return GpuPresentationTargetCode::WrongThread;
    }
    if (impl_->deviceLost) {
        return GpuPresentationTargetCode::DeviceLost;
    }
    if (!impl_->retiring && impl_->resources != nullptr &&
        impl_->resources->acquiredIndex.has_value() && !impl_->resources->presentOutstanding &&
        !impl_->resources->renderInFlight) {
        // An acquired-but-unpresented image holds the acquire semaphore. Consume it with a legal
        // clear discard present so its retirement can be proven; if that cannot be queued the
        // pending/retain state below keeps the native surface alive rather than pretending drain.
        const GpuPresentationTargetCode discard =
            present(GpuClearColor{.red = 0.0, .green = 0.0, .blue = 0.0, .alpha = 1.0});
        if (discard == GpuPresentationTargetCode::DeviceLost) {
            return GpuPresentationTargetCode::DeviceLost;
        }
    }
    if (!impl_->retiring) {
        impl_->retiring = true;
    }
    return pollRetirement();
}

GpuPresentationTargetCode GpuPresentationTarget::retireState() const noexcept {
    if (impl_ == nullptr) {
        return GpuPresentationTargetCode::PresentationUnavailable;
    }
    if (impl_->retired) {
        return GpuPresentationTargetCode::Retired;
    }
    if (impl_->deviceLost) {
        return GpuPresentationTargetCode::DeviceLost;
    }
    return impl_->retiring ? GpuPresentationTargetCode::RetirePending
                           : GpuPresentationTargetCode::Ok;
}

GpuPresentationTargetInfo GpuPresentationTarget::info() const noexcept {
    return impl_ != nullptr ? impl_->info : GpuPresentationTargetInfo{};
}

std::uint32_t GpuPresentationTarget::acquiredImageIndex() const noexcept {
    if (impl_ == nullptr || impl_->resources == nullptr ||
        !impl_->resources->acquiredIndex.has_value()) {
        return 0;
    }
    return *impl_->resources->acquiredIndex;
}

GpuPresentationTargetCode GpuPresentationTarget::lastCode() const noexcept {
    return impl_ != nullptr ? impl_->lastCode : GpuPresentationTargetCode::PresentationUnavailable;
}

const std::string& GpuPresentationTarget::lastMessage() const noexcept {
    static const std::string kEmpty;
    return impl_ != nullptr ? impl_->lastMessage : kEmpty;
}

} // namespace bloom::render
