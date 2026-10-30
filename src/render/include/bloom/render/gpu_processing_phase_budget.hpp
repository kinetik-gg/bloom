#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace bloom::render {
namespace detail {
struct GpuProcessingPhaseBudgetState;
class ProcessingHostBackingToken;
struct PackedHostRetirementState;
void markHostPixelsMaterialized(void*) noexcept;
void retireHostPixelsBeforeFree(void*) noexcept;
} // namespace detail
class PackedHostRetirementOwner;
class NativeImageStorageToken;
class NativeBufferStorageToken;
class ProcessingHostStorageToken;

// Lifecycle identity of the existing neutral budget state, never a pixel/cache content identity.
// A handle retains no phase admission, native owner or device. The incomplete const state cannot
// be mutated through this public surface and cannot be confused after root-address reuse.
using GpuProcessingRootIdentity = std::shared_ptr<const detail::GpuProcessingPhaseBudgetState>;

// Admission of simultaneous processing phases against measured capacity. Each phase claims its
// stated need (fixed output/metadata, 3x the minimum source bytes, device working bytes plus the
// minimum executor bytes); requested source/executor values are ceilings that are not claimed.
// Runtime owns process/device sampling and injects resolved capacity; this class performs no I/O.
struct GpuProcessingPhaseRequest final {
    std::uint64_t fixedHostBytes = 0;
    // Subset of fixedHostBytes reserved for allocator-backed processing metadata.
    std::uint64_t processingHostScratchBytes = 0;
    std::uint64_t requestedSourceBytes = 0;
    std::uint64_t minimumSourceBytes = 0;
    std::uint64_t deviceWorkingBytes = 0;
    std::uint64_t requestedExecutorBytes = 0;
    std::uint64_t minimumExecutorBytes = 0;
    bool sourceFree = false;
    bool native = true;
};

enum class GpuProcessingPhaseAdmission {
    Accepted,
    AwaitingCapacity,
    BudgetRefused,
    InvalidRequest,
    AllocationFailure,
    SerialExhausted,
};
// Viewer: an export or background phase yields to a waiting or active Viewer (ADR 0033 s.9).
enum class GpuProcessingPhaseDomain { None, Host, Device, RequestCount, Viewer };

struct NativePresentationStorageRefusal final {
    GpuProcessingPhaseAdmission status = GpuProcessingPhaseAdmission::InvalidRequest;
    GpuProcessingPhaseDomain domain = GpuProcessingPhaseDomain::None;
    std::uint64_t admissionRevision = 0;
    bool image = false;
};

struct GpuProcessingPhaseSnapshot final {
    std::optional<std::uint64_t> hostCapacityBytes;
    // The most host memory this root could ever be given (the tightest finite memory limit or the
    // physical memory, minus headroom). With it, a host need above the measured free capacity but
    // within the ceiling waits for memory to be released; only a need above it is refused.
    std::optional<std::uint64_t> hostCeilingBytes;
    std::optional<std::uint64_t> deviceCapacityBytes;
    bool hostReady = false;
    bool deviceReady = false;
    std::uint64_t hostReservedBytes = 0;
    // Only allocator-requested payload bytes for explicitly rooted packed RGBA8 storage.
    // Legacy vectors, allocator overhead and native allocations are not included.
    std::uint64_t ownedPackedHostBytes = 0;
    // Exact requested allocation header, including its intrusive retirement node. This is not
    // opaque malloc/standard-library/thread overhead; payload and metadata remain distinct.
    std::uint64_t ownedPackedHostMetadataBytes = 0;
    std::uint64_t totalHostClaimBytes = 0;
    std::size_t ownedPackedHostAllocations = 0;
    // Final process vectors/builders and encoded products use distinct allocation domains.
    std::uint64_t ownedFinalProcessHostBytes = 0;
    std::uint64_t ownedFinalEncodedHostBytes = 0;
    std::uint64_t ownedFinalHostMetadataBytes = 0;
    std::size_t ownedFinalHostAllocations = 0;
    // Exact processing scratch and adopted, measured retained host backing. Metadata applies
    // only to allocator-owned scratch; an adopted backing includes its object in payload bytes.
    std::uint64_t ownedProcessingHostBytes = 0;
    std::uint64_t ownedProcessingHostMetadataBytes = 0;
    std::size_t ownedProcessingHostAllocations = 0;
    std::uint64_t deviceReservedBytes = 0;
    // Covered image suballocations, not VMA committed blocks/driver memory. The nonlocal-host
    // subset overlaps this constraint; it is not an additive physical-memory total.
    std::uint64_t ownedNativeImageBytes = 0;
    std::uint64_t ownedNonlocalNativeHostBytes = 0;
    std::uint64_t totalDeviceClaimBytes = 0;
    std::size_t ownedNativeImages = 0;
    // Covered transient buffer suballocations. Output/upload nonlocal backing replaces planned
    // host debit; processing nonlocal backing adds a fresh headroom claim. This subset overlaps
    // the device constraint and is not additive physical memory.
    std::uint64_t ownedNativeBufferBytes = 0;
    std::uint64_t ownedNonlocalNativeBufferHostBytes = 0;
    std::size_t ownedNativeBuffers = 0;
    // Subsets of the native image/buffer counters above. Presentation has explicit independent
    // root admission and never consumes or pins a frame's processing phase. WSI-owned swapchain
    // images have unknown physical allocation size and are excluded from these owned subsets.
    std::uint64_t ownedPresentationImageBytes = 0;
    std::uint64_t ownedPresentationBufferBytes = 0;
    std::size_t ownedPresentationImages = 0;
    std::size_t ownedPresentationBuffers = 0;
    std::uint64_t peakHostReservedBytes = 0;
    std::uint64_t peakDeviceReservedBytes = 0;
    std::size_t admittedPhases = 0;
    std::size_t unprovenPhases = 0;
    std::size_t requestCapacity = 0;
    std::uint64_t budgetRefusals = 0;
    // Admission-only readiness/release revision; never a process/display/cache identity.
    std::uint64_t admissionRevision = 0;
};

