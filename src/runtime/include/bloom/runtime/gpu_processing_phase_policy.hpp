#pragma once

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/memory_headroom.hpp>
#include <bloom/runtime/task_budget_view.hpp>

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime {
enum class GpuProcessingPhaseOutput { PackedPreview, FloatFinal };
// ADR 0033 section 9 classes. Viewer: the current Viewer frame, scrub, playback and RAM preview.
// Export: final render (still, sequence, movie, headless). Background: thumbnails, audio, other
// speculative work.
enum class GpuAdmissionClass : std::uint8_t { Viewer, Export, Background };
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
    GpuAdmissionClass admissionClass = GpuAdmissionClass::Viewer;
};
// Exact core-owned output overlap plus an explicitly reserved source/upload/scratch allowance.
// These conservative phase estimates do not meter opaque codec/driver allocations or retained
// products after publication. Source-free classification must use reachable compiled operations.
[[nodiscard]] std::optional<render::GpuProcessingPhaseRequest>
processingPhaseRequest(const GpuProcessingPhaseEnvelope&) noexcept;

// Measured capacity minus the headroom preferences, and what admitted work holds of it now.
struct UsableCapacity final {
    bool hostReady = false;
    bool deviceReady = false;
    // Available RAM within real OS/process limits minus the RAM headroom.
    std::uint64_t hostUsableBytes = 0;
    // The driver's live device budget (Bloom's own backing credited back) minus the VRAM headroom.
    std::uint64_t deviceUsableBytes = 0;
    std::uint64_t hostClaimBytes = 0;
    std::uint64_t deviceClaimBytes = 0;
    std::uint64_t hostFreeBytes = 0;
    std::uint64_t deviceFreeBytes = 0;
    // Evictable cache retention inside hostClaimBytes (retained preview frames).
    std::uint64_t hostRetentionBytes = 0;
    std::size_t admittedPhases = 0;
    std::uint64_t admissionRevision = 0;
};

