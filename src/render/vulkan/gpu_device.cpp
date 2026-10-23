#include <bloom/render/gpu_device.hpp>

#include "gpu_device_private.hpp"
#include "gpu_display_transfer_retirement_private.hpp"
#include "gpu_image_reclamation_private.hpp"
#include "gpu_image_upload_retirement_private.hpp"
#include "gpu_ocio_program_private.hpp"
#include "gpu_operation_batch_private.hpp"
#include "gpu_output_color_readback_retirement_private.hpp"
#include "gpu_queue_timing_private.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <cassert>
#include <cstdint>
#include <limits>
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

GpuDevice& GpuDevice::operator=(GpuDevice&& other) noexcept {
    if (this != &other) {
        if (impl_ != nullptr && impl_->control != nullptr) {
            assert(impl_->owner == std::this_thread::get_id());
            upload_detail::closeUploads(*impl_->control);
            display_transfer_detail::closeTransfers(*impl_->control);
            output_color_readback_detail::closeReadbacks(*impl_->control);
            ocio_program_detail::closeProgramOwner(*impl_->control);
            image_detail::closeImageOwner(*impl_->control);
            queue_timing_detail::drainRetired(*impl_->control);
            operation_batch_detail::drainRetired(*impl_->control);
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

GpuDevice::~GpuDevice() {
    if (impl_ != nullptr) {
        assert(impl_->owner == std::this_thread::get_id());
        if (impl_->control != nullptr) {
            upload_detail::closeUploads(*impl_->control);
            display_transfer_detail::closeTransfers(*impl_->control);
            output_color_readback_detail::closeReadbacks(*impl_->control);
            ocio_program_detail::closeProgramOwner(*impl_->control);
            image_detail::closeImageOwner(*impl_->control);
            queue_timing_detail::drainRetired(*impl_->control);
            operation_batch_detail::drainRetired(*impl_->control);
        }
    }
}

void GpuDevice::reclaimReleasedImages() noexcept {
    if (isOwnerThread() && impl_->control != nullptr) {
        operation_batch_detail::drainRetired(*impl_->control);
        upload_detail::drainUploads(*impl_->control);
        display_transfer_detail::drainTransfers(*impl_->control);
        output_color_readback_detail::drainReadbacks(*impl_->control);
        ocio_program_detail::retireUploadQuarantine();
        ocio_program_detail::drainPrograms(*impl_->control);
        image_detail::drainImages(*impl_->control);
    }
}
GpuOcioProgramRetirementStats GpuDevice::ocioProgramRetirementStats() const noexcept {
    if (!impl_ || !impl_->control) {
        return {};
    }
    auto stats = ocio_program_detail::programRetirementStats(*impl_->control);
    stats.pendingUploads = ocio_program_detail::uploadQuarantinePendingFor(*impl_->control) ? 1 : 0;
    return stats;
}

GpuImageReclamationStats GpuDevice::imageReclamationStats() const noexcept {
    return impl_ != nullptr && impl_->control != nullptr
               ? image_detail::imageStats(impl_->control.get())
               : GpuImageReclamationStats{};
}
GpuImageReclamationStats GpuDevice::processImageReclamationStats() noexcept {
    return image_detail::imageStats(nullptr);
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

bool GpuDevice::bindProcessingRoot(GpuProcessingRootIdentity root) noexcept {
    if (!isOwnerThread() || !root || !impl_->control ||
        (impl_->control->processingRoot && impl_->control->processingRoot != root) ||
        (impl_->control->processingPhase &&
         impl_->control->processingPhase->rootIdentity() != root))
        return false;
    impl_->control->processingRoot = std::move(root);
    return true;
}
GpuProcessingRootIdentity GpuDevice::processingRootIdentity() const noexcept {
    return isOwnerThread() && impl_->control ? impl_->control->processingRoot : nullptr;
}
bool GpuDevice::retainProcessingPhase(std::shared_ptr<GpuProcessingPhaseLease> lease) noexcept {
    if (!isOwnerThread() || !lease || !impl_->control || impl_->control->processingPhase) {
        return false;
    }
    if (impl_->control->processingRoot && lease->rootIdentity() != impl_->control->processingRoot)
        return false;
    impl_->control->imageStorageRefusal.reset();
    impl_->control->bufferStorageRefusal.reset();
    impl_->control->processingPhase = std::move(lease);
    return true;
}
void GpuDevice::retireProcessingPhase(const std::uint64_t leaseId,
                                      const bool retirementProven) noexcept {
    if (!isOwnerThread() || !impl_->control || !impl_->control->processingPhase ||
        impl_->control->processingPhase->id() != leaseId) {
        return;
    }
    auto& control = *impl_->control;
    if (!retirementProven || control.processingPhaseUnproven) {
        control.processingPhaseUnproven = true;
        control.processingPhase->markUnproven();
        return;
    }
    control.processingPhase.reset();
}
std::shared_ptr<GpuProcessingPhaseLease> GpuDevice::retainedProcessingPhase() const noexcept {
    return isOwnerThread() && impl_->control ? impl_->control->processingPhase : nullptr;
}
bool GpuDevice::processingPhaseOccupied() const noexcept {
    return !isOwnerThread() || !impl_->control || impl_->control->processingPhase != nullptr;
}

std::optional<NativeImageStorageRefusal> GpuDevice::nativeImageStorageRefusal() const noexcept {
    if (!isOwnerThread() || !impl_->control || !impl_->control->processingPhase ||
        impl_->control->processingPhaseUnproven)
        return {};
    const auto& refusal = impl_->control->imageStorageRefusal;
    if (!refusal || refusal->phaseId != impl_->control->processingPhase->id())
        return {};
    return refusal;
}

std::optional<NativeBufferStorageRefusal> GpuDevice::nativeBufferStorageRefusal() const noexcept {
    if (!isOwnerThread() || !impl_->control || !impl_->control->processingPhase ||
        impl_->control->processingPhaseUnproven || impl_->control->processingPhase->isUnproven())
        return {};
    const auto& refusal = impl_->control->bufferStorageRefusal;
    if (!refusal || refusal->phaseId != impl_->control->processingPhase->id())
        return {};
    return refusal;
}
bool GpuDevice::displayTransferPending() const noexcept {
    return impl_ && impl_->control && display_transfer_detail::transferPendingFor(*impl_->control);
}
std::optional<NativePresentationStorageRefusal>
GpuDevice::nativePresentationStorageRefusal() const noexcept {
    return isOwnerThread() && impl_->control ? impl_->control->presentationStorageRefusal
                                             : std::nullopt;
}

bool GpuDevice::imageUploadPending() const noexcept {
    return impl_ && impl_->control && upload_detail::uploadPendingFor(*impl_->control);
}
bool GpuDevice::outputColorReadbackPending() const noexcept {
    return impl_ && impl_->control &&
           output_color_readback_detail::readbackPendingFor(*impl_->control);
}

GpuAllocationBudget GpuDevice::availableAllocationBudget() const noexcept {
    GpuAllocationBudget result;
    if (impl_ == nullptr || impl_->state != GpuDeviceState::Ready || impl_->control == nullptr) {
        return result;
    }
    if (std::this_thread::get_id() != impl_->owner) {
        return result;
    }
    auto& control = *impl_->control;
    result.device_local_bytes = control.deviceLocalBytes;

    if (control.allocator != VK_NULL_HANDLE && control.memoryBudgetEnabled) {
        // vmaGetHeapBudgets normally reuses its last driver sample until enough allocations pass
        // through this allocator. Refresh explicitly so an idle Bloom sees memory freed or used by
        // other processes on the owner's existing budget-sampling cadence.
        vmaSetCurrentFrameIndex(control.allocator, ++control.budgetFrameIndex);
        const VkPhysicalDeviceMemoryProperties* properties = nullptr;
        vmaGetMemoryProperties(control.allocator, &properties);
        if (properties != nullptr && properties->memoryHeapCount > 0U) {
            // VMA fills exactly memoryHeapCount entries; VK_MAX_MEMORY_HEAPS is the spec upper
            // bound, so a fixed stack array is always sufficient and this noexcept query never
            // allocates.
            std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
            vmaGetHeapBudgets(control.allocator, budgets.data());
            std::uint64_t available = 0;
            bool reported = false;
            const auto addSaturating = [&available](const std::uint64_t value) noexcept {
                const std::uint64_t sum = available + value;
                available = sum < available ? std::numeric_limits<std::uint64_t>::max() : sum;
            };
            for (std::uint32_t heap = 0;
                 heap < properties->memoryHeapCount && heap < VK_MAX_MEMORY_HEAPS; ++heap) {
                if ((properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0U) {
                    continue;
                }
                reported = true;
                const VmaBudget& budget = budgets[heap];
                if (budget.budget > budget.usage) {
                    addSaturating(budget.budget - budget.usage);
                }
            }
            if (reported) {
                result.source = GpuAllocationBudgetSource::MemoryBudget;
                result.available_bytes = available;
                return result;
            }
        }
    }

    // No live budget: report the summed DEVICE_LOCAL capacity as an explicitly-typed nominal
    // estimate. Callers that plan against it must apply their own conservative fraction; this is
    // never silently labelled as actual free VRAM.
    if (result.device_local_bytes > 0U) {
        result.source = GpuAllocationBudgetSource::DeviceLocalEstimate;
        result.available_bytes = result.device_local_bytes;
    }
    return result;
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
std::uint64_t deviceOwnerLifetimeToken() noexcept {
    static std::atomic<std::uint64_t> next{1};
    // A new thread lifetime gets a monotonic token even if the OS recycles std::thread::id.
    static thread_local const std::uint64_t token = next.fetch_add(1, std::memory_order_relaxed);
    return token;
}

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

namespace {
std::atomic<int> diagActiveTeardowns{0}; // DIAG-355: step-1 experiment only.
std::mutex nativeTeardownMutex;
} // namespace

DeviceAllocatorState::~DeviceAllocatorState() {
    assert(owner == std::this_thread::get_id());
    const int already = diagActiveTeardowns.fetch_add(1);
    if (already > 0)
        std::fprintf(stderr, "BLOOM-DIAG concurrent native device teardown (%d already)\n",
                     already);
    const std::lock_guard serialized(nativeTeardownMutex);
    if (processingPhase) {
        processingPhase->markUnproven();
    }
    if (allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator);
        allocator = VK_NULL_HANDLE;
    }
    computeQueue = nullptr;
    presentQueue = nullptr;
    device.clear();
    physicalDevice.clear();
    instance.clear();
    loader = DynamicLibrary{};
    diagActiveTeardowns.fetch_sub(1);
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