// Exact rooted host allocations that completed construction, and cumulative bytes whose actual
// free has begun. Retirement is recorded before deallocation so a sampler never credits bytes
// already in that operation's free gap. Reservations and new allocations stay uncredited.
struct RootedHostBackingSnapshot final {
    std::uint64_t materializedBytes = 0;
    std::uint64_t retiredMaterializedBytes = 0;
    bool retiredBytesSaturated = false;
};

// Shared aliases refer to one admission. Destruction releases its byte/count reservation once;
// the neutral state outlives the composition root if a native quarantine still owns this lease.
class GpuProcessingPhaseLease final {
  public:
    GpuProcessingPhaseLease(const GpuProcessingPhaseLease&) = delete;
    GpuProcessingPhaseLease& operator=(const GpuProcessingPhaseLease&) = delete;
    ~GpuProcessingPhaseLease();

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] GpuProcessingRootIdentity rootIdentity() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t hostBytes() const noexcept { return hostBytes_; }
    [[nodiscard]] std::uint64_t deviceBytes() const noexcept { return deviceBytes_; }
    // Ceiling for retained decoded sources: the room available at admission. Only the stated
    // need (3 * sourceClaimBytes) is claimed; growth beyond it is reconciled at the handoff.
    [[nodiscard]] std::uint64_t sourceByteLimit() const noexcept { return sourceCeilingBytes_; }
    [[nodiscard]] std::uint64_t sourceClaimBytes() const noexcept { return sourceBytes_; }
    // Logical upload occupancy ceiling; uploads acquire their actual host backing separately.
    [[nodiscard]] std::uint64_t uploadByteLimit() const noexcept { return sourceCeilingBytes_; }
    // Device ceiling: the live room at admission. Only working + minimum executor bytes are
    // claimed up front; native allocations beyond that claim are admitted against live capacity.
    [[nodiscard]] std::uint64_t executorByteLimit() const noexcept { return executorBytes_; }
    [[nodiscard]] std::uint64_t deviceClaimBytes() const noexcept { return deviceClaimBytes_; }
    // After the complete scene has finished CPU preparation, retain its actual decoded backing.
    // Future native upload buffers acquire their actual host bytes at allocation, within the
    // original upload ceiling. Spent decode/conversion scratch must not block native execution.
    // Call once, before publishing the prepared scene to the device thread.
    [[nodiscard]] bool finishSourcePreparation(std::uint64_t retainedSourceBytes) noexcept;
    // Expected watermark after this lease alone releases. Independent changes remain visible.
    [[nodiscard]] std::uint64_t revisionAfterOwnRelease() const noexcept;
    void markUnproven() noexcept;
    [[nodiscard]] bool isUnproven() const noexcept;
    // Only after the owning device has gone idle (every submission that could touch this phase's
    // storage has completed). Lets the phase release its claims normally again.
    void proveRetiredAfterDeviceIdle() noexcept;

  private:
    // Callers hold state_->mutex. A draw first consumes this phase's unallocated device claim;
    // any remainder is a new claim admitted against the live device capacity. A return puts the
    // bytes back into this phase's claim, so the root total is unchanged.
    [[nodiscard]] bool drawDeviceLocked(std::uint64_t bytes) noexcept;
    void releaseLocked() noexcept;
    void returnDeviceLocked(std::uint64_t bytes) noexcept;
    friend class GpuProcessingPhaseBudget;
    friend class PackedHostStorageAuthority;
    friend class NativeImageStorageToken;
    friend class NativeBufferStorageToken;
    friend class ProcessingHostStorageToken;
    friend class detail::ProcessingHostBackingToken;
    explicit GpuProcessingPhaseLease(
        std::shared_ptr<detail::GpuProcessingPhaseBudgetState>) noexcept;
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state_;
    std::uint64_t id_ = 0;
    std::uint64_t admissionRevision_ = 0;
    std::uint64_t hostBytes_ = 0;
    // Separate from immutable granted limits; accesses are guarded by state_->mutex.
    std::uint64_t remainingHostBytes_ = 0;
    // Ordinary owned output uses fixed F only; S/C estimates never become spare output credit.
    std::uint64_t remainingOutputHostBytes_ = 0;
    std::uint64_t remainingProcessingHostScratchBytes_ = 0;
    // Logical occupancy applies even to device-local/BAR upload staging.
    std::uint64_t remainingUploadBytes_ = 0;
    std::uint64_t deviceBytes_ = 0;
    std::uint64_t remainingDeviceBytes_ = 0;
    std::uint64_t sourceBytes_ = 0;
    std::uint64_t sourceCeilingBytes_ = 0;
    std::uint64_t executorBytes_ = 0;
    // Unallocated device bytes this phase holds in totalDeviceClaimBytes, at most its admitted
    // need. Allocations beyond the need are separate live claims, returned to the root on release.
    std::uint64_t deviceClaimBytes_ = 0;
    std::uint64_t deviceNeedBytes_ = 0;
    bool unproven_ = false;
    bool sourcePreparationFinished_ = false;
};

