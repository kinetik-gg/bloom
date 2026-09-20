#ifndef BLOOM_RENDER_VULKAN_GPU_IMAGE_UPLOAD_FAULT_HPP
#define BLOOM_RENDER_VULKAN_GPU_IMAGE_UPLOAD_FAULT_HPP

// Private to src/render/vulkan. Vulkan-free observability and test-only fault seam for the bounded
// GpuImageUpload resident pool. The production-owned atomics are always defined in the Vulkan
// translation unit (and inert in the CPU stub), so a shipping build carries only the readable
// atomic and never a fault path unless a test sets it. This header is never installed or included
// by a public render header.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bloom::render::upload_detail {

enum class UploadRetirementFault : std::uint8_t {
    None,
    // The owner drain treats an orphaned submission's fence as unproven without querying the real
    // fence, so retention and recovery can be proven deterministically.
    ForceFenceTimeout,
};

// Production-owned atomic read by the retirement path.
[[nodiscard]] std::atomic<std::uint8_t>& uploadRetirementFault() noexcept;

// Bounded resident pool observability. Every GpuImageUpload native resource set (command
// pool/buffer/fence plus any in-flight job and Ready resident) occupies exactly one fixed slot,
// acquired before its first native allocation and held until owner-thread release. A foreign-thread
// destruction only marks the already-owned slot orphaned and never destroys native state; the owner
// drain retires orphaned residents (allocation-free, noexcept) and returns their slots. Admission
// refuses cleanly when the pool is full and recovers once the owner drains. Production-owned
// (defined in the Vulkan translation unit and inert in the CPU stub).
[[nodiscard]] std::size_t uploadResidentCapacity() noexcept;
[[nodiscard]] std::size_t uploadResidentInUse() noexcept;
[[nodiscard]] std::size_t uploadResidentOrphaned() noexcept;
[[nodiscard]] std::uint64_t uploadResidentRefusals() noexcept;
[[nodiscard]] std::uint64_t uploadResidentRetired() noexcept;

// Defined in the Vulkan translation unit; tests call them.
void setUploadRetirementFaultForTest(UploadRetirementFault fault) noexcept;

} // namespace bloom::render::upload_detail

#endif // BLOOM_RENDER_VULKAN_GPU_IMAGE_UPLOAD_FAULT_HPP
