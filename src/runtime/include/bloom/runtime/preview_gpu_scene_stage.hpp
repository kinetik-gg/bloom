#pragma once

#include <bloom/runtime/gpu_ocio_display_arm.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// The pure, Qt-free seam between the composition preview's CPU half and the GPU scene preparation
// half. Nothing here knows about Vulkan, a device, a service thread, or the UI. A
// PreviewGpuSceneStage is the immutable result of compiling, preparing the CPU-side GPU scene, and
// SELECTING a CPU display processor for one request: it retains the immutable PreparedGpuScene and
// the selected color::PreparedCpuDisplayProcessorHandle (null means the reference/unqualified
// startup path) so a future executor/display product can consume the same request without ever
// recompiling the graph.
//
// This is deliberately the GPU-scene analogue of PreviewCpuStage / PreviewCpuStageFunction
// (preview_cpu_stage.hpp). It differs in exactly one respect: where the CPU stage evaluates the
// compiled plan into a full ProcessFrame, this stage runs the stateless CpuGpuSceneBuilder's
// `build()` -- the exact existing API; the task brief's "prepare" names this step, there is no
// separate method -- to produce an immutable PreparedGpuScene of resolved operands and geometry.
// It NEVER allocates a full CPU scene image for a supported solid graph, never evaluates the graph
// itself, and never fabricates an empty frame.
//
// The factories that build these functions live in bloom::ui
// (bloom/ui/composition_preview_gpu_scene_stage.hpp) because the compiled-plan cache is a UI-owned
// handle, exactly as makeCompositionPreviewCpuStage is. The builder is CONSTRUCTED by the caller
// and injected; a media-capable builder substitutes its decoder/context through that one object, so
// no alternative decoder is ever invented here.
namespace bloom::runtime {

enum class PreviewGpuSceneStageStatus : std::uint8_t {
    // A plan was compiled and an immutable scene prepared; `stage` carries it.
    Prepared,
    // Compilation rejected the composition semantically; no scene exists. This is the same terminal
    // outcome as PreviewCpuStageStatus::Unsupported and is NOT a GPU-subset fallback.
    Unsupported,
    // The composition compiled, but a reachable operation is outside the prepared GPU subset (a
    // text/effect/rotation/scale/media/non-Normal/ROI/non-neutral graph). The caller takes the FULL
    // original CPU path (compile + evaluate + display) -- never a fake empty frame and never a
    // silently different subset. This is deliberately distinct from `Unsupported`, which means the
    // composition itself could not compile.
    UnsupportedGpuSubset,
};

// Immutable output of the GPU-scene stage. Constructed once by
// makeCompositionPreviewGpuSceneStage() and retained by shared_ptr; it has no setters.
// `displayProcessor` is null exactly when the request takes the reference/unqualified display path.
class PreviewGpuSceneStage final {
  public:
    PreviewGpuSceneStage(
        PreviewRequestIdentity desiredIdentity, std::shared_ptr<const PreparedGpuScene> scene,
        std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> displayProcessor,
        std::size_t pixelStorageByteLimit, std::vector<TaskDiagnostic> diagnostics) noexcept
        : desiredIdentity_(std::move(desiredIdentity)), scene_(std::move(scene)),
          displayProcessor_(std::move(displayProcessor)),
          pixelStorageByteLimit_(pixelStorageByteLimit), diagnostics_(std::move(diagnostics)) {}

    // The general-display form: additionally carries the off-UI-prepared immutable GPU display
    // program (exact OCIO DisplayRgba8 command + its CPU oracle) for this request's own display/view
    // pair. A null displayProgram means the request takes the startup Neutral fast path or the CPU
    // display fallback; it is never a silent downgrade of a prepared general program.
    PreviewGpuSceneStage(
        PreviewRequestIdentity desiredIdentity, std::shared_ptr<const PreparedGpuScene> scene,
        std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> displayProcessor,
        std::shared_ptr<const GpuDisplayProgram> displayProgram, std::size_t pixelStorageByteLimit,
        std::vector<TaskDiagnostic> diagnostics) noexcept
        : desiredIdentity_(std::move(desiredIdentity)), scene_(std::move(scene)),
          displayProcessor_(std::move(displayProcessor)), displayProgram_(std::move(displayProgram)),
          pixelStorageByteLimit_(pixelStorageByteLimit), diagnostics_(std::move(diagnostics)) {}

    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const& noexcept {
        return desiredIdentity_;
    }
    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const&& = delete;

