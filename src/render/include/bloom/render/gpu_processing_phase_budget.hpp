#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace bloom::render {
namespace detail {
struct GpuProcessingPhaseBudgetState;
struct PackedHostRetirementState;
} // namespace detail
class PackedHostRetirementOwner;
class NativeImageStorageToken;
class NativeBufferStorageToken;

// Lifecycle identity of the existing neutral budget state, never a pixel/cache content identity.
// A handle retains no phase admission, native owner or device. The incomplete const state cannot
// be mutated through this public surface and cannot be confused after root-address reuse.
using GpuProcessingRootIdentity = std::shared_ptr<const detail::GpuProcessingPhaseBudgetState>;

// A conservative quota for simultaneous processing phases, not physical allocation accounting.
// Runtime owns process/device sampling and injects resolved capacity; this class performs no I/O.
struct GpuProcessingPhaseRequest final {
    std::uint64_t fixedHostBytes = 0;
    std::uint64_t requestedSourceBytes = 0;
    std::uint64_t minimumSourceBytes = 0;
    std::uint64_t deviceWorkingBytes = 0;
    std::uint64_t requestedExecutorBytes = 0;
    std::uint64_t minimumExecutorBytes = 0;
    bool sourceFree = false;
    bool native = true;
    // Unknown sources get a bounded per-request share, independently of output extent. Known
    // minima may exceed the share; the paired remaining capacity still limits the whole phase.
    std::uint32_t capacityShareDenominator = 4;
};

enum class GpuProcessingPhaseAdmission {
    Accepted,
    AwaitingCapacity,
    BudgetRefused,
    InvalidRequest,
    AllocationFailure,
    SerialExhausted,
};
enum class GpuProcessingPhaseDomain { None, Host, Device, RequestCount };

struct GpuProcessingPhaseSnapshot final {
    std::optional<std::uint64_t> hostCapacityBytes;
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
    std::uint64_t deviceReservedBytes = 0;
    // Covered image suballocations, not VMA committed blocks/driver memory. The nonlocal-host
    // subset overlaps this constraint; it is not an additive physical-memory total.
    std::uint64_t ownedNativeImageBytes = 0;
    std::uint64_t ownedNonlocalNativeHostBytes = 0;
    std::uint64_t totalDeviceClaimBytes = 0;
    std::size_t ownedNativeImages = 0;
    // Covered transient buffer suballocations. Their nonlocal host subset replaces an existing
    // host phase debit, unlike the additive nonlocal image subset above.
    std::uint64_t ownedNativeBufferBytes = 0;
    std::uint64_t ownedNonlocalNativeBufferHostBytes = 0;
    std::size_t ownedNativeBuffers = 0;
    std::uint64_t peakHostReservedBytes = 0;
    std::uint64_t peakDeviceReservedBytes = 0;
    std::size_t admittedPhases = 0;
    std::size_t unprovenPhases = 0;
    std::size_t requestCapacity = 0;
    std::uint64_t budgetRefusals = 0;
    // Admission-only readiness/release revision; never a process/display/cache identity.
    std::uint64_t admissionRevision = 0;
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
    [[nodiscard]] std::uint64_t sourceByteLimit() const noexcept { return sourceBytes_; }
    [[nodiscard]] std::uint64_t executorByteLimit() const noexcept { return executorBytes_; }
    // Expected watermark after this lease alone releases. Independent changes remain visible.
    [[nodiscard]] std::uint64_t revisionAfterOwnRelease() const noexcept;
    void markUnproven() noexcept;
    [[nodiscard]] bool isUnproven() const noexcept;

  private:
    friend class GpuProcessingPhaseBudget;
    friend class PackedHostStorageAuthority;
    friend class NativeImageStorageToken;
    friend class NativeBufferStorageToken;
    explicit GpuProcessingPhaseLease(
        std::shared_ptr<detail::GpuProcessingPhaseBudgetState>) noexcept;
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state_;
    std::uint64_t id_ = 0;
    std::uint64_t admissionRevision_ = 0;
    std::uint64_t hostBytes_ = 0;
    // Separate from immutable granted limits; accesses are guarded by state_->mutex.
    std::uint64_t remainingHostBytes_ = 0;
    std::uint64_t deviceBytes_ = 0;
    std::uint64_t remainingDeviceBytes_ = 0;
    std::uint64_t sourceBytes_ = 0;
    std::uint64_t executorBytes_ = 0;
    bool unproven_ = false;
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
    PackedHostStorageToken(std::shared_ptr<PackedHostStorageAuthority>, std::uint64_t,
                           std::uint64_t) noexcept;
    void reset() noexcept;
    std::shared_ptr<PackedHostStorageAuthority> authority_;
    std::uint64_t bytes_ = 0;
    std::uint64_t metadataBytes_ = 0;
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

  private:
    friend class PackedHostStorageToken;
    PackedHostStorageAuthority(const std::shared_ptr<GpuProcessingPhaseLease>&, std::uint64_t,
                               HostPixelStorageDomain) noexcept;
    void release(std::uint64_t bytes, std::uint64_t metadataBytes) noexcept;
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
    void setDeviceCapacity(std::optional<std::uint64_t> bytes, bool ready) noexcept;
    [[nodiscard]] GpuProcessingPhaseSnapshot snapshot() const noexcept;
    [[nodiscard]] GpuProcessingPhaseGrant reserve(const GpuProcessingPhaseRequest&) noexcept;

  private:
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state_;
};
} // namespace bloom::render