// Readable record of one admission decision: status and domain names plus the counters.
[[nodiscard]] std::string describeAdmission(const render::GpuProcessingPhaseGrant&,
                                            const UsableCapacity&);

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
    // Worker-only OS sample bracketed by this root's exact completed host allocations. The
    // resulting credit is consumed only by this policy on a matching fresh delivery.
    using HostMemorySampler = MemoryBudgetSample (*)(const CancellationToken&) noexcept;
    [[nodiscard]] MemoryBudgetSample
    sampleHostMemory(const CancellationToken& = {},
                     HostMemorySampler sampler = memoryBudgetSample) noexcept;
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
    // The RAM/VRAM headroom preferences: the only margins kept free of Bloom's work and caches.
    // Applies at once to the current capacity and wakes waiters. Composition roots deliver the
    // preference (MemoryHeadroom{} when none is saved); a root starts with no margin.
    void setHeadroom(MemoryHeadroom headroom) noexcept;
    [[nodiscard]] MemoryHeadroom headroom() const noexcept;
    [[nodiscard]] UsableCapacity usableCapacity() const noexcept;
    // How many more frames of this per-frame need fit the free capacity right now.
    [[nodiscard]] std::size_t framesThatFit(std::uint64_t hostBytesPerFrame,
                                            std::uint64_t deviceBytesPerFrame) const noexcept;
    // Waits for a release or capacity change after `seenRevision`, cancellation or `maxWait`.
    // True when admission state changed. Never call on the UI thread.
    [[nodiscard]] bool waitForAdmissionChange(std::uint64_t seenRevision,
                                              const CancellationToken& cancellation,
                                              std::chrono::milliseconds maxWait) const;
    // Evicts reusable retention (caches) when a request waits for capacity. Called outside every
    // admission lock with the waiting domain and the bytes it needs; returns bytes released.
    // Strict Viewer priority over the shared root (ADR 0033 section 9), the GpuAdmissionArbiter
    // role: while a Viewer request waits for capacity, Export and Background requests wait with
    // domain Viewer; while the Viewer is active (a Viewer frame admitted within the activity
    // window), an Export/Background grant must leave the last Viewer need free. An idle Viewer
    // holds nothing: exports run at full speed. A Viewer request never waits for an export rule.
    static constexpr std::chrono::milliseconds kViewerActivityWindow{250};
    struct ViewerPriority final {
        bool waiting = false;
        bool active = false;
        std::uint64_t reserveHostBytes = 0;
        std::uint64_t reserveDeviceBytes = 0;
    };
    [[nodiscard]] ViewerPriority
    viewerPriority(Clock::time_point now = Clock::now()) const noexcept;
    // A reclaimer receives the waiting domain and the shortfall (the need minus what that domain
    // has free now). Composition roots register one that evicts every reusable ledger pool, so
    // caches are never a reason for work to wait.
    using RetentionReclaimer =
        std::function<std::uint64_t(render::GpuProcessingPhaseDomain, std::uint64_t)>;
    void addRetentionReclaimer(RetentionReclaimer reclaimer);
    // The most recent request that had to wait for capacity: its domain, whole need and shortfall
    // (export progress reports "waiting for memory: N needed").
    struct AwaitingNeed final {
        render::GpuProcessingPhaseDomain domain = render::GpuProcessingPhaseDomain::Host;
        std::uint64_t bytes = 0;
        std::uint64_t shortfallBytes = 0;
        Clock::time_point at{};
    };
    [[nodiscard]] std::optional<AwaitingNeed> lastAwaitingNeed() const noexcept;
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
    // The tightest finite process limit or physical memory, before headroom.
    std::optional<std::uint64_t> hostCeiling_;
    ProcessMemoryConstraintHistory processConstraintHistory_;
    std::optional<Clock::time_point> hostSampleAt_;
    struct HostSampleBackingCredit final {
        Clock::time_point sampledAt{};
        std::uint64_t bytes = 0;
        std::optional<std::uint64_t> finiteHeadroom;
        std::uint64_t sampleSerial = 0;
        std::uint64_t retiredBytesAtSample = 0;
    };
    std::optional<HostSampleBackingCredit> pendingHostBackingCredit_;
    std::optional<HostSampleBackingCredit> appliedHostBackingCredit_;
    // No margin until the composition root delivers the preference (default 2 GiB each).
    MemoryHeadroom headroom_{.ramBytes = 0, .vramBytes = 0};
    std::optional<Clock::time_point> viewerWaitingAt_;
    std::optional<Clock::time_point> viewerAdmittedAt_;
    std::uint64_t viewerNeedHostBytes_ = 0;
    std::uint64_t viewerNeedDeviceBytes_ = 0;
    std::vector<RetentionReclaimer> reclaimers_;
    mutable std::mutex reclaimerMutex_;
    std::optional<AwaitingNeed> lastAwaitingNeed_;
    std::chrono::steady_clock::time_point lastAdmissionLog_{};
    void logWaitOrRefusal(const render::GpuProcessingPhaseGrant&) noexcept;
    std::array<DeviceReport, 32> devices_{};
    std::size_t deviceCount_ = 0;
    void applyDeviceReportsLocked() noexcept;
};

// RAM-fill and playback read-ahead width ceiling: enough frames to keep CPU preparation, the
// device (one scene at a time) and the readback overlapped. Frames in flight are bounded by that
// concurrency, never by how much memory happens to be free.
inline constexpr std::size_t kMaximumPreviewFramesInFlight = 8;
// Preview frames (Viewer, RAM fill, playback) of `framePixels` that fit the free capacity now: a
// packed output, one float source and preparation overlap on the host, the preview working set
// plus one float frame on the device. Capped at one per hardware thread (decode and scene
// preparation run on CPU workers), at kMaximumPreviewFramesInFlight, and at what fits a quarter of
// the free capacity (at least one frame when one fits); zero when even one frame does not fit.
[[nodiscard]] std::size_t previewFramesThatFit(const GpuProcessingPhasePolicy&,
                                               std::uint64_t framePixels,
                                               std::uint64_t deviceWorkingBytes) noexcept;

// Host-only CPU reference evaluation uses the same root/request-count quota. The granted S
// bounds its existing pixel-storage ceiling; 3*S reserves preparation/temporary overlap, not
// opaque decoder internals. The lease must outlive downstream analysis/display adoption.
[[nodiscard]] render::GpuProcessingPhaseGrant
reserveHostEvaluationPhase(GpuProcessingPhasePolicy&, std::uint64_t pixels,
                           std::uint64_t requestedPixelBytes, GpuProcessingPhaseOutput output,
                           bool encodedOutput = false,
                           GpuAdmissionClass admissionClass = GpuAdmissionClass::Viewer,
                           std::uint64_t minimumPixelBytes = 0) noexcept;
} // namespace bloom::runtime
