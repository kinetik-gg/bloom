#pragma once

// Capacity-aware admission policy for the GPU-resident preview route.
//
// The resident frame cache must never be sized from host RAM alone: a large RAM-preview budget is
// not evidence of usable VRAM. This slice resolves the real device allocation budget on the GPU
// owner thread (render::GpuDevice::availableAllocationBudget()) and partitions ONE resident pool
// into the shared frame-cache, lease-registry, scene-cache, and per-request sub-budgets so no
// ledger can overcommit the device. Every value stays finite, is never raised above the artist's
// explicit configured ceiling, and an unknown device budget falls back to a small safe pool.
//
// Pure functions only: no device, no thread, no allocation. The owner thread produces the resolved
// capacity and the clamps; tests exercise the policy with synthetic budgets.

#include <bloom/render/gpu_device.hpp>

#include <cstdint>

namespace bloom::runtime {

// Mirrors render::GpuAllocationBudgetSource in the runtime vocabulary. `MemoryBudget` is an actual
// live available figure; `DeviceLocalEstimate` is a nominal DEVICE_LOCAL capacity; `Unknown` means
// no budget could be resolved (null/stub/foreign-thread device or an empty heap report).
enum class GpuResidentCapacitySource : std::uint8_t {
    Unknown,
    MemoryBudget,
    DeviceLocalEstimate,
};

// Immutable resolved device capacity for the resident route.
struct GpuResidentCapacity final {
    GpuResidentCapacitySource source = GpuResidentCapacitySource::Unknown;
    // Conservative available allocation budget. Only meaningful when `source != Unknown`; a known
    // live budget may legitimately report zero free bytes, which must NOT be confused with an
    // unresolved device.
    std::uint64_t availableBytes = 0;
    // Summed DEVICE_LOCAL heap size; a diagnostic fact, never an availability claim.
    std::uint64_t deviceLocalBytes = 0;

    // True once the device actually answered (even with a zero budget). The safe 256 MiB fallback
    // is reserved for a genuinely unresolved device, never for a known-zero one.
    [[nodiscard]] bool isKnown() const noexcept {
        return source != GpuResidentCapacitySource::Unknown;
    }

    friend bool operator==(const GpuResidentCapacity&, const GpuResidentCapacity&) = default;
};

// The whole resident route may claim at most this fraction of a resolved available budget; the rest
// stays for the process's other GPU consumers and driver overhead. A `DeviceLocalEstimate` is
// deliberately halved again by this same fraction, so it never behaves like detected free memory.
inline constexpr std::uint64_t kResidentCapacityClaimDivisor = 2;

// Safe total pool when the device budget is genuinely unknown.
inline constexpr std::uint64_t kUnknownResidentPoolBytes = 256ULL * 1024ULL * 1024ULL;
// The 60/20/20 lease/scene/request split of the resident pool.
inline constexpr std::uint64_t kResidentLeaseNumerator = 3;
inline constexpr std::uint64_t kResidentLeaseDenominator = 5;
inline constexpr std::uint64_t kResidentSceneDenominator = 5;
// The resident cache references at most 4/5 of the lease ledger; the remaining 1/5 is headroom for
// leases that are in flight or currently presented but not yet retained by the cache.
inline constexpr std::uint64_t kResidentCacheHeadroomDenominator = 5;
// Nominal resident frame used to turn a byte sublimit into an entry sublimit. A bound, not a claim
// about any composition extent; the byte budget is the authoritative limit.
inline constexpr std::uint64_t kNominalResidentFrameBytes = 8ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kResidentCacheEntryFloor = 8;
inline constexpr std::uint64_t kResidentCacheEntryCeiling = 4096;
inline constexpr std::uint64_t kResidentLeaseEntryCeiling = 4096;

// The artist-visible configured upper bounds for the resident route, already partitioned. The
// owner clamps each against the capacity pool; none is ever raised.
struct GpuResidentConfiguredBudgets final {
    std::uint64_t leaseBytes = 0;
    std::uint64_t sceneCacheBytes = 0;
    std::uint64_t requestBytes = 0;

    friend bool operator==(const GpuResidentConfiguredBudgets&,
                           const GpuResidentConfiguredBudgets&) = default;
};

// The resolved resident admission plan. `poolBytes` is the total device pool the route may use;
// `leaseBytes` includes the cache sublimit plus in-flight/presented headroom, and lease + scene +
// request never exceed the pool.
struct GpuResidentCapacityPlan final {
    // True only for a plan the GPU owner actually resolved and published. A configured host-only
    // plan or a default-constructed plan is NOT resolved, so a consumer can tell "not resolved yet"
    // apart from "resolved to the conservative unknown-device fallback" and from a known-zero
    // device. This is the sole discriminator the UI bootstrap acts on.
    bool resolved = false;
    GpuResidentCapacity capacity;
    std::uint64_t poolBytes = 0;
    std::uint64_t cacheBytes = 0;
    std::uint64_t cacheEntries = 0;
    std::uint64_t leaseBytes = 0;
    std::uint64_t leaseEntries = 0;
    std::uint64_t sceneCacheBytes = 0;
    std::uint64_t requestBytes = 0;

    friend bool operator==(const GpuResidentCapacityPlan&,
                           const GpuResidentCapacityPlan&) = default;
};

// Maps the render device budget into the runtime vocabulary without inventing availability.
[[nodiscard]] GpuResidentCapacity
gpuResidentCapacityFromAllocationBudget(const render::GpuAllocationBudget& allocation) noexcept;

namespace gpu_resident_capacity_detail {

// Overflow-safe proportional share used where the exact 128-bit multiply path is unavailable. It
// computes an estimate of value*numerator/denominator and then clamps into [0, min(value,
// numerator)], so a caller relying on "share <= numerator" can never overcommit even when the host
// long double is only 64-bit. Exposed so the portable path can be tested directly.
[[nodiscard]] std::uint64_t scaledShareClamped(std::uint64_t value, std::uint64_t numerator,
                                               std::uint64_t denominator) noexcept;

} // namespace gpu_resident_capacity_detail

// Pure host-configured partition of an explicit ceiling. Used at construction, before any device
// exists, to set the upper bounds the owner later clamps. A zero ceiling yields an all-zero plan
// (an explicit "no resident caching" choice is respected, never floored upward).
[[nodiscard]] GpuResidentCapacityPlan
gpuResidentCapacityPlanConfigured(std::uint64_t configuredCeilingBytes) noexcept;

// Pure capacity-aware clamp. When the configured route already fits the resolved pool, every
// configured value is preserved exactly; otherwise each ledger is clamped to its pool share so the
// total can never overcommit the device. An unknown budget uses the safe fallback pool.
[[nodiscard]] GpuResidentCapacityPlan
gpuResidentCapacityPlanFor(const GpuResidentConfiguredBudgets& configured,
                           const GpuResidentCapacity& capacity) noexcept;

// One request's DEVICE-stage byte budget: the caller's host/decode pixel-storage ceiling clamped
// down to the owner-resolved GPU request ceiling. A host ceiling larger than the device share is
// clamped, never turned into a GPU refusal; the host ceiling itself is preserved for decoding and
// the CPU fallback. A zero host ceiling or a zero GPU ceiling yields zero, which is the honest
// "no device admission" signal the caller turns into the CPU path.
[[nodiscard]] std::uint64_t gpuResidentRequestBudget(std::uint64_t hostByteAllowance,
                                                     std::uint64_t gpuRequestCeiling) noexcept;

} // namespace bloom::runtime
