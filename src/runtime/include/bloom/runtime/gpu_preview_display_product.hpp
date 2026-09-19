#pragma once

// GPU Neutral display preview product: turns one successful native packed-display result into the
// existing display-only PreviewDisplayOnlyFrame product, off the UI thread, with no Float32 image
// and no fourth PreparedPreviewFrame arm.
//
// This is the product half of the fixed GPU Neutral v1 display operation. The native dispatch
// (device, pipeline, begin/poll/readback) belongs to the runtime-owned preview display service,
// which creates and owns the device/pipeline on its own thread; this only wraps the bytes that
// service already produced. `gpuNeutralDisplayStageIsEligible` is the one authoritative eligibility
// helper shared by that service (before dispatch) and by the finalizer (after readback), so the
// two cannot drift apart.
//
// The finalizer carries exactly the qualification report from the dispatch that produced the
// pixels. There is deliberately no way to build a successful report here: a report exists only from
// qualifyGpuNeutralDisplay().

#include <bloom/render/gpu_neutral_display.hpp>
#include <bloom/runtime/gpu_neutral_display_qualification.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/preview_cpu_stage.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace bloom::runtime {

// The largest display area the fixed GPU operation is admitted for. The report's measured eligible
// interval is always the real authority; this is only the 4K ceiling that interval can never
// exceed. Never a 720p floor: eligibility comes from the measured interval.
inline constexpr std::uint64_t kGpuNeutralDisplayMaxPixels = 3840ULL * 2160ULL;

// One authoritative eligibility check for the fixed GPU Neutral display product. True only when:
// the report is genuinely eligible; its shader/config/processor-cache identities are the pinned
// ones; the CPU stage selected the exact pinned qualified processor; the request identity is
// neutral (identity view adjustment, empty-or-default display/view, lin_rec709_scene working
// space) and matches the process frame's identity and plan; the process image has a valid
// descriptor with dataWindow == displayWindow; the area is inside the measured eligible interval
// and at or below 4K; and the packed pixels plus evaluated geometry fit the stage's byte budget.
// `reason` is set on failure. Used by the service before dispatch and rechecked by the finalizer.
[[nodiscard]] bool
gpuNeutralDisplayStageIsEligible(const PreviewCpuStage& stage,
                                 const GpuNeutralDisplayQualificationReport& report,
                                 std::string& reason) noexcept;

// Wraps a successful native readback into a display-only PreparedPreviewFrame. Returns
// std::nullopt, publishing nothing, on any of: a null or ineligible report; a typed native readback
// failure; a pixel count or budget mismatch; or any eligibility failure above (including a stage
// with a null process frame).
//
// Byte ownership: every rejection up to and including the adopt() call happens before the payload
// is moved, so the caller's readback keeps its bytes. Once adoption has occurred, a later
// allocation failure (metadata or the shared envelope) may consume the readback's bytes -- but the
// ProcessFrame and the whole CPU stage are untouched, so the caller can still fall back to the CPU
// display path with no re-evaluation. On success the packed vector is adopted (moved, pointer
// preserved) and the frame retains the immutable qualification report as GPU provenance; the
// stage's ProcessFrame is not retained.
[[nodiscard]] std::optional<PreparedPreviewFrame>
makeGpuNeutralDisplayPreview(const PreviewCpuStage& stage,
                             std::shared_ptr<const GpuNeutralDisplayQualificationReport> report,
                             render::GpuNeutralDisplayReadback&& readback) noexcept;

} // namespace bloom::runtime
