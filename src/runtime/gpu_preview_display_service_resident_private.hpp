#pragma once

// Resident-route generation of GpuPreviewDisplayService.
//
// This is the only place the service creates, qualifies, drives, or retires the resident display
// route: one GpuSceneCache, one GpuSceneExecutor, and one GpuResidentDisplay, all on the service's
// own device and own service thread, with no second service, thread, device, or scheduler lease.
//
// The route is genuinely qualified at startup on the owner thread by qualifyResidentPreview(); the
// report is published independently of the older packed readback qualification and is the only
// authority for resident eligibility (never the packed report, packed bandwidth, a fake
// qualification, or the raw document revision). A prepared GPU scene is executed by the existing
// GpuSceneExecutor, its output is displayed by the existing GpuResidentDisplay with zero full-frame
// readback, and the product factory publishes the resulting opaque GpuResidentFrameLease into the
// service's own presentation registry.
//
// Any refusal -- unsupported compile, GPU-subset refusal, non-neutral request, unavailable
// presentation generation, over-budget lease publication, or a resident native failure -- takes the
// FULL original CPU path through the CPU stage function and display fallback. Live presentation
// leases and pins are never invalidated by a budget refusal, and no permanent GPU poison is latched
// for normal capacity pressure.

#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/runtime/gpu_resident_preview_qualification.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/preview_gpu_scene_stage.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace bloom::runtime::detail {

struct PreviewDisplayServiceCore;
struct PreviewDisplayStageRecord;

// Owner-thread resident native sub-state for one admitted stage.
enum class ResidentNativePhase : std::uint8_t {
    None,
    Executor,
    Display,
    // A failure/deadline/cancellation/lost-generation was observed while a native submission may
    // still be unretired. The stage, its GpuTaskCompletion, its source snapshot/identity/overrides
    // and the native pins are ALL retained until retirement is proven (bounded poll) or the owned
    // pipelines are destroyed on the owner thread (their destructor performs a bounded
    // drain/quarantine). The parent completion is never consumed before then.
    Retiring,
    Done,
};

// Bounded owner-thread retirement pumps before the resident pipelines are destroyed (and their own
// bounded drain/quarantine runs). Bounded so a fence that never signals cannot busy-loop; the
// service loop already wakes at the short active interval while a stage is Retiring.
inline constexpr std::size_t kResidentRetirementPumpBudget = 512;

// Owner thread. Creates the resident scene cache, executor, and resident display, runs the genuine
// qualifyResidentPreview(), and publishes the immutable report + status. Returns true only when a
// genuinely eligible report exists. Never throws (failure is published as Unavailable).
[[nodiscard]] bool
createAndQualifyResidentRoute(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread. Refreshes the UI-readable resident report/detail snapshot.
void publishResidentStatus(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread. Retires the resident executor, display, and scene cache in dependency order. Any
// unproven native submission is drained by the owned pipeline (the pipeline retains its pins until
// it proves retirement or is destroyed), exactly like the packed path.
void retireResidentRoute(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread. Pre-dispatch eligibility for one prepared GPU scene. Checks the genuine resident
// report against the exact device + selected processor, the usable presentation generation, the
// neutral request, and the trusted output descriptor/area/budget. The product factory re-checks the
// actual native image; this only avoids a pointless native begin. `reason` is set on refusal.
[[nodiscard]] bool residentStageIsEligible(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                           const PreviewDisplayStageRecord& stage,
                                           std::string& reason) noexcept;

// Owner thread native stepping primitives. Each returns false / a terminal poll result on failure
// and sets `reason`; nothing is published on failure.
[[nodiscard]] bool residentExecutorBegin(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                         const PreviewDisplayStageRecord& stage,
                                         std::string& reason) noexcept;
[[nodiscard]] GpuSceneExecutorPollResult
residentExecutorPoll(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;
// Takes the executor output and begins the resident display with it.
[[nodiscard]] bool residentDisplayBegin(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                        const PreviewDisplayStageRecord& stage,
                                        std::string& reason) noexcept;
[[nodiscard]] render::GpuResidentDisplayPollResult
residentDisplayPoll(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;
// Takes the resident display output and publishes the product. `std::nullopt` on any refusal
// (including registry budget pressure), with `reason` set. No full-frame readback is ever
// performed.
[[nodiscard]] std::optional<PreparedPreviewFrame>
residentFinishFrame(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                    const PreviewDisplayStageRecord& stage, std::string& reason) noexcept;

// Owner thread cancellation/terminal probes.
void residentCancelNative(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;
[[nodiscard]] bool
residentOwnerDrainRequired(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;
// True while the resident display owns a submitted job whose fence has not been proven retired.
// This is the authoritative retirement test (NOT state()==Pending, which a logical Failure can
// leave behind with an unretired submission).
[[nodiscard]] bool residentDisplayHasUnretiredSubmission(
    const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;
// True while the resident executor's own native-in-flight flag is set or any owned native pipeline
// still holds an unretired submission. Broader than ownerDrainRequired().
[[nodiscard]] bool residentExecutorHasUnretiredSubmission(
    const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;
[[nodiscard]] bool
residentDeviceLost(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread. Copies the executor's cumulative cache/counter observations into the service's
// cached counters so status() reflects concrete end-to-end measurements.
void noteResidentExecutorCounters(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

} // namespace bloom::runtime::detail
