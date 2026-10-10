#include "gpu_image_upload_private.hpp"

#include "gpu_bounded_retirement.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

// Bounded process-global resident pool and native orphan retirement for the GpuImageUpload family.
// Mirrors the accepted GpuPathCoverage lifecycle: one fixed-capacity slot per
// native-resource-owning Impl, acquired before its first native allocation and held until
// owner-thread release. A foreign-thread destruction only orphans the already-owned slot; the owner
// drain proves fence retirement (or device loss) non-blockingly and then frees on the owner thread.
// The pool is the only retained store, so it is fixed and allocation-free and can never leak per
// object. Admission refuses cleanly when full and recovers after the owner drains: there is no
// permanent fuse.
//
// The slot operations are members of the private nested Impl because only a member context may name
// that type; the observability/fault functions are free and type-erased.

namespace bloom::render {
namespace {

constexpr std::size_t kUploadResidentCapacity = 8;

// Distinguishes the GpuImageUpload pool from every other family's function-local static store.
struct UploadResidentTag final {};

primitive_detail::VoidResidentSlotStore<kUploadResidentCapacity>& uploadSlots() noexcept {
    // Allocation-free, intentionally immortal function-local storage (see
    // immortalResidentSlotStore). No heap allocation on first use; the store never deletes an Impl.
    return primitive_detail::immortalResidentSlotStore<kUploadResidentCapacity,
                                                       UploadResidentTag>();
}

std::atomic<std::uint8_t>& uploadRetirementFaultCell() noexcept {
    static std::atomic<std::uint8_t> value{0};
    return value;
}

} // namespace

bool GpuImageUpload::Impl::acquireResidentSlot() noexcept {
    if (residentSlot != kUploadNoResidentSlot) {
        return true;
    }
    std::size_t index = 0;
    if (!uploadSlots().acquire(this, index)) {
        return false;
    }
    residentSlot = index;
    uploadSlots().setImageMetadata(this, index, control.get(), 0);
    return true;
}

void GpuImageUpload::Impl::releaseResidentSlot() noexcept {
    if (residentSlot >= kUploadResidentCapacity) {
        return;
    }
    uploadSlots().release(this, residentSlot);
    upload_detail::recordUploadInitializationSlotReturn();
    residentSlot = kUploadNoResidentSlot;
}

void GpuImageUpload::Impl::orphanResidentSlot() noexcept {
    if (residentSlot >= kUploadResidentCapacity) {
        return;
    }
    uploadSlots().orphan(this, residentSlot);
}

void GpuImageUpload::Impl::drainResidentOrphansOnOwnerThread(
    const vulkan_detail::DeviceAllocatorState* exactOwner) noexcept {
    uploadSlots().drainRetainedOrphansOutsideLock(
        [exactOwner](void* const raw) noexcept {
            // Exact ownership gate: only the device owner thread that created the Impl, and only
            // while it still belongs to its device generation, may touch its Vulkan state.
            const auto* const impl = static_cast<const GpuImageUpload::Impl*>(raw);
            return impl != nullptr && impl->owner != std::thread::id{} &&
                   impl->owner == std::this_thread::get_id() && impl->control != nullptr &&
                   (!exactOwner || impl->control.get() == exactOwner) &&
                   !impl->control->imageOwnerClosed &&
                   impl->control->presentationEpoch == impl->expectedEpoch &&
                   impl->control->ownerLifetimeToken == vulkan_detail::deviceOwnerLifetimeToken();
        },
        [](void* const raw) noexcept {
            auto* const impl = static_cast<GpuImageUpload::Impl*>(raw);
            if (impl == nullptr || !impl->queueSubmitted) {
                return true;
            }
            if (static_cast<upload_detail::UploadRetirementFault>(
                    uploadRetirementFaultCell().load()) ==
                upload_detail::UploadRetirementFault::ForceFenceTimeout) {
                return false;
            }
            if (impl->fence == vk::raii::Fence{nullptr}) {
                return false;
            }
            const VkResult status = impl->control->device.getDispatcher()->vkGetFenceStatus(
                static_cast<VkDevice>(*impl->control->device), static_cast<VkFence>(*impl->fence));
            if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) {
                return false;
            }
            impl->deviceLost = impl->deviceLost || status == VK_ERROR_DEVICE_LOST;
            impl->queueSubmitted = false;
            return true;
        },
        [](void* const raw) noexcept {
            auto* const impl = static_cast<GpuImageUpload::Impl*>(raw);
            delete impl;
        });
}

namespace upload_detail {

void drainUploads(vulkan_detail::DeviceAllocatorState& state) noexcept {
    GpuImageUploadRetirementAccess::drain(state);
}
void closeUploads(vulkan_detail::DeviceAllocatorState& state) noexcept {
    if (state.owner != std::this_thread::get_id() ||
        state.ownerLifetimeToken != vulkan_detail::deviceOwnerLifetimeToken())
        return;
    drainUploads(state);
    uploadSlots().closeImageOwner(&state);
}
bool uploadPendingFor(const vulkan_detail::DeviceAllocatorState& state) noexcept {
    bool pending = false;
    uploadSlots().inspectImageMetadata([&](const void* owner, std::uint64_t, bool, bool) {
        pending = pending || owner == &state;
    });
    return pending;
}

std::size_t uploadResidentCapacity() noexcept { return kUploadResidentCapacity; }
std::size_t uploadResidentInUse() noexcept { return uploadSlots().inUse(); }
std::size_t uploadResidentOrphaned() noexcept { return uploadSlots().orphaned(); }
std::uint64_t uploadResidentRefusals() noexcept { return uploadSlots().refusals(); }
std::uint64_t uploadResidentRetired() noexcept { return uploadSlots().retired(); }

std::atomic<std::uint8_t>& uploadRetirementFault() noexcept { return uploadRetirementFaultCell(); }

void setUploadRetirementFaultForTest(const UploadRetirementFault fault) noexcept {
    uploadRetirementFaultCell().store(static_cast<std::uint8_t>(fault));
}

} // namespace upload_detail

} // namespace bloom::render
