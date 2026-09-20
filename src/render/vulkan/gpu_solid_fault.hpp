#ifndef BLOOM_RENDER_VULKAN_GPU_SOLID_FAULT_HPP
#define BLOOM_RENDER_VULKAN_GPU_SOLID_FAULT_HPP

// Private to src/render/vulkan. Vulkan-free observability and test-only fault seam for the bounded
// GpuSolid/GpuCoveredSolid resident pool. The production-owned atomics are always defined in the
// Vulkan translation unit (and inert in the CPU stub), so a shipping build carries only the
// readable atomic and never a fault path unless a test sets it. This header is never installed or
// included by a public render header.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bloom::render {
struct GpuSolidImpl;
} // namespace bloom::render

namespace bloom::render::solid_detail {

enum class SolidRetirementFault : std::uint8_t {
    None,
    // The owner drain treats an orphaned submission's fence as unproven without querying the real
    // fence, so retention and recovery can be proven deterministically.
    ForceFenceTimeout,
};

// Production-owned atomic read by the retirement path.
[[nodiscard]] std::atomic<std::uint8_t>& solidRetirementFault() noexcept;

// Bounded resident pool observability. Every GpuSolid native resource set (compute pipeline plus any
// in-flight job and Ready resident) occupies exactly one fixed slot, acquired before its first
// native allocation and held until owner-thread release. A foreign-thread destruction only marks
// the already-owned slot orphaned and never destroys native state; the owner drain retires orphaned
// residents (allocation-free, noexcept) and returns their slots. Admission refuses cleanly when the
// pool is full and recovers once the owner drains. Production-owned (defined in the Vulkan
// translation unit and inert in the CPU stub).
[[nodiscard]] std::size_t solidResidentCapacity() noexcept;
[[nodiscard]] std::size_t solidResidentInUse() noexcept;
[[nodiscard]] std::size_t solidResidentOrphaned() noexcept;
[[nodiscard]] std::uint64_t solidResidentRefusals() noexcept;
[[nodiscard]] std::uint64_t solidResidentRetired() noexcept;

// Defined in the Vulkan translation unit; tests call them.
void setSolidRetirementFaultForTest(SolidRetirementFault fault) noexcept;

} // namespace bloom::render::solid_detail

#endif // BLOOM_RENDER_VULKAN_GPU_SOLID_FAULT_HPP
