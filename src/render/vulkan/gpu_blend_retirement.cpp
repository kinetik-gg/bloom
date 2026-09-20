#include "gpu_blend_private.hpp"

#include "gpu_blend_fault.hpp"
#include "gpu_bounded_retirement.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

// Bounded process-global resident pool and native orphan retirement for the GpuBlend (BlendV1)
// family. Mirrors the accepted GpuSolid/GpuImageUpload lifecycle: one fixed-capacity slot per
// native-resource-owning Impl, acquired before its first native allocation and held until
// owner-thread release. A foreign-thread destruction only orphans the already-owned slot; the owner
// drain proves fence retirement (or device loss) non-blockingly and then frees on the owner thread.
// The pool is the only retained store, so it is fixed and allocation-free and can never leak per
// object. Admission refuses cleanly when full and recovers after the owner drains: there is no
// permanent fuse.
//
// GpuBlend has its OWN tagged pool. It no longer shares the composite quarantine counter/fuse: the
// drain callback casts a slot's opaque pointer back to this family's private Impl, so a shared
// store across different Impl layouts would be a type-confusion hazard, and each family must be
// able to report and recover its own pressure independently.
//
// The slot operations are members of the private nested Impl because only a member context may name
// that type; the observability/fault functions are free and type-erased.

namespace bloom::render {
namespace {

constexpr std::size_t kBlendResidentCapacity = 8;

// Distinguishes this family's function-local static store from every other family's.
struct BlendResidentTag final {};

primitive_detail::VoidResidentSlotStore<kBlendResidentCapacity>& blendSlots() noexcept {
    // Allocation-free, intentionally immortal function-local storage (see
    // immortalResidentSlotStore). No heap allocation on first use; the store never deletes an Impl.
    return primitive_detail::immortalResidentSlotStore<kBlendResidentCapacity, BlendResidentTag>();
}

std::atomic<std::uint8_t>& blendRetirementFaultCell() noexcept {
    static std::atomic<std::uint8_t> value{0};
    return value;
}

} // namespace

bool GpuBlend::Impl::acquireResidentSlot() noexcept {
    if (residentSlot != kBlendNoResidentSlot) {
        return true;
    }
    std::size_t index = 0;
    if (!blendSlots().acquire(this, index)) {
        return false;
    }
    residentSlot = index;
    return true;
}

void GpuBlend::Impl::releaseResidentSlot() noexcept {
    if (residentSlot >= kBlendResidentCapacity) {
        return;
    }
    blendSlots().release(this, residentSlot);
    residentSlot = kBlendNoResidentSlot;
}

void GpuBlend::Impl::orphanResidentSlot() noexcept {
    if (residentSlot >= kBlendResidentCapacity) {
        return;
    }
    blendSlots().orphan(this, residentSlot);
}

void GpuBlend::Impl::drainResidentOrphansOnOwnerThread() noexcept {
    blendSlots().drainOrphans(
        [](void* const raw) noexcept {
            // Exact ownership gate: only the device owner thread that created the Impl, and only
            // while it still belongs to its device generation, may touch its Vulkan state.
            const auto* const impl = static_cast<const GpuBlend::Impl*>(raw);
            return impl != nullptr && impl->owner != std::thread::id{} &&
                   impl->owner == std::this_thread::get_id() && impl->control != nullptr &&
                   impl->control->generation == impl->expectedGeneration;
        },
        [](void* const raw) noexcept {
            auto* const impl = static_cast<GpuBlend::Impl*>(raw);
            if (impl == nullptr || !impl->queueSubmitted) {
                return true;
            }
            if (static_cast<blend_detail::BlendRetirementFault>(
                    blendRetirementFaultCell().load()) ==
                blend_detail::BlendRetirementFault::ForceFenceTimeout) {
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
            auto* const impl = static_cast<GpuBlend::Impl*>(raw);
            // Clear the back-pointer before the Impl is destroyed so a stale destructor can never
            // address a slot that may already have been re-admitted.
            impl->residentSlot = kBlendNoResidentSlot;
            delete impl;
        });
}

namespace blend_detail {

std::size_t blendResidentCapacity() noexcept { return kBlendResidentCapacity; }
std::size_t blendResidentInUse() noexcept { return blendSlots().inUse(); }
std::size_t blendResidentOrphaned() noexcept { return blendSlots().orphaned(); }
std::uint64_t blendResidentRefusals() noexcept { return blendSlots().refusals(); }
std::uint64_t blendResidentRetired() noexcept { return blendSlots().retired(); }

std::atomic<std::uint8_t>& blendRetirementFault() noexcept { return blendRetirementFaultCell(); }

void setBlendRetirementFaultForTest(const BlendRetirementFault fault) noexcept {
    blendRetirementFaultCell().store(static_cast<std::uint8_t>(fault));
}

} // namespace blend_detail

} // namespace bloom::render
