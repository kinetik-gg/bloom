#ifndef BLOOM_RENDER_VULKAN_GPU_AFFINE_FAULT_HPP
#define BLOOM_RENDER_VULKAN_GPU_AFFINE_FAULT_HPP

// Private to src/render/vulkan. Vulkan-free observability and test-only fault seam for the bounded
// GpuAffine (AffineBilinearV1) resident pool. The production-owned atomics are always defined in
// the Vulkan translation unit (and inert in the CPU stub), so a shipping build carries only the
// readable atomic and never a fault path unless a test sets it. This header is never installed or
// included by a public render header.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bloom::render::affine_detail {

enum class AffineRetirementFault : std::uint8_t {
    None,
    // The owner drain treats an orphaned submission's fence as unproven without querying the real
    // fence, so retention and recovery can be proven deterministically.
    ForceFenceTimeout,
    // Pipeline creation fails after the complete native resource set is built, so the bounded-slot
    // cleanup path (children before parents) and the subsequent retry can be proven
    // deterministically.
    FailPipelineCreation,
};

// Production-owned atomic read by the retirement path.
[[nodiscard]] std::atomic<std::uint8_t>& affineRetirementFault() noexcept;

// Bounded resident pool observability. Every GpuAffine native resource set (the AffineBilinearV1
// pipeline plus the shared descriptor/command/fence state, any in-flight job and Ready resident)
// occupies exactly one fixed slot, acquired before its first native allocation and held until
// owner-thread release. A foreign-thread destruction only marks the already-owned slot orphaned and
// never destroys native state; the owner drain retires orphaned residents (allocation-free,
// noexcept) and returns their slots. Admission refuses cleanly when the pool is full and recovers
// once the owner drains. Production-owned (defined in the Vulkan translation unit and inert in the
// CPU stub).
[[nodiscard]] std::size_t affineResidentCapacity() noexcept;
[[nodiscard]] std::size_t affineResidentInUse() noexcept;
[[nodiscard]] std::size_t affineResidentOrphaned() noexcept;
[[nodiscard]] std::uint64_t affineResidentRefusals() noexcept;
[[nodiscard]] std::uint64_t affineResidentRetired() noexcept;

// Defined in the Vulkan translation unit; tests call them.
void setAffineRetirementFaultForTest(AffineRetirementFault fault) noexcept;

} // namespace bloom::render::affine_detail

#endif // BLOOM_RENDER_VULKAN_GPU_AFFINE_FAULT_HPP
