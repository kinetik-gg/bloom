#include <bloom/render/gpu_device.hpp>

#include "gpu_device_private.hpp"

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

        const int rank = deviceTypeRank(properties2.properties.deviceType);
        if (best.has_value() && best->rank >= rank) {
            continue;
        }

        VkPhysicalDeviceMemoryProperties2 memoryProperties2{};
        memoryProperties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        dispatcher->vkGetPhysicalDeviceMemoryProperties2(device, &memoryProperties2);
        std::uint64_t memoryBytes = 0;
        for (std::uint32_t heap = 0; heap < memoryProperties2.memoryProperties.memoryHeapCount;
             ++heap) {
            memoryBytes += memoryProperties2.memoryProperties.memoryHeaps[heap].size;
        }

        DeviceSelection selection;
        selection.physicalDevice = device;
        selection.queueFamily = computeFamily;
        selection.properties = properties2.properties;
        selection.deviceMemoryBytes = memoryBytes;
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

    std::vector<const char*> deviceExtensions;
    if (selection->portabilitySubset) {
        deviceExtensions.push_back(kPortabilitySubsetExtensionName);
    }

    const float queuePriority = 1.0F;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = selection->queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceVulkan12Features enabledFeatures12{};
    enabledFeatures12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enabledFeatures12.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceFeatures2 enabledFeatures{};
    enabledFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabledFeatures.pNext = &enabledFeatures12;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &enabledFeatures;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
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

} // namespace bloom::render
