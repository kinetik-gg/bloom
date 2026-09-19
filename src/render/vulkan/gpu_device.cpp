#include <bloom/render/gpu_device.hpp>

#include "gpu_device_private.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// Vulkan-Hpp's dynamic default dispatcher is never used: every command is reached through an
// explicit instance/device dispatcher built from the privately opened loader. Defining the single
// storage translation unit anyway keeps the header's extern declaration satisfied and guarantees no
// static Vulkan prototype can be pulled in by an accidental default-dispatcher reference.
#if defined(VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE)
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#endif

namespace bloom::render {
namespace {

#if defined(_WIN32)
constexpr const char* kDefaultLoaderName = "vulkan-1.dll";
#elif defined(__APPLE__)
constexpr const char* kDefaultLoaderName = "libvulkan.1.dylib";
#else
constexpr const char* kDefaultLoaderName = "libvulkan.so.1";
#endif

// VK_KHR_portability_subset lives behind VK_ENABLE_BETA_EXTENSIONS in the Vulkan headers. The
// extension name string itself is stable and public, so name it directly instead of enabling the
// whole beta vocabulary for one device-extension probe.
constexpr const char* kPortabilitySubsetExtensionName = "VK_KHR_portability_subset";
constexpr const char* kSurfaceExtensionName = "VK_KHR_surface";
constexpr const char* kWaylandSurfaceExtensionName = "VK_KHR_wayland_surface";
constexpr const char* kSwapchainExtensionName = "VK_KHR_swapchain";
constexpr const char* kSwapchainMaintenance1ExtName = "VK_EXT_swapchain_maintenance1";
constexpr const char* kSwapchainMaintenance1KhrName = "VK_KHR_swapchain_maintenance1";
constexpr const char* kPresentIdExtensionName = "VK_KHR_present_id";
constexpr const char* kPresentWaitExtensionName = "VK_KHR_present_wait";

// This slice enables Wayland only. XCB/Xlib are declared in the public enum but need a reviewed
// xcb/x11/xrandr dependency intake before their surface extensions may be enabled, so a request for
// them stays explicitly Unavailable with the compute path preserved.
[[nodiscard]] const char*
platformSurfaceExtensionName(const GpuPresentationPlatform platform) noexcept {
    switch (platform) {
    case GpuPresentationPlatform::Wayland:
        return kWaylandSurfaceExtensionName;
    case GpuPresentationPlatform::None:
    case GpuPresentationPlatform::Xcb:
    case GpuPresentationPlatform::Xlib:
        return nullptr;
    }
    return nullptr;
}

// Unique monotonic presentation epoch, minted once per created GpuDevice instance. A function-local
// static is intentional: it is per-process uniqueness with no global service locator, and a device
// that outlives another can never collide with it.
[[nodiscard]] std::uint64_t allocatePresentationEpoch() noexcept {
    static std::atomic<std::uint64_t> next{1};
    for (;;) {
        const std::uint64_t raw = next.fetch_add(1, std::memory_order_relaxed);
        if (raw != 0) {
            return raw;
        }
    }
}

// Native handles cross the public boundary as integer bits only; no Vk* type is ever public.
template <typename Handle> [[nodiscard]] std::uint64_t handleBits(const Handle handle) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
}

template <typename Handle> [[nodiscard]] Handle handleFromBits(const std::uint64_t bits) noexcept {
    return reinterpret_cast<Handle>(static_cast<std::uintptr_t>(bits));
}

[[nodiscard]] GpuDiagnostic diagnostic(const GpuDiagnosticCode code, std::string message) {
    return GpuDiagnostic{code, std::move(message)};
}

[[nodiscard]] int deviceTypeRank(const VkPhysicalDeviceType type) noexcept {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 5;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 4;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 3;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 2;
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        return 1;
    default:
        return 0;
    }
}

[[nodiscard]] std::string_view deviceTypeLabel(const VkPhysicalDeviceType type) noexcept {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "cpu";
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        return "other";
    default:
        return "unknown";
    }
}