    // Non-null for every Prepared stage (the factory rejects a null scene before constructing one).
    // A future executor consumes THIS scene; it never re-prepares the graph.
    [[nodiscard]] const std::shared_ptr<const PreparedGpuScene>& scene() const& noexcept {
        return scene_;
    }
    [[nodiscard]] const std::shared_ptr<const PreparedGpuScene>& scene() const&& = delete;

    // Null on the reference/unqualified startup path; non-null once a qualified processor has been
    // selected. ocioQualified() is the one place that distinction is read.
    [[nodiscard]] const std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle>&
    displayProcessor() const& noexcept {
        return displayProcessor_;
    }
    [[nodiscard]] const std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle>&
    displayProcessor() const&& = delete;
    [[nodiscard]] bool ocioQualified() const noexcept { return displayProcessor_ != nullptr; }

    // The off-UI-prepared general GPU display program for this request's display/view pair. Null
    // when the request's display/view is the startup self-qualified Neutral pair (handled by the
    // service's fast path) or when display preparation was not attempted. Non-null means the service
    // must dispatch THIS command through the general display arm -- never fall back silently.
    [[nodiscard]] const std::shared_ptr<const GpuDisplayProgram>& displayProgram() const& noexcept {
        return displayProgram_;
    }
    [[nodiscard]] const std::shared_ptr<const GpuDisplayProgram>& displayProgram() const&& = delete;
    [[nodiscard]] bool hasGeneralDisplayProgram() const noexcept {
        return displayProgram_ != nullptr;
    }

    // The per-request byte allowance the scene was prepared under; carried so a downstream product
    // charges the same limit the preparation did.
    [[nodiscard]] std::size_t pixelStorageByteLimit() const noexcept {
        return pixelStorageByteLimit_;
    }

    // Compile + preparation diagnostics accumulated before display preparation. A fallback copies
    // these and appends its own diagnostics, exactly as the CPU stage does.
    [[nodiscard]] const std::vector<TaskDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

  private:
    PreviewRequestIdentity desiredIdentity_;
    std::shared_ptr<const PreparedGpuScene> scene_;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> displayProcessor_;
    std::shared_ptr<const GpuDisplayProgram> displayProgram_;
    std::size_t pixelStorageByteLimit_ = 0;
    std::vector<TaskDiagnostic> diagnostics_;
};

// A stage outcome is small enough to pass through TaskResult only as a handle. For Prepared the
// `stage` is non-null and `diagnostics` is empty (the stage owns them). For Unsupported and
// UnsupportedGpuSubset the `stage` is null and `diagnostics` explains the rejection so a caller can
// still publish or fall back.
struct PreviewGpuSceneStageOutcome final {
    PreviewGpuSceneStageStatus status = PreviewGpuSceneStageStatus::Unsupported;
    std::shared_ptr<const PreviewGpuSceneStage> stage;
    std::vector<TaskDiagnostic> diagnostics;
};

using PreviewGpuSceneStageOutcomeHandle = std::shared_ptr<const PreviewGpuSceneStageOutcome>;

// Compile -> prepare the immutable GPU scene -> select a CPU display processor. Returns a terminal
// TaskResult::cancelled or ::failed for cancellation and genuine failures (never a fake empty
// frame); returns Unsupported or UnsupportedGpuSubset as SUCCEEDED outcomes so the caller can
// distinguish a semantic compile rejection from a full CPU fallback. The inputs are the same
// immutable snapshot, request identity, allowance, override vector and TaskContext a
// PreviewCpuStageFunction takes.
using PreviewGpuSceneStageFunction = std::function<TaskResult<PreviewGpuSceneStageOutcomeHandle>(
    const document::Snapshot&, const PreviewRequestIdentity&, std::size_t,
    const std::vector<SnapshotParameterOverride>&, TaskContext&)>;

} // namespace bloom::runtime
