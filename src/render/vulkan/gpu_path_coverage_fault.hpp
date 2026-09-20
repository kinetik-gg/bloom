#pragma once

// Private test-only fault-injection seam for GpuPathCoverage. The production-owned atomics are
// always defined in the Vulkan translation unit (and inert in the CPU stub), so a shipping build
// carries only the readable atomic and never a fault path unless a test sets it. The setters are
// declared here and defined in the same translation units; this header is never installed or
// included by a public render header.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bloom::render {
struct GpuPathCoverageImpl;
} // namespace bloom::render

namespace bloom::render::path_coverage_detail {

// Acquire/release a resident slot. Acquire is called before the Impl's first native allocation;
// release is owner-thread retirement. Defined in the Vulkan translation unit.
[[nodiscard]] bool acquireResidentSlot(GpuPathCoverageImpl* impl) noexcept;
void releaseResidentSlot(GpuPathCoverageImpl* impl) noexcept;

enum class PathCoverageFault : std::uint8_t {
    None,
    // vkQueueSubmit is forced to fail before any submission is considered in flight.
    FailSubmit,
    // poll() treats the fence as unretired without waiting on the real fence, so the submission is
    // quarantined deterministically.
    ForceFenceTimeout,
    // poll() reports device loss, proving the exact submission is released without quarantine.
    ForceDeviceLost,
};

// Production-owned atomic read by the producer.
[[nodiscard]] std::atomic<std::uint8_t>& pathCoverageFault() noexcept;

// Test-only override of the effective compute workgroup count limits; 0 uses the real device value.
[[nodiscard]] std::atomic<std::uint32_t>& pathCoverageMaxWorkGroupCountXOverride() noexcept;
[[nodiscard]] std::atomic<std::uint32_t>& pathCoverageMaxWorkGroupCountYOverride() noexcept;

// Test/observability seams for the bounded reservation.
[[nodiscard]] bool pathCoverageQuarantineOccupied() noexcept;
[[nodiscard]] bool retirePathCoverageQuarantineForOwner() noexcept;

// Bounded resident pool. Every GpuPathCoverage native resource set (compute pipeline plus resident
// mask plus any in-flight job resources) occupies exactly one of a fixed number of pool slots,
// acquired before its first native allocation and held until owner-thread retirement. A foreign
// thread destruction only marks the already-owned slot orphaned and never destroys native state;
// the owner drain retires orphaned residents (allocation-free, noexcept) and returns their slots.
// Admission refuses cleanly when the pool is full, and recovers once the owner drains. Production-
// owned (defined in the Vulkan translation unit and inert in the CPU stub).
[[nodiscard]] std::size_t pathCoverageResidentCapacity() noexcept;
[[nodiscard]] std::size_t pathCoverageResidentInUse() noexcept;
[[nodiscard]] std::size_t pathCoverageResidentOrphaned() noexcept;
[[nodiscard]] std::uint64_t pathCoverageResidentRefusals() noexcept;
[[nodiscard]] std::uint64_t pathCoverageResidentRetired() noexcept;
void drainPathCoverageResidentOrphansOnOwnerThread() noexcept;

// Defined in the Vulkan translation unit and the CPU stub; tests call them.
void setPathCoverageFaultForTest(PathCoverageFault fault) noexcept;
void setPathCoverageMaxWorkGroupCountXForTest(std::uint32_t value) noexcept;
void setPathCoverageMaxWorkGroupCountYForTest(std::uint32_t value) noexcept;
[[nodiscard]] bool pathCoverageQuarantineOccupiedForTest() noexcept;
[[nodiscard]] bool retirePathCoverageQuarantineForTest() noexcept;

} // namespace bloom::render::path_coverage_detail
