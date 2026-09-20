#include "gpu_solid_private.hpp"

#include "gpu_bounded_retirement.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

// Bounded process-global resident pool and native orphan retirement for the GpuSolid /
// CoveredSolidV1 family. Mirrors the accepted GpuPathCoverage lifecycle: one fixed-capacity slot
// per native-resource-owning Impl, acquired before its first native allocation and held until
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

constexpr std::size_t kSolidResidentCapacity = 8;

// Distinguishes the Solid/CoveredSolidV1 pool from every other family's function-local static
// store.
struct SolidResidentTag final {};

primitive_detail::VoidResidentSlotStore<kSolidResidentCapacity>& solidSlots() noexcept {
    // Allocation-free, intentionally immortal function-local storage (see
    // immortalResidentSlotStore). No heap allocation on first use; the store never deletes an Impl.
    return primitive_detail::immortalResidentSlotStore<kSolidResidentCapacity, SolidResidentTag>();
}

std::atomic<std::uint8_t>& solidRetirementFaultCell() noexcept {
    static std::atomic<std::uint8_t> value{0};
    return value;
}

} // namespace

bool GpuSolid::Impl::acquireResidentSlot() noexcept {
    if (residentSlot != kSolidNoResidentSlot) {
        return true;
    }
    std::size_t index = 0;
    if (!solidSlots().acquire(this, index)) {
        return false;
    }
    residentSlot = index;
    return true;
}

void GpuSolid::Impl::releaseResidentSlot() noexcept {
    if (residentSlot >= kSolidResidentCapacity) {
        return;
    }
    solidSlots().release(this, residentSlot);
    residentSlot = kSolidNoResidentSlot;
}

void GpuSolid::Impl::orphanResidentSlot() noexcept {
    if (residentSlot >= kSolidResidentCapacity) {
        return;
    }
    solidSlots().orphan(this, residentSlot);
}

void GpuSolid::Impl::drainResidentOrphansOnOwnerThread() noexcept {
    solidSlots().drainOrphans(
        [](void* const raw) noexcept {
            // Exact ownership gate: only the device owner thread that created the Impl, and only
            // while it still belongs to its device generation, may touch its Vulkan state.
            const auto* const impl = static_cast<const GpuSolid::Impl*>(raw);
            return impl != nullptr && impl->owner != std::thread::id{} &&
                   impl->owner == std::this_thread::get_id() && impl->control != nullptr &&
                   impl->control->generation == impl->expectedGeneration;
        },
        [](void* const raw) noexcept {
            auto* const impl = static_cast<GpuSolid::Impl*>(raw);
            if (impl == nullptr || !impl->queueSubmitted) {
                return true;
            }
            if (static_cast<solid_detail::SolidRetirementFault>(
                    solidRetirementFaultCell().load()) ==
                solid_detail::SolidRetirementFault::ForceFenceTimeout) {
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
            auto* const impl = static_cast<GpuSolid::Impl*>(raw);
            // Clear the back-pointer before the Impl is destroyed so a stale destructor can never
            // address a slot that may already have been re-admitted.
            impl->residentSlot = kSolidNoResidentSlot;
            delete impl;
        });
}

namespace solid_detail {

std::size_t solidResidentCapacity() noexcept { return kSolidResidentCapacity; }
std::size_t solidResidentInUse() noexcept { return solidSlots().inUse(); }
std::size_t solidResidentOrphaned() noexcept { return solidSlots().orphaned(); }
std::uint64_t solidResidentRefusals() noexcept { return solidSlots().refusals(); }
std::uint64_t solidResidentRetired() noexcept { return solidSlots().retired(); }

std::atomic<std::uint8_t>& solidRetirementFault() noexcept { return solidRetirementFaultCell(); }

void setSolidRetirementFaultForTest(const SolidRetirementFault fault) noexcept {
    solidRetirementFaultCell().store(static_cast<std::uint8_t>(fault));
}

} // namespace solid_detail

} // namespace bloom::render
