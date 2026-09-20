#pragma once

// Private owner-thread helper for GpuProcessFrameEvaluator's ONE final combined readback and the
// immutable process-image publication that follows it. This is internal to the evaluator: it
// borrows the already-created output-colour stage and the already-taken resident process image and
// never touches the request queue, bootstrap, or lifecycle. It exists so gpu_process_frame.cpp
// stays within the owning-file size budget without changing any evaluator behavior.

#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

[[nodiscard]] GpuProcessFrameDiagnosticCode
mapOutputColorCode(GpuOutputColorDiagnosticCode code) noexcept;

// The complete result of the combined readback and process-image publication. `status` is
// Evaluated only when `processImage` is non-null; every other status carries a typed diagnostic
// and no frame.
struct FinalReadbackOutcome final {
    std::shared_ptr<const render::Rgba32fImage> processImage;
    GpuOutputColorArm encodedArm = GpuOutputColorArm::None;
    std::vector<render::Rgba32f> encodedEffectRgba32f;
    std::vector<render::Rgba8> encodedDisplayRgba8;
    core::Sha256Digest outputCommandIdentity{};
    GpuOutputColorCounters outputColorCounters;
    std::uint64_t readbacks = 0;
    GpuProcessFrameStatus status = GpuProcessFrameStatus::Failed;
    GpuProcessFrameDiagnosticCode diagnosticCode = GpuProcessFrameDiagnosticCode::None;
    std::string diagnosticMessage;
};

// Owner-thread only. Runs the one combined final readback against an admitted stage, bounded by
// `nativeDeadline` and observing cancellation/stop, then publishes an immutable Rgba32fImage from
// the unchanged process payload. The encoded output is kept distinct from the process payload.
[[nodiscard]] FinalReadbackOutcome runFinalCombinedReadback(
    GpuOutputColorStage& stage, const render::Rgba32fImageDescriptor& descriptor,
    std::shared_ptr<const PreparedGpuOcioCommand> outputCommand,
    std::shared_ptr<const render::GpuImage> image, std::uint64_t readbackByteBudget,
    std::chrono::milliseconds nativeDeadline, const CancellationToken& cancellation,
    const std::atomic_bool& stopRequested);

} // namespace bloom::runtime::detail