enum class HostPixelStorageDomain { PackedPreview, FinalProcess, FinalEncoded };

class PackedHostStorageAuthority;
// One actual typed host-vector allocation. Its allocator destroys the backing block before this
// token returns provisional bytes to the originating phase or releases published bytes.
class PackedHostStorageToken final {
  public:
    PackedHostStorageToken() = default;
    PackedHostStorageToken(const PackedHostStorageToken&) = delete;
    PackedHostStorageToken& operator=(const PackedHostStorageToken&) = delete;
    PackedHostStorageToken(PackedHostStorageToken&&) noexcept;
    PackedHostStorageToken& operator=(PackedHostStorageToken&&) noexcept;
    ~PackedHostStorageToken();
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool published() const noexcept;

  private:
    friend class PackedHostStorageAuthority;
    friend void detail::markHostPixelsMaterialized(void*) noexcept;
    friend void detail::retireHostPixelsBeforeFree(void*) noexcept;
    // Called only after the typed vector has constructed/touched its complete allocation.
    void markMaterialized() noexcept;
    void retireBeforeFree() noexcept;
    PackedHostStorageToken(std::shared_ptr<PackedHostStorageAuthority>, std::uint64_t,
                           std::uint64_t) noexcept;
    void reset() noexcept;
    std::shared_ptr<PackedHostStorageAuthority> authority_;
    std::uint64_t bytes_ = 0;
    std::uint64_t metadataBytes_ = 0;
    bool materialized_ = false;
};

struct PackedHostStorageSnapshot final {
    std::uint64_t bytes = 0;
    std::uint64_t metadataBytes = 0;
    bool published = false;
};

