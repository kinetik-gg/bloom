#include "gpu_composite_private.hpp"

#include "gpu_bounded_retirement.hpp"
#include "gpu_composite_fault.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

// Bounded process-global resident pool and native orphan retirement for the GpuComposite
// (TranslationOpacityBilinearV1 / SourceOverV1) family. Mirrors the accepted
// GpuSolid/GpuImageUpload lifecycle: one fixed-capacity slot per native-resource-owning Impl,
// acquired before its first native allocation and held until owner-thread release. A foreign-thread
// destruction only orphans the already-owned slot; the owner drain proves fence retirement (or
// device loss) non-blockingly and then frees on the owner thread. The pool is the only retained
// store, so it is fixed and allocation-free and can never leak per object. Admission refuses
// cleanly when full and recovers after the owner drains: there is no permanent fuse.
//
// GpuComposite has its OWN tagged pool. It deliberately does not share the GpuBlend pool (or any
// other family's): the drain callback casts a slot's opaque pointer back to this family's private
// Impl, so a shared store across different Impl layouts would be a type-confusion hazard.
//
// The slot operations are members of the private nested Impl because only a member context may name
// that type; the observability/fault functions are free and type-erased.

namespace bloom::render {
namespace {

constexpr std::size_t kCompositeResidentCapacity = 8;

// Distinguishes this family's function-local static store from every other family's.
struct CompositeResidentTag final {};

primitive_detail::VoidResidentSlotStore<kCompositeResidentCapacity>& compositeSlots() noexcept {
    // Allocation-free, intentionally immortal function-local storage (see
    // immortalResidentSlotStore). No heap allocation on first use; the store never deletes an Impl.
    return primitive_detail::immortalResidentSlotStore<kCompositeResidentCapacity,
                                                       CompositeResidentTag>();
}

std::atomic<std::uint8_t>& compositeRetirementFaultCell() noexcept {
    static std::atomic<std::uint8_t> value{0};
    return value;
}

} // namespace

bool GpuComposite::Impl::acquireResidentSlot() noexcept {
    if (residentSlot != kCompositeNoResidentSlot) {
        return true;
    }
    std::size_t index = 0;
    if (!compositeSlots().acquire(this, index)) {
        return false;
    }
    residentSlot = index;
    return true;
}

void GpuComposite::Impl::releaseResidentSlot() noexcept {
    if (residentSlot >= kCompositeResidentCapacity) {
        return;
    }
    compositeSlots().release(this, residentSlot);
    residentSlot = kCompositeNoResidentSlot;
}

void GpuComposite::Impl::orphanResidentSlot() noexcept {
    if (residentSlot >= kCompositeResidentCapacity) {
        return;
    }
    compositeSlots().orphan(this, residentSlot);
}

void GpuComposite::Impl::drainResidentOrphansOnOwnerThread() noexcept {
    compositeSlots().drainOrphans(
        [](void* const raw) noexcept {
            // Exact ownership gate: only the device owner thread that created the Impl, and only
            // while it still belongs to its device generation, may touch its Vulkan state.
            const auto* const impl = static_cast<const GpuComposite::Impl*>(raw);
            return impl != nullptr && impl->owner != std::thread::id{} &&
                   impl->owner == std::this_thread::get_id() && impl->control != nullptr &&
                   impl->control->generation == impl->expectedGeneration;
        },
        [](void* const raw) noexcept {
            auto* const impl = static_cast<GpuComposite::Impl*>(raw);
            if (impl == nullptr || !impl->queueSubmitted) {
                return true;
            }
            if (static_cast<composite_detail::CompositeRetirementFault>(
                    compositeRetirementFaultCell().load()) ==
                composite_detail::CompositeRetirementFault::ForceFenceTimeout) {
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
            auto* const impl = static_cast<GpuComposite::Impl*>(raw);
            // Clear the back-pointer before the Impl is destroyed so a stale destructor can never
            // address a slot that may already have been re-admitted.
            impl->residentSlot = kCompositeNoResidentSlot;
            delete impl;
        });
}

namespace composite_detail {

std::size_t compositeResidentCapacity() noexcept { return kCompositeResidentCapacity; }
std::size_t compositeResidentInUse() noexcept { return compositeSlots().inUse(); }
std::size_t compositeResidentOrphaned() noexcept { return compositeSlots().orphaned(); }
std::uint64_t compositeResidentRefusals() noexcept { return compositeSlots().refusals(); }
std::uint64_t compositeResidentRetired() noexcept { return compositeSlots().retired(); }

std::atomic<std::uint8_t>& compositeRetirementFault() noexcept {
    return compositeRetirementFaultCell();
}

void setCompositeRetirementFaultForTest(const CompositeRetirementFault fault) noexcept {
    compositeRetirementFaultCell().store(static_cast<std::uint8_t>(fault));
}

} // namespace composite_detail

} // namespace bloom::render
