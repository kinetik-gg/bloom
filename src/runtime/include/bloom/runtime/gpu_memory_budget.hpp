#pragma once

// Capacity-aware ceilings for the GPU production path.
//
// Every value is a conservative fraction of the budget the process memory ledger actually ASSIGNED
// (which already honours the operator's operation/preview overrides and the live host cap). There
// is deliberately no floor and no arbitrary replacement cutoff: an explicitly tiny assigned budget
// yields a tiny ceiling, so a deliberate limit is never silently defeated, and a large assigned
// budget admits a large source. The fraction is computed with integer division before any
// multiplication, so the arithmetic cannot overflow.
//
// Host and device budgets stay distinct:
//  * the prepared-upload cache is HOST-retained (decoded/converted images);
//  * the scene cache and the per-request executor allowance are DEVICE admission bounds. They are
//    host-derived here because the policy is host-side, and the native primitives additionally
//    clamp every per-resource request to the device's real limits: a VkImage request is bounded by
//    the format's maxResourceSize (querySolidImageSupport), and a buffer-backed request (coverage
//    mask, affine coordinate buffer) by the device's storage-buffer range. The effective device
//    limit is therefore min(host-derived bound, device limit), and a device that cannot allocate
//    refuses honestly and takes the CPU fallback.
//
// The `...For` overloads are pure functions of an assigned budget so a policy test can prove the
// tiny-budget and high-capacity cases without a device or the process ledger.

#include <bloom/runtime/memory_budget_ledger.hpp>

#include <algorithm>
#include <cstddef>

namespace bloom::runtime {
namespace gpu_memory_budget_detail {

[[nodiscard]] inline std::size_t fraction(const std::size_t budget, const std::size_t numerator,
                                          const std::size_t denominator) noexcept {
    return (budget / denominator) * numerator;
}

} // namespace gpu_memory_budget_detail

// Upper bound on a single native image, from the assigned operation-cache budget. A device whose
// image format maxResourceSize is smaller clamps below this; a 6000x4000 RGBA32F frame (384 MB) is
// admitted only when the assigned budget and the device both allow it.
[[nodiscard]] inline std::size_t
gpuProducerMaxImageBytesFor(const std::size_t operationCacheByteBudget) noexcept {
    return gpu_memory_budget_detail::fraction(operationCacheByteBudget, 1, 4);
}
[[nodiscard]] inline std::size_t gpuProducerMaxImageBytes() noexcept {
    return gpuProducerMaxImageBytesFor(
        processMemoryBudgetLedger().allocate().operationCacheByteBudget);
}

// The converted-upload cache: a HOST-retained derived cache, one sixteenth of the assigned
// operation-cache budget. At a large assigned budget a 4608x3164 source (~233 MB) is retained
// instead of re-decoded every request; at a small one it is bounded accordingly.
[[nodiscard]] inline std::size_t
gpuPreparedUploadCacheByteBudgetFor(const std::size_t operationCacheByteBudget) noexcept {
    return gpu_memory_budget_detail::fraction(operationCacheByteBudget, 1, 16);
}
[[nodiscard]] inline std::size_t gpuPreparedUploadCacheByteBudget() noexcept {
    return gpuPreparedUploadCacheByteBudgetFor(
        processMemoryBudgetLedger().allocate().operationCacheByteBudget);
}

// The resident retained scene cache: one eighth of the assigned operation-cache budget. The cache
// still evicts unpinned entries and never invalidates a live pin.
[[nodiscard]] inline std::size_t
gpuSceneCacheRetainedByteBudgetFor(const std::size_t operationCacheByteBudget) noexcept {
    return gpu_memory_budget_detail::fraction(operationCacheByteBudget, 1, 8);
}
[[nodiscard]] inline std::size_t gpuSceneCacheRetainedByteBudget() noexcept {
    return gpuSceneCacheRetainedByteBudgetFor(
        processMemoryBudgetLedger().allocate().operationCacheByteBudget);
}

// The per-request preview display byte allowance (also the GPU request-owned admission
// reservation): one quarter of the assigned RAM-preview budget. It must cover the LIVE GPU set,
// which for a full-resolution source over an FHD composition is roughly two source frames.
[[nodiscard]] inline std::size_t
gpuPreviewRequestByteAllowanceFor(const std::size_t previewFrameCacheByteBudget) noexcept {
    return gpu_memory_budget_detail::fraction(previewFrameCacheByteBudget, 1, 4);
}
[[nodiscard]] inline std::size_t gpuPreviewRequestByteAllowance() noexcept {
    return gpuPreviewRequestByteAllowanceFor(
        processMemoryBudgetLedger().allocate().previewFrameCacheByteBudget);
}

// Deterministic default for the task scheduler's AGGREGATE GPU request-owned reservation ceiling
// (the sum of `GpuTaskAdmission::requestOwnedBytes` over pending/accepted GPU tasks). It is
// host-derived and ALWAYS valid: the assigned operation-cache split when positive, else the usable
// host budget, else a 1-byte validity floor, clamped to the usable host budget. It is a finite
// safety ceiling, never a device-capability claim, and it deliberately replaces the old fixed 1 GiB
// default that silently refused a full-resolution resident preview.
[[nodiscard]] inline std::size_t
defaultGpuRequestOwnedByteCapacityFor(const std::size_t usableBudget,
                                      const std::size_t operationCacheBudget) noexcept {
    const std::size_t ceiling = usableBudget > 0 ? usableBudget : std::size_t{1};
    const std::size_t budget = operationCacheBudget > 0 ? operationCacheBudget : ceiling;
    return std::clamp(budget, std::size_t{1}, ceiling);
}

// The live default for the current process.
[[nodiscard]] inline std::size_t defaultGpuRequestOwnedByteCapacity() noexcept {
    return defaultGpuRequestOwnedByteCapacityFor(
        processMemoryBudgetLedger().usableByteBudget(),
        processMemoryBudgetLedger().allocate().operationCacheByteBudget);
}

} // namespace bloom::runtime
