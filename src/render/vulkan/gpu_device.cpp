#include <bloom/render/gpu_device.hpp>

#include "gpu_device_private.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace bloom::render {
namespace {

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

std::uint64_t GpuDevice::ownershipEpoch() const noexcept {
    // impl_->presentationEpoch is assigned for every created device, presentation requested or not.
    return impl_ != nullptr ? impl_->presentationEpoch : 0U;
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
