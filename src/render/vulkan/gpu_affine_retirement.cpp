#include "gpu_affine_private.hpp"

#include "gpu_affine_fault.hpp"
#include "gpu_bounded_retirement.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

// Bounded process-global resident pool and native orphan retirement for the GpuAffine
// (AffineBilinearV1) family. Mirrors the accepted GpuSolid/GpuImageUpload lifecycle: one
// fixed-capacity slot per native-resource-owning Impl, acquired before its first native allocation
// and held until owner-thread release. A foreign-thread destruction only orphans the already-owned
// slot; the owner drain proves fence retirement (or device loss) non-blockingly and then frees on
// the owner thread. The pool is the only retained store, so it is fixed and allocation-free and can
// never leak per object. Admission refuses cleanly when full and recovers after the owner drains:
// there is no permanent fuse.
//
// GpuAffine has its OWN tagged pool, replacing the former process-global quarantine counter/fuse.
// The drain callback casts a slot's opaque pointer back to this family's private Impl, so a shared
// store across different Impl layouts would be a type-confusion hazard, and each family must be
// able to report and recover its own pressure independently.
//
// The slot operations are members of the private nested Impl because only a member context may name
// that type; the observability/fault functions are free and type-erased.

namespace bloom::render {
namespace {

constexpr std::size_t kAffineResidentCapacity = 8;

// Distinguishes this family's function-local static store from every other family's.
struct AffineResidentTag final {};

primitive_detail::VoidResidentSlotStore<kAffineResidentCapacity>& affineSlots() noexcept {
    // Allocation-free, intentionally immortal function-local storage (see
    // immortalResidentSlotStore). No heap allocation on first use; the store never deletes an Impl.
    return primitive_detail::immortalResidentSlotStore<kAffineResidentCapacity,
                                                       AffineResidentTag>();
}

std::atomic<std::uint8_t>& affineRetirementFaultCell() noexcept {
    static std::atomic<std::uint8_t> value{0};
    return value;
}

} // namespace

bool GpuAffine::Impl::acquireResidentSlot() noexcept {
    if (residentSlot != kAffineNoResidentSlot) {
        return true;
    }
    std::size_t index = 0;
    if (!affineSlots().acquire(this, index)) {
        return false;
    }
    residentSlot = index;
    return true;
}

void GpuAffine::Impl::releaseResidentSlot() noexcept {
    if (residentSlot >= kAffineResidentCapacity) {
        return;
    }
    affineSlots().release(this, residentSlot);
    residentSlot = kAffineNoResidentSlot;
}

void GpuAffine::Impl::orphanResidentSlot() noexcept {
    if (residentSlot >= kAffineResidentCapacity) {
        return;
    }
    affineSlots().orphan(this, residentSlot);
}

void GpuAffine::Impl::drainResidentOrphansOnOwnerThread() noexcept {
    affineSlots().drainOrphans(
        [](void* const raw) noexcept {
            // Exact ownership gate: only the device owner thread that created the Impl, and only
            // while it still belongs to its device generation, may touch its Vulkan state.
            const auto* const impl = static_cast<const GpuAffine::Impl*>(raw);
            return impl != nullptr && impl->owner != std::thread::id{} &&
                   impl->owner == std::this_thread::get_id() && impl->control != nullptr &&
                   impl->control->generation == impl->expectedGeneration;
        },
        [](void* const raw) noexcept {
            auto* const impl = static_cast<GpuAffine::Impl*>(raw);
            if (impl == nullptr || !impl->queueSubmitted) {
                return true;
            }
            if (static_cast<affine_detail::AffineRetirementFault>(
                    affineRetirementFaultCell().load()) ==
                affine_detail::AffineRetirementFault::ForceFenceTimeout) {
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
            auto* const impl = static_cast<GpuAffine::Impl*>(raw);
            // Clear the back-pointer before the Impl is destroyed so a stale destructor can never
            // address a slot that may already have been re-admitted.
            impl->residentSlot = kAffineNoResidentSlot;
            delete impl;
        });
}

namespace affine_detail {

std::size_t affineResidentCapacity() noexcept { return kAffineResidentCapacity; }
std::size_t affineResidentInUse() noexcept { return affineSlots().inUse(); }
std::size_t affineResidentOrphaned() noexcept { return affineSlots().orphaned(); }
std::uint64_t affineResidentRefusals() noexcept { return affineSlots().refusals(); }
std::uint64_t affineResidentRetired() noexcept { return affineSlots().retired(); }

std::atomic<std::uint8_t>& affineRetirementFault() noexcept { return affineRetirementFaultCell(); }

void setAffineRetirementFaultForTest(const AffineRetirementFault fault) noexcept {
    affineRetirementFaultCell().store(static_cast<std::uint8_t>(fault));
}

} // namespace affine_detail

} // namespace bloom::render
