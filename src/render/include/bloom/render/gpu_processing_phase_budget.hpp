#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace bloom::render {
namespace detail {
struct GpuProcessingPhaseBudgetState;
}

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
    std::uint64_t deviceReservedBytes = 0;
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
    [[nodiscard]] std::uint64_t hostBytes() const noexcept { return hostBytes_; }
    [[nodiscard]] std::uint64_t deviceBytes() const noexcept { return deviceBytes_; }
    [[nodiscard]] std::uint64_t sourceByteLimit() const noexcept { return sourceBytes_; }
    [[nodiscard]] std::uint64_t executorByteLimit() const noexcept { return executorBytes_; }
    // Expected watermark after this lease alone releases. Independent changes remain visible.
    [[nodiscard]] std::uint64_t revisionAfterOwnRelease() const noexcept;
    void markUnproven() noexcept;

  private:
    friend class GpuProcessingPhaseBudget;
    explicit GpuProcessingPhaseLease(
        std::shared_ptr<detail::GpuProcessingPhaseBudgetState>) noexcept;
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state_;
    std::uint64_t id_ = 0;
    std::uint64_t admissionRevision_ = 0;
    std::uint64_t hostBytes_ = 0;
    std::uint64_t deviceBytes_ = 0;
    std::uint64_t sourceBytes_ = 0;
    std::uint64_t executorBytes_ = 0;
    bool unproven_ = false;
};

struct GpuProcessingPhaseGrant final {
    GpuProcessingPhaseAdmission status = GpuProcessingPhaseAdmission::InvalidRequest;
    GpuProcessingPhaseDomain domain = GpuProcessingPhaseDomain::None;
    std::shared_ptr<GpuProcessingPhaseLease> lease;
    std::uint64_t admissionRevision = 0;
};

class GpuProcessingPhaseBudget final {
  public:
    explicit GpuProcessingPhaseBudget(std::size_t requestCapacity = 32);
    // Unknown/stale delivery closes new admission but does not erase the last resolved constraint
    // or free live leases. A reliable measured zero is a ready, zero capacity (BudgetRefused).
    void setHostCapacity(std::optional<std::uint64_t> bytes, bool ready) noexcept;
    void setDeviceCapacity(std::optional<std::uint64_t> bytes, bool ready) noexcept;
    [[nodiscard]] GpuProcessingPhaseSnapshot snapshot() const noexcept;
    [[nodiscard]] GpuProcessingPhaseGrant reserve(const GpuProcessingPhaseRequest&) noexcept;

  private:
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state_;
};
} // namespace bloom::render
