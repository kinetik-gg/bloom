#pragma once

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/task_budget_view.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

namespace bloom::runtime {
enum class GpuProcessingPhaseOutput { PackedPreview, FloatFinal };
struct GpuProcessingPhaseEnvelope final {
    GpuProcessingPhaseOutput output = GpuProcessingPhaseOutput::PackedPreview;
    std::uint64_t pixels = 0;
    std::uint64_t encodedBytes = 0;
    std::uint64_t metadataBytes = 0;
    // Explicit retained vector/text coverage cap, plus one temporary overlap in fixed host quota.
    std::uint64_t retainedCoverageByteLimit = 0;
    std::uint64_t requestedSourceBytes = 0;
    std::uint64_t minimumSourceBytes = 0;
    // Omitted rooted-view requests derive this ceiling per attempt from the selected request.
    // Explicit zero/finite values retain their exact meaning; no-view omission stays legacy zero.
    std::optional<std::uint64_t> requestedExecutorBytes = {};
    std::uint64_t minimumExecutorBytes = 0;
    std::uint64_t deviceWorkingBytes = 0;
    bool sourceFree = false;
    bool native = true;
};
// Exact core-owned output overlap plus an explicitly reserved source/upload/scratch allowance.
// These conservative phase estimates do not meter opaque codec/driver allocations or retained
// products after publication. Source-free classification must use reachable compiled operations.
[[nodiscard]] std::optional<render::GpuProcessingPhaseRequest>
processingPhaseRequest(const GpuProcessingPhaseEnvelope&) noexcept;

struct GpuProcessingPhasePolicySnapshot final {
    render::GpuProcessingPhaseSnapshot reservations;
    ProcessMemorySource processSource = ProcessMemorySource::Unavailable;
    ProcessMemoryStatus processStatus = ProcessMemoryStatus::Pending;
    std::uint64_t sampleSerial = 0;
    std::size_t deviceReports = 0;
    bool usesNominalDeviceEstimate = false;
};

// One explicitly injected composition-root owner, shared by preview and final providers. Sampling
// is external and worker-only. Native owner threads key reports by GpuDevice::ownershipEpoch(),
// never capability generation1; a common minimum over all reports is an intentionally conservative
// all-device quota, not GPU identity.
class GpuProcessingPhasePolicy final {
  public:
    using Clock = std::chrono::steady_clock;
    explicit GpuProcessingPhasePolicy(
        std::size_t requestCapacity = 32,
        const std::shared_ptr<render::PackedHostRetirementOwner>& retirementOwner = {},
        std::shared_ptr<TaskBudgetView> budgetView = {});
    void applyHostSample(const MemoryBudgetState&, std::size_t physicalBytes,
                         Clock::time_point now = Clock::now()) noexcept;
    // Explicit root delivery: allocation reflects the owner's configured cache split/overrides.
    // Publish the numeric Tasks view only after host-policy locks have been released.
    void applyHostSample(const MemoryBudgetState&, std::size_t physicalBytes,
                         const MemoryBudgetAllocation&,
                         Clock::time_point now = Clock::now()) noexcept;
    [[nodiscard]] const std::shared_ptr<TaskBudgetView>& budgetView() const noexcept {
        return budgetView_;
    }
    // Re-report the same ownership epoch as live free capacity changes. Single-owner live reports
    // reconcile exact root-owned local native backing with the root's existing claim ledger;
    // unallocated phase reservations and multi-device ownership receive no such credit.
    [[nodiscard]] bool reportDevice(std::uint64_t ownershipEpoch,
                                    render::GpuAllocationBudget) noexcept;
    // A proven owner teardown removes its report. Unproven generations remain bounded/charged;
    // this is not a retirement oracle and callers must supply an exact native proof.
    [[nodiscard]] bool retireDeviceReport(std::uint64_t ownershipEpoch,
                                          bool retirementProven) noexcept;
    [[nodiscard]] render::GpuProcessingPhaseGrant
    reserve(const GpuProcessingPhaseEnvelope&, Clock::time_point now = Clock::now()) noexcept;
    [[nodiscard]] GpuProcessingPhasePolicySnapshot snapshot() const noexcept;
    [[nodiscard]] render::GpuProcessingRootIdentity rootIdentity() const noexcept {
        return budget_.rootIdentity();
    }

  private:
    struct DeviceReport final {
        std::uint64_t ownershipEpoch = 0;
        render::GpuAllocationBudget budget;
    };
    mutable std::mutex mutex_;
    render::GpuProcessingPhaseBudget budget_;
    const std::shared_ptr<TaskBudgetView> budgetView_;
    ProcessMemorySource processSource_ = ProcessMemorySource::Unavailable;
    ProcessMemoryStatus processStatus_ = ProcessMemoryStatus::Pending;
    std::uint64_t sampleSerial_ = 0;
    std::optional<std::uint64_t> finiteProcessCeiling_;
    ProcessMemoryConstraintHistory processConstraintHistory_;
    std::optional<Clock::time_point> hostSampleAt_;
    std::array<DeviceReport, 32> devices_{};
    std::size_t deviceCount_ = 0;
    void applyDeviceReportsLocked() noexcept;
};

// Host-only CPU reference evaluation uses the same root/request-count quota. The granted S
// bounds its existing pixel-storage ceiling; 3*S reserves preparation/temporary overlap, not
// opaque decoder internals. The lease must outlive downstream analysis/display adoption.
[[nodiscard]] render::GpuProcessingPhaseGrant
reserveHostEvaluationPhase(GpuProcessingPhasePolicy&, std::uint64_t pixels,
                           std::uint64_t requestedPixelBytes, GpuProcessingPhaseOutput output,
                           bool encodedOutput = false) noexcept;
} // namespace bloom::runtime