[[nodiscard]] bool instanceExtensionAvailable(const vk::raii::Context& context, const char* name) {
    const auto* dispatcher = context.getDispatcher();
    if (dispatcher->vkEnumerateInstanceExtensionProperties == nullptr) {
        return false;
    }
    std::uint32_t count = 0;
    if (dispatcher->vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) !=
            VK_SUCCESS ||
        count == 0U) {
        return false;
    }
    std::vector<VkExtensionProperties> available(count);
    if (dispatcher->vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()) !=
        VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties& extension : available) {
        if (std::string_view(extension.extensionName) == name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool deviceExtensionAvailable(const vk::raii::Instance& instance,
                                            const VkPhysicalDevice device, const char* name) {
    const auto* dispatcher = instance.getDispatcher();
    if (dispatcher->vkEnumerateDeviceExtensionProperties == nullptr) {
        return false;
    }
    std::uint32_t count = 0;
    if (dispatcher->vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) !=
            VK_SUCCESS ||
        count == 0U) {
        return false;
    }
    std::vector<VkExtensionProperties> available(count);
    if (dispatcher->vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
                                                         available.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties& extension : available) {
        if (std::string_view(extension.extensionName) == name) {
            return true;
        }
    }
    return false;
}

struct DeviceSelection final {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    // Preselected presentation family: the compute family when it also supports graphics, otherwise
    // the first graphics-capable family. This is only a heuristic; the actual surface is validated
    // later with vkGetPhysicalDeviceSurfaceSupportKHR.
    std::uint32_t presentFamily = UINT32_MAX;
    VkPhysicalDeviceProperties properties{};
    std::uint64_t deviceMemoryBytes = 0;
    bool memoryBudgetSupported = false;
    bool portabilitySubset = false;
    int rank = -1;
};

// Enumerates the physical devices and selects the best one that satisfies the bootstrap baseline:
// Vulkan 1.2, timeline semaphores from the 1.2 feature set, and a compute-capable queue family.
// Every requirement is checked directly; a device-wide advertised version is never treated as
// proof of a feature, and a missing entry point is an Unavailable path rather than a call.
[[nodiscard]] std::optional<DeviceSelection>
selectPhysicalDevice(const vk::raii::Instance& instance) {
    const auto* dispatcher = instance.getDispatcher();
    if (dispatcher->vkEnumeratePhysicalDevices == nullptr ||
        dispatcher->vkGetPhysicalDeviceProperties2 == nullptr ||
        dispatcher->vkGetPhysicalDeviceFeatures2 == nullptr ||
        dispatcher->vkGetPhysicalDeviceQueueFamilyProperties == nullptr ||
        dispatcher->vkGetPhysicalDeviceMemoryProperties2 == nullptr) {
        return std::nullopt;
    }

    const VkInstance rawInstance = static_cast<VkInstance>(*instance);
    std::uint32_t count = 0;
    if (dispatcher->vkEnumeratePhysicalDevices(rawInstance, &count, nullptr) != VK_SUCCESS ||
        count == 0U) {
        return std::nullopt;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (dispatcher->vkEnumeratePhysicalDevices(rawInstance, &count, devices.data()) != VK_SUCCESS) {
        return std::nullopt;
    }

    std::optional<DeviceSelection> best;
    for (const VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        dispatcher->vkGetPhysicalDeviceProperties2(device, &properties2);
        if (properties2.properties.apiVersion < VK_API_VERSION_1_2) {
            continue;
        }

        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &features12;
        dispatcher->vkGetPhysicalDeviceFeatures2(device, &features2);
        if (features12.timelineSemaphore != VK_TRUE) {
            continue;
        }

        std::uint32_t familyCount = 0;
        dispatcher->vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        dispatcher->vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
        std::uint32_t computeFamily = UINT32_MAX;
        for (std::uint32_t index = 0; index < familyCount; ++index) {
            if ((families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U &&
                families[index].queueCount > 0U) {
                computeFamily = index;
                break;
            }
        }
        if (computeFamily == UINT32_MAX) {
            continue;
        }

        // Presentation prefers one combined graphics+compute queue, so resident GPU images stay
        // EXCLUSIVE on the compute queue and no split-queue transfers are implied. A device whose
        // compute family lacks the graphics bit therefore reports presentation Unavailable (the
        // caller keeps the CPU path) instead of allocating a second queue family.
        std::uint32_t presentFamily = UINT32_MAX;
        if ((families[computeFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
            presentFamily = computeFamily;
        }

        const int rank = deviceTypeRank(properties2.properties.deviceType);
        if (best.has_value() && best->rank >= rank) {
            continue;
        }

        VkPhysicalDeviceMemoryProperties2 memoryProperties2{};
        memoryProperties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        dispatcher->vkGetPhysicalDeviceMemoryProperties2(device, &memoryProperties2);
        std::uint64_t deviceLocalBytes = 0;
        for (std::uint32_t heap = 0; heap < memoryProperties2.memoryProperties.memoryHeapCount;
             ++heap) {
            // Only DEVICE_LOCAL heaps are summed: this is a bounded device-local fact, never a VRAM
            // or usable-budget claim (integrated GPUs share host memory, and drivers may migrate).
            if ((memoryProperties2.memoryProperties.memoryHeaps[heap].flags &
                 VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0U) {
                deviceLocalBytes += memoryProperties2.memoryProperties.memoryHeaps[heap].size;
            }
        }

        DeviceSelection selection;
        selection.physicalDevice = device;
        selection.queueFamily = computeFamily;
        selection.presentFamily = presentFamily;
        selection.properties = properties2.properties;
        selection.deviceMemoryBytes = deviceLocalBytes;
        selection.memoryBudgetSupported =
            deviceExtensionAvailable(instance, device, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        selection.portabilitySubset =
            deviceExtensionAvailable(instance, device, kPortabilitySubsetExtensionName);
        selection.rank = rank;
        best = selection;
    }
    return best;
}

const GpuCapabilityReport kEmptyCapabilityReport{};

} // namespace

struct GpuBufferAllocation::Impl final {
    ~Impl() {
        if (control != nullptr) {
            assert(control->owner == std::this_thread::get_id());
            if (allocation != VK_NULL_HANDLE) {
                vmaDestroyBuffer(control->allocator, buffer, allocation);
            }
        }
    }

    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    GpuBufferInfo info;
};

struct GpuDevice::Impl final {
    std::thread::id owner;
    GpuDeviceState state = GpuDeviceState::Unavailable;
    GpuCapabilityReport report;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;

    // Immutable after create(): safe for borrowedInstanceView() reads from the UI thread.
    bool presentationReady = false;
    std::uint64_t presentationEpoch = 0;
    std::uint32_t presentQueueFamily = 0;
    std::uint64_t borrowedInstanceBits = 0;
};

GpuDevice::GpuDevice(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

GpuDevice::GpuDevice(GpuDevice&& other) noexcept = default;

GpuDevice& GpuDevice::operator=(GpuDevice&& other) noexcept = default;

GpuDevice::~GpuDevice() {
    if (impl_ != nullptr) {
        assert(impl_->owner == std::this_thread::get_id());
    }
}

GpuDeviceState GpuDevice::state() const noexcept {
    return impl_ != nullptr ? impl_->state : GpuDeviceState::Unavailable;
}

bool GpuDevice::isOwnerThread() const noexcept {
    return impl_ != nullptr && impl_->owner == std::this_thread::get_id();
}

const GpuCapabilityReport& GpuDevice::capabilityReport() const noexcept {
    if (impl_ != nullptr) {
        return impl_->report;
    }
    return kEmptyCapabilityReport;
}

GpuQualification GpuDevice::qualificationFor(const GpuOperationId operation,
                                             const GpuPrecision precision) const noexcept {
    if (impl_ == nullptr) {
        return GpuQualification::Unavailable;
    }
    for (const GpuOperationCapability& capability : impl_->report.operations) {
        if (capability.operation == operation && capability.precision == precision) {
            return capability.qualification;
        }
    }
    return GpuQualification::Unavailable;
}

GpuPresentationStatus GpuDevice::presentationStatus() const noexcept {
    if (impl_ != nullptr) {
        return impl_->report.presentation;
    }
    return {};
}

GpuBorrowedInstanceView GpuDevice::borrowedInstanceView() const noexcept {
    GpuBorrowedInstanceView view;
    if (impl_ == nullptr || !impl_->presentationReady) {
        return view;
    }
    view.valid = true;
    view.instance_bits = impl_->borrowedInstanceBits;
    view.present_queue_family = impl_->presentQueueFamily;
    view.epoch = GpuPresentationEpoch{.value = impl_->presentationEpoch};
    return view;
}

GpuSurfaceSupportResult
GpuDevice::validateBorrowedSurface(const GpuBorrowedSurface& surface) const {
    if (impl_ == nullptr || impl_->state != GpuDeviceState::Ready || impl_->control == nullptr) {
        return {GpuSurfaceSupport::PresentationUnavailable, "the device generation is not ready"};
    }
    if (surface.epoch.value == 0 || surface.epoch.value != impl_->presentationEpoch) {
        return {GpuSurfaceSupport::WrongEpoch,
                "the borrowed surface belongs to a different device generation"};
    }
    if (std::this_thread::get_id() != impl_->owner) {
        return {GpuSurfaceSupport::WrongThread,
                "surface validation may only run on the device owner thread"};
    }
    if (surface.surface_bits == 0) {
        return {GpuSurfaceSupport::InvalidArgument, "the borrowed surface handle is empty"};
    }
    if (!impl_->presentationReady) {
        return {GpuSurfaceSupport::PresentationUnavailable,
                "presentation is not ready for this device generation"};
    }
    const auto* dispatcher = impl_->control->instance.getDispatcher();
    if (dispatcher == nullptr || dispatcher->vkGetPhysicalDeviceSurfaceSupportKHR == nullptr) {
        return {GpuSurfaceSupport::DriverUnavailable,
                "the loaded driver exposes no vkGetPhysicalDeviceSurfaceSupportKHR"};
    }
    VkBool32 supported = VK_FALSE;
    const VkResult result = dispatcher->vkGetPhysicalDeviceSurfaceSupportKHR(
        static_cast<VkPhysicalDevice>(*impl_->control->physicalDevice), impl_->presentQueueFamily,
        handleFromBits<VkSurfaceKHR>(surface.surface_bits), &supported);
    if (result != VK_SUCCESS) {
        return {GpuSurfaceSupport::DriverUnavailable,
                "vkGetPhysicalDeviceSurfaceSupportKHR rejected the borrowed surface"};
    }
    return supported == VK_TRUE
               ? GpuSurfaceSupportResult{GpuSurfaceSupport::Supported, {}}
               : GpuSurfaceSupportResult{
                     GpuSurfaceSupport::UnsupportedByQueue,
                     "the preselected present queue cannot present to this surface"};
}

GpuDeviceCreationResult GpuDevice::create(const GpuDeviceCreationOptions& options) {
    const std::string loaderPath = options.loader_path.empty() ? std::string(kDefaultLoaderName)
                                                               : options.loader_path.string();
    vulkan_detail::DynamicLibrary loader(loaderPath.c_str());
    if (!loader.isOpen()) {
        return {nullptr,
                diagnostic(GpuDiagnosticCode::LoaderUnavailable,
                           "the Vulkan loader could not be opened from the requested location")};
    }

    const auto getInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(loader.symbol("vkGetInstanceProcAddr"));
    const auto getDeviceProcAddr =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(loader.symbol("vkGetDeviceProcAddr"));
    if (getInstanceProcAddr == nullptr || getDeviceProcAddr == nullptr) {
        return {nullptr, diagnostic(GpuDiagnosticCode::LoaderUnavailable,
                                    "the Vulkan loader does not expose vkGetInstanceProcAddr and "
                                    "vkGetDeviceProcAddr")};
    }

    vk::raii::Context context(getInstanceProcAddr);
    if (context.getDispatcher()->vkCreateInstance == nullptr) {
        return {nullptr, diagnostic(GpuDiagnosticCode::LoaderUnavailable,
                                    "the Vulkan loader does not expose vkCreateInstance")};
    }

    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (context.getDispatcher()->vkEnumerateInstanceVersion != nullptr) {
        std::uint32_t version = 0;
        if (context.getDispatcher()->vkEnumerateInstanceVersion(&version) == VK_SUCCESS) {
            loaderVersion = version;
        }
    }
    if (loaderVersion < VK_API_VERSION_1_2) {
        return {nullptr,
                diagnostic(GpuDiagnosticCode::DeviceIncompatible,
                           "Vulkan 1.2 is the required API floor and this loader reports an "
                           "older version")};
    }

    const bool portabilityEnumeration =
        instanceExtensionAvailable(context, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

    const bool presentationRequested = options.request_presentation;
    const GpuPresentationPlatform presentationPlatform = options.presentation_platform;
    const char* const platformSurfaceExtension = platformSurfaceExtensionName(presentationPlatform);
    const bool surfaceExtensionAvailable =
        instanceExtensionAvailable(context, kSurfaceExtensionName);
    const bool platformSurfaceExtensionAvailable =
        platformSurfaceExtension != nullptr &&
        instanceExtensionAvailable(context, platformSurfaceExtension);
    const bool surfaceExtensionEnabled = presentationRequested && surfaceExtensionAvailable;
    const bool platformSurfaceExtensionEnabled =
        surfaceExtensionEnabled && platformSurfaceExtensionAvailable;

    vk::ApplicationInfo applicationInfo;
    applicationInfo.pApplicationName = "Bloom";
    applicationInfo.applicationVersion = VK_MAKE_VERSION(0, 0, 0);
    applicationInfo.pEngineName = "Bloom";
    applicationInfo.engineVersion = VK_MAKE_VERSION(0, 0, 0);
    applicationInfo.apiVersion = VK_API_VERSION_1_2;

    std::vector<const char*> instanceExtensions;
    if (portabilityEnumeration) {
        instanceExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
    if (surfaceExtensionEnabled) {
        instanceExtensions.push_back(kSurfaceExtensionName);
    }
    if (platformSurfaceExtensionEnabled) {
        instanceExtensions.push_back(platformSurfaceExtension);
    }

    vk::InstanceCreateInfo instanceInfo;
    instanceInfo.pApplicationInfo = &applicationInfo;
    instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames =
        instanceExtensions.empty() ? nullptr : instanceExtensions.data();
    if (portabilityEnumeration) {
        instanceInfo.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
    }

    VkInstance rawInstance = VK_NULL_HANDLE;
    const VkResult instanceResult = context.getDispatcher()->vkCreateInstance(
        reinterpret_cast<const VkInstanceCreateInfo*>(&instanceInfo), nullptr, &rawInstance);
    if (instanceResult != VK_SUCCESS || rawInstance == VK_NULL_HANDLE) {
        return {nullptr, diagnostic(GpuDiagnosticCode::InstanceCreationFailed,
                                    "vkCreateInstance failed for the Vulkan 1.2 instance")};
    }
    vk::raii::Instance instance(context, rawInstance);

    const std::optional<DeviceSelection> selection = selectPhysicalDevice(instance);
    if (!selection.has_value()) {
        return {nullptr,
                diagnostic(GpuDiagnosticCode::NoPhysicalDevice,
                           "no Vulkan 1.2 physical device with a compute queue and timeline "
                           "semaphores was found")};
    }
    if (instance.getDispatcher()->vkCreateDevice == nullptr) {
        return {nullptr, diagnostic(GpuDiagnosticCode::DeviceCreationFailed,
                                    "the Vulkan instance does not expose vkCreateDevice")};
    }

    const bool presentFamilySelected = selection->presentFamily != UINT32_MAX;
    const bool swapchainAvailable =
        platformSurfaceExtensionEnabled && presentFamilySelected &&
        deviceExtensionAvailable(instance, selection->physicalDevice, kSwapchainExtensionName);
    const bool swapchainExtensionEnabled = swapchainAvailable;

    std::vector<const char*> deviceExtensions;
    if (selection->portabilitySubset) {
        deviceExtensions.push_back(kPortabilitySubsetExtensionName);
    }
    if (swapchainExtensionEnabled) {
        deviceExtensions.push_back(kSwapchainExtensionName);
    }

    // Presentation-retirement mechanisms. Prefer maintenance1 present fences; fall back to
    // present_wait + present_id. Each is enabled only when both the extension and its device
    // feature are actually advertised, so device creation cannot fail over an unsupported feature.
    bool maintenance1ExtensionAvailable = false;
    const char* maintenance1ExtensionName = nullptr;
    if (swapchainExtensionEnabled) {
        if (deviceExtensionAvailable(instance, selection->physicalDevice,
                                     kSwapchainMaintenance1ExtName)) {
            maintenance1ExtensionAvailable = true;
            maintenance1ExtensionName = kSwapchainMaintenance1ExtName;
        } else if (deviceExtensionAvailable(instance, selection->physicalDevice,
                                            kSwapchainMaintenance1KhrName)) {
            maintenance1ExtensionAvailable = true;
            maintenance1ExtensionName = kSwapchainMaintenance1KhrName;
        }
    }
    const bool presentIdExtensionAvailable =
        swapchainExtensionEnabled &&
        deviceExtensionAvailable(instance, selection->physicalDevice, kPresentIdExtensionName);
    const bool presentWaitExtensionAvailable =
        swapchainExtensionEnabled &&
        deviceExtensionAvailable(instance, selection->physicalDevice, kPresentWaitExtensionName);

    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance1Query{};
    maintenance1Query.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
    VkPhysicalDevicePresentIdFeaturesKHR presentIdQuery{};
    presentIdQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    VkPhysicalDevicePresentWaitFeaturesKHR presentWaitQuery{};
    presentWaitQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    VkPhysicalDeviceFeatures2 presentationFeatureQuery{};
    presentationFeatureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    void** queryTail = &presentationFeatureQuery.pNext;
    if (maintenance1ExtensionAvailable) {
        *queryTail = &maintenance1Query;
        queryTail = &maintenance1Query.pNext;
    }
    if (presentIdExtensionAvailable) {
        *queryTail = &presentIdQuery;
        queryTail = &presentIdQuery.pNext;
    }
    if (presentWaitExtensionAvailable) {
        *queryTail = &presentWaitQuery;
        queryTail = &presentWaitQuery.pNext;
    }
    if (queryTail != &presentationFeatureQuery.pNext) {
        instance.getDispatcher()->vkGetPhysicalDeviceFeatures2(selection->physicalDevice,
                                                               &presentationFeatureQuery);
    }
    const bool presentFencesEnabled =
        maintenance1ExtensionAvailable && maintenance1Query.swapchainMaintenance1 == VK_TRUE;
    const bool presentWaitEnabled = presentIdExtensionAvailable && presentWaitExtensionAvailable &&
                                    presentIdQuery.presentId == VK_TRUE &&
                                    presentWaitQuery.presentWait == VK_TRUE;
    if (presentFencesEnabled) {
        deviceExtensions.push_back(maintenance1ExtensionName);
    }
    if (presentWaitEnabled) {
        deviceExtensions.push_back(kPresentIdExtensionName);
        deviceExtensions.push_back(kPresentWaitExtensionName);
    }

    const float queuePriority = 1.0F;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    const auto appendQueue = [&](const std::uint32_t family) {
        VkDeviceQueueCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &queuePriority;
        queueInfos.push_back(info);
    };
    appendQueue(selection->queueFamily);
    if (swapchainExtensionEnabled && selection->presentFamily != selection->queueFamily) {
        appendQueue(selection->presentFamily);
    }

    VkPhysicalDeviceVulkan12Features enabledFeatures12{};
    enabledFeatures12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enabledFeatures12.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT enabledMaintenance1{};
    enabledMaintenance1.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
    enabledMaintenance1.swapchainMaintenance1 = presentFencesEnabled ? VK_TRUE : VK_FALSE;
    VkPhysicalDevicePresentIdFeaturesKHR enabledPresentId{};
    enabledPresentId.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    enabledPresentId.presentId = presentWaitEnabled ? VK_TRUE : VK_FALSE;
    VkPhysicalDevicePresentWaitFeaturesKHR enabledPresentWait{};
    enabledPresentWait.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    enabledPresentWait.presentWait = presentWaitEnabled ? VK_TRUE : VK_FALSE;
    void** featureTail = &enabledFeatures12.pNext;
    const auto appendFeature = [&featureTail](auto* feature) {
        *featureTail = feature;
        featureTail = &feature->pNext;
    };
    if (presentFencesEnabled) {
        appendFeature(&enabledMaintenance1);
    }
    if (presentWaitEnabled) {
        appendFeature(&enabledPresentId);
        appendFeature(&enabledPresentWait);
    }

    VkPhysicalDeviceFeatures2 enabledFeatures{};
    enabledFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabledFeatures.pNext = &enabledFeatures12;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &enabledFeatures;
    deviceInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames =
        deviceExtensions.empty() ? nullptr : deviceExtensions.data();

    VkDevice rawDevice = VK_NULL_HANDLE;
    const VkResult deviceResult = instance.getDispatcher()->vkCreateDevice(
        selection->physicalDevice, &deviceInfo, nullptr, &rawDevice);
    if (deviceResult != VK_SUCCESS || rawDevice == VK_NULL_HANDLE) {
        return {nullptr, diagnostic(GpuDiagnosticCode::DeviceCreationFailed,
                                    "vkCreateDevice failed for the selected compute device")};
    }

    auto control = std::make_shared<vulkan_detail::DeviceAllocatorState>();
    control->owner = std::this_thread::get_id();
    control->loader = std::move(loader);
    control->instance = std::move(instance);
    control->physicalDevice =
        vk::raii::PhysicalDevice(control->instance, selection->physicalDevice);
    control->device = vk::raii::Device(control->physicalDevice, rawDevice);
    VkQueue rawQueue = VK_NULL_HANDLE;
    control->device.getDispatcher()->vkGetDeviceQueue(static_cast<VkDevice>(*control->device),
                                                      selection->queueFamily, 0U, &rawQueue);
    control->computeQueue = vk::raii::Queue(control->device, rawQueue);
    control->computeQueueFamily = selection->queueFamily;
    control->presentQueue = control->computeQueue;
    control->presentQueueFamily = selection->queueFamily;
    if (swapchainExtensionEnabled && selection->presentFamily != selection->queueFamily) {
        VkQueue rawPresentQueue = VK_NULL_HANDLE;
        control->device.getDispatcher()->vkGetDeviceQueue(static_cast<VkDevice>(*control->device),
                                                          selection->presentFamily, 0U,
                                                          &rawPresentQueue);
        control->presentQueue = vk::raii::Queue(control->device, rawPresentQueue);
        control->presentQueueFamily = selection->presentFamily;
    }
    control->presentationRequested = presentationRequested;
    control->surfaceExtensionEnabled = surfaceExtensionEnabled;
    control->platformSurfaceExtensionEnabled = platformSurfaceExtensionEnabled;
    control->swapchainExtensionEnabled = swapchainExtensionEnabled;
    control->presentFencesEnabled = presentFencesEnabled;
    control->presentWaitEnabled = presentWaitEnabled;
    control->presentationReady = platformSurfaceExtensionEnabled && swapchainExtensionEnabled;
    control->presentationEpoch = allocatePresentationEpoch();
    control->borrowedInstanceBits = handleBits(static_cast<VkInstance>(rawInstance));
    control->generation = 1;
    control->maxStorageBufferRange = selection->properties.limits.maxStorageBufferRange;
    control->maxComputeWorkGroupCountX = selection->properties.limits.maxComputeWorkGroupCount[0];
    control->maxComputeWorkGroupInvocations =
        selection->properties.limits.maxComputeWorkGroupInvocations;
    control->maxComputeWorkGroupSizeX = selection->properties.limits.maxComputeWorkGroupSize[0];

    VmaVulkanFunctions vmaFunctions{};
    vmaFunctions.vkGetInstanceProcAddr = getInstanceProcAddr;
    vmaFunctions.vkGetDeviceProcAddr = getDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = selection->physicalDevice;
    allocatorInfo.device = rawDevice;
    allocatorInfo.instance = rawInstance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;
    allocatorInfo.pVulkanFunctions = &vmaFunctions;
    if (vmaCreateAllocator(&allocatorInfo, &control->allocator) != VK_SUCCESS) {
        return {nullptr, diagnostic(GpuDiagnosticCode::AllocatorUnavailable,
                                    "the Vulkan Memory Allocator could not be initialized")};
    }

    auto impl = std::make_unique<GpuDevice::Impl>();
    impl->owner = std::this_thread::get_id();
    impl->state = GpuDeviceState::Ready;
    impl->control = std::move(control);
    impl->report.generation = 1;
    impl->report.state = GpuDeviceState::Ready;
    impl->report.identity.backend = "vulkan";
    impl->report.identity.device_name = selection->properties.deviceName;
    impl->report.identity.driver = "vulkan";
    impl->report.identity.driver_version = selection->properties.driverVersion;
    impl->report.identity.api_version_major =
        VK_API_VERSION_MAJOR(selection->properties.apiVersion);
    impl->report.identity.api_version_minor =
        VK_API_VERSION_MINOR(selection->properties.apiVersion);
    impl->report.identity.vendor_id = selection->properties.vendorID;
    impl->report.identity.device_type =
        std::string(deviceTypeLabel(selection->properties.deviceType));
    impl->report.compute_queue = true;
    impl->report.timeline_semaphore = true;
    impl->report.memory_budget_supported = selection->memoryBudgetSupported;
    impl->report.device_memory_bytes = selection->deviceMemoryBytes;

    GpuPresentationStatus presentationStatus;
    presentationStatus.platform = presentationPlatform;
    if (!presentationRequested) {
        presentationStatus.availability = GpuPresentationAvailability::NotRequested;
        presentationStatus.detail = "presentation was not requested";
    } else if (platformSurfaceExtension == nullptr) {
        presentationStatus.availability = GpuPresentationAvailability::Unavailable;
        presentationStatus.detail =
            "the requested presentation platform is not enabled by this slice";
    } else if (!surfaceExtensionAvailable) {
        presentationStatus.availability = GpuPresentationAvailability::Unavailable;
        presentationStatus.detail =
            std::string(kSurfaceExtensionName) + " is not supported by this loader";
    } else if (!platformSurfaceExtensionAvailable) {
        presentationStatus.availability = GpuPresentationAvailability::Unavailable;
        presentationStatus.detail =
            std::string(platformSurfaceExtension) + " is not supported by this loader";
    } else if (!presentFamilySelected) {
        presentationStatus.availability = GpuPresentationAvailability::Unavailable;
        presentationStatus.detail = "no graphics/present-capable queue family was found";
    } else if (!swapchainAvailable) {
        presentationStatus.availability = GpuPresentationAvailability::Unavailable;
        presentationStatus.detail =
            std::string(kSwapchainExtensionName) + " is not supported by this device";
    } else {
        presentationStatus.availability = GpuPresentationAvailability::Ready;
        presentationStatus.detail =
            "surface, Wayland surface, swapchain, and present queue are enabled";
    }
    presentationStatus.surface_extension = surfaceExtensionEnabled;
    presentationStatus.platform_surface_extension = platformSurfaceExtensionEnabled;
    presentationStatus.swapchain_extension = swapchainExtensionEnabled;
    presentationStatus.present_queue = impl->control->presentationReady;
    presentationStatus.present_fences = presentFencesEnabled;
    presentationStatus.present_wait = presentWaitEnabled;
    impl->report.presentation = presentationStatus;
    impl->presentationReady = impl->control->presentationReady;
    impl->presentationEpoch = impl->control->presentationEpoch;
    impl->presentQueueFamily = impl->control->presentQueueFamily;
    impl->borrowedInstanceBits = impl->control->borrowedInstanceBits;

    return {std::unique_ptr<GpuDevice>(new GpuDevice(std::move(impl))), GpuDiagnostic{}};
}

GpuBufferAllocationResult GpuDevice::allocateHostBuffer(const std::uint64_t size_bytes) {
    if (impl_ == nullptr) {
        return {GpuBufferAllocation{},
                diagnostic(GpuDiagnosticCode::DeviceUnavailable, "the device is not initialized")};
    }
    if (std::this_thread::get_id() != impl_->owner) {
        return {GpuBufferAllocation{},
                diagnostic(GpuDiagnosticCode::WrongThread,
                           "host buffer allocation was attempted from a non-owner thread")};
    }
    if (size_bytes == 0U) {
        return {GpuBufferAllocation{},
                diagnostic(GpuDiagnosticCode::InvalidArgument,
                           "a host buffer allocation must be at least one byte")};
    }
    if (size_bytes > kMaxGpuHostBufferBytes) {
        return {GpuBufferAllocation{},
                diagnostic(GpuDiagnosticCode::AllocationLimitExceeded,
                           "the requested host buffer exceeds the bounded allocation limit")};
    }
    if (impl_->state != GpuDeviceState::Ready || impl_->control == nullptr) {
        return {GpuBufferAllocation{},
                diagnostic(GpuDiagnosticCode::DeviceUnavailable,
                           "the device generation is not ready for allocation")};
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size_bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocatedInfo{};
    if (vmaCreateBuffer(impl_->control->allocator, &bufferInfo, &allocationInfo, &buffer,
                        &allocation, &allocatedInfo) != VK_SUCCESS) {
        return {GpuBufferAllocation{},
                diagnostic(GpuDiagnosticCode::BufferAllocationFailed,
                           "the Vulkan Memory Allocator could not create the host buffer")};
    }

    auto bufferImpl = std::make_unique<GpuBufferAllocation::Impl>();
    bufferImpl->control = impl_->control;
    bufferImpl->buffer = buffer;
    bufferImpl->allocation = allocation;
    bufferImpl->info.size_bytes = allocatedInfo.size;
    bufferImpl->info.host_visible = true;
    return {GpuBufferAllocation(std::move(bufferImpl)), GpuDiagnostic{}};
}

GpuBufferAllocation::GpuBufferAllocation() noexcept = default;

GpuBufferAllocation::GpuBufferAllocation(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GpuBufferAllocation::GpuBufferAllocation(GpuBufferAllocation&& other) noexcept = default;

GpuBufferAllocation& GpuBufferAllocation::operator=(GpuBufferAllocation&& other) noexcept = default;

GpuBufferAllocation::~GpuBufferAllocation() = default;

bool GpuBufferAllocation::isValid() const noexcept { return impl_ != nullptr; }

GpuBufferInfo GpuBufferAllocation::info() const noexcept {
    return impl_ != nullptr ? impl_->info : GpuBufferInfo{};
}

namespace vulkan_detail {

DynamicLibrary::DynamicLibrary(const char* path) noexcept {
#if defined(_WIN32)
    handle_ = path != nullptr ? static_cast<void*>(LoadLibraryA(path)) : nullptr;
#else
    handle_ = path != nullptr ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : nullptr;
#endif
}

DynamicLibrary::~DynamicLibrary() { close(); }

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void* DynamicLibrary::symbol(const char* name) const noexcept {
#if defined(_WIN32)
    if (handle_ == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return handle_ != nullptr ? dlsym(handle_, name) : nullptr;
#endif
}

void DynamicLibrary::close() noexcept {
#if defined(_WIN32)
    if (handle_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(handle_));
        handle_ = nullptr;
    }
#else
    if (handle_ != nullptr) {
        dlclose(handle_);
        handle_ = nullptr;
    }
#endif
}

DeviceAllocatorState::~DeviceAllocatorState() {
    assert(owner == std::this_thread::get_id());
    if (allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator);
        allocator = VK_NULL_HANDLE;
    }
}

} // namespace vulkan_detail

std::shared_ptr<vulkan_detail::DeviceAllocatorState>
GpuRendererAccess::state(GpuDevice& device) noexcept {
    if (device.impl_ == nullptr) {
        return {};
    }
    return device.impl_->control;
}

std::thread::id GpuRendererAccess::owner(const GpuDevice& device) noexcept {
    return device.impl_ != nullptr ? device.impl_->owner : std::thread::id{};
}

} // namespace bloom::render