// A bounded allocator authority under the existing phase root, never a second allowance. Empty
// allocators hold only a weak origin; actual provisional allocations keep it alive until every
// debit is folded back or the complete successful product is promoted once.
class PackedHostStorageAuthority final
    : public std::enable_shared_from_this<PackedHostStorageAuthority> {
  public:
    [[nodiscard]] static std::shared_ptr<PackedHostStorageAuthority>
    create(const std::shared_ptr<GpuProcessingPhaseLease>&, std::uint64_t simultaneousByteLimit,
           HostPixelStorageDomain domain = HostPixelStorageDomain::PackedPreview);
    [[nodiscard]] std::optional<PackedHostStorageToken>
    reserve(std::uint64_t bytes, std::uint64_t metadataBytes = 0) noexcept;
    [[nodiscard]] std::shared_ptr<detail::PackedHostRetirementState>
    retirementState() const noexcept;
    [[nodiscard]] bool promote() noexcept;
    // Complete final product has exactly a process allocation and an optional encoded allocation.
    // Validate both before changing either; detach origins outside the common neutral lock.
    [[nodiscard]] static bool
    promoteFinalProduct(const std::shared_ptr<PackedHostStorageAuthority>& process,
                        const std::shared_ptr<PackedHostStorageAuthority>& encoded = {}) noexcept;
    [[nodiscard]] PackedHostStorageSnapshot snapshot() const noexcept;
    [[nodiscard]] GpuProcessingRootIdentity rootIdentity() const noexcept { return state_; }

  private:
    friend class PackedHostStorageToken;
    PackedHostStorageAuthority(const std::shared_ptr<GpuProcessingPhaseLease>&, std::uint64_t,
                               HostPixelStorageDomain) noexcept;
    void release(std::uint64_t bytes, std::uint64_t metadataBytes, bool materialized) noexcept;
    void materialize(std::uint64_t bytes, std::uint64_t metadataBytes) noexcept;
    void retireMaterialized(std::uint64_t bytes, std::uint64_t metadataBytes) noexcept;
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state_;
    std::weak_ptr<GpuProcessingPhaseLease> origin_;
    std::shared_ptr<GpuProcessingPhaseLease> retainedOrigin_;
    HostPixelStorageDomain domain_ = HostPixelStorageDomain::PackedPreview;
    std::uint64_t originId_ = 0;
    std::uint64_t limit_ = 0;
    std::uint64_t bytes_ = 0;
    std::uint64_t metadataBytes_ = 0;
    bool published_ = false;
};

struct GpuProcessingPhaseGrant final {
    GpuProcessingPhaseAdmission status = GpuProcessingPhaseAdmission::InvalidRequest;
    GpuProcessingPhaseDomain domain = GpuProcessingPhaseDomain::None;
    std::shared_ptr<GpuProcessingPhaseLease> lease;
    std::uint64_t admissionRevision = 0;
};

class GpuProcessingPhaseBudget final {
  public:
    explicit GpuProcessingPhaseBudget(
        std::size_t requestCapacity = 32,
        const std::shared_ptr<PackedHostRetirementOwner>& retirementOwner = {});
    // Unknown/stale delivery closes new admission but does not erase the last resolved constraint
    // or free live leases. A reliable measured zero is a ready, zero capacity (BudgetRefused).
    // Runtime supplies the delivered sample deadline. Explicit legacy/manual quotas default to
    // no expiry; new supplemental host allocation cannot use an expired delivered sample.
    void setHostCapacity(std::optional<std::uint64_t> bytes, bool ready,
                         std::chrono::steady_clock::time_point validUntil =
                             std::chrono::steady_clock::time_point::max()) noexcept;
    // Apply one measured sample and its exact initial credit atomically with retirements that
    // occurred after the sampling bracket. Subsequent retirement revokes the remaining credit.
    void setSampledHostCapacity(std::optional<std::uint64_t> bytes, bool ready,
                                std::chrono::steady_clock::time_point validUntil,
                                std::uint64_t creditBytes,
                                std::uint64_t retiredBytesAtSample) noexcept;
    // Duplicate/stale delivery may tighten an existing capacity only. Keep its current readiness
    // and expiry for a duplicate; a stale delivery closes readiness at the supplied deadline.
    void tightenHostCapacity(std::optional<std::uint64_t> upperBound, bool closeReadiness,
                             std::chrono::steady_clock::time_point validUntil) noexcept;
    void setDeviceCapacity(std::optional<std::uint64_t> bytes, bool ready) noexcept;
    // The host ceiling described on GpuProcessingPhaseSnapshot::hostCeilingBytes.
    void setHostCeiling(std::optional<std::uint64_t> bytes) noexcept;
    // Moves a known host capacity by a preference change (headroom), keeping readiness/expiry.
    void shiftHostCapacity(std::uint64_t add, std::uint64_t subtract) noexcept;
    // Blocks until the admission revision differs from `seen` (a release, a capacity change) or
    // the deadline passes. Returns true when it changed. Releases from storage tokens are seen
    // within one short slice even when they do not notify.
    [[nodiscard]] bool waitForRevisionChange(std::uint64_t seen,
                                             std::chrono::steady_clock::time_point deadline) const;
    [[nodiscard]] GpuProcessingPhaseSnapshot snapshot() const noexcept;
    [[nodiscard]] RootedHostBackingSnapshot rootedHostBackingSnapshot() const noexcept;
    [[nodiscard]] GpuProcessingRootIdentity rootIdentity() const noexcept { return state_; }
    [[nodiscard]] GpuProcessingPhaseGrant reserve(const GpuProcessingPhaseRequest&) noexcept;

  private:
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state_;
};
} // namespace bloom::render
