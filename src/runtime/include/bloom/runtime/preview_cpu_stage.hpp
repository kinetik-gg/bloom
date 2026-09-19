#pragma once

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

// The pure, Qt-free seam between the composition preview's CPU half and whatever display product is
// applied to it. Nothing here knows about GPU, Vulkan, Qt, or the UI. A PreviewCpuStage is the
// immutable result of compiling, evaluating, and SELECTING a CPU display processor for one request:
// it retains the evaluated ProcessFrame and the selected color::PreparedCpuDisplayProcessorHandle
// (null means the reference/unqualified startup path) so that a display-only fallback can map the
// same frame without ever recompiling or re-evaluating.
//
// This header introduces no new document, plan, or color type: every field is an existing runtime
// value. The factories that build these functions live in bloom::ui
// (bloom/ui/composition_preview_cpu_stage.hpp) because the compiled-plan cache is a UI-owned
// handle, exactly as makeCompositionPreviewPipeline already is.
namespace bloom::runtime {

enum class PreviewCpuStageStatus : std::uint8_t {
    // A plan was compiled and a frame evaluated; `stage` carries it.
    Evaluated,
    // Compilation rejected the composition semantically; no frame exists. This is an explicit
    // status, never a fabricated empty frame.
    Unsupported,
};

// Immutable output of the CPU stage. Constructed once by makeCompositionPreviewCpuStage() and
// retained by shared_ptr; it has no setters. `displayProcessor` is null exactly when the request
// takes the reference/unqualified display path.
class PreviewCpuStage final {
  public:
    PreviewCpuStage(
        PreviewRequestIdentity desiredIdentity, std::shared_ptr<const ProcessFrame> processFrame,
        std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> displayProcessor,
        std::size_t pixelStorageByteLimit, std::vector<TaskDiagnostic> diagnostics) noexcept
        : desiredIdentity_(std::move(desiredIdentity)), processFrame_(std::move(processFrame)),
          displayProcessor_(std::move(displayProcessor)),
          pixelStorageByteLimit_(pixelStorageByteLimit), diagnostics_(std::move(diagnostics)) {}

    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const& noexcept {
        return desiredIdentity_;
    }
    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const&& = delete;

    // Non-null for every Evaluated stage (the stage factory rejects a null evaluation frame before
    // constructing one). A fallback maps THIS frame; it never evaluates the graph again.
    [[nodiscard]] const std::shared_ptr<const ProcessFrame>& processFrame() const& noexcept {
        return processFrame_;
    }
    [[nodiscard]] const std::shared_ptr<const ProcessFrame>& processFrame() const&& = delete;

    // Null on the reference/unqualified startup path; non-null once a qualified processor has been
    // selected. ocioQualified() is the one place that distinction is read.
    [[nodiscard]] const std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle>&
    displayProcessor() const& noexcept {
        return displayProcessor_;
    }
    [[nodiscard]] const std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle>&
    displayProcessor() const&& = delete;
    [[nodiscard]] bool ocioQualified() const noexcept { return displayProcessor_ != nullptr; }

    // The per-request display budget the request was built with; carried so the display-only
    // fallback charges the same limit the evaluation did.
    [[nodiscard]] std::size_t pixelStorageByteLimit() const noexcept {
        return pixelStorageByteLimit_;
    }

    // Compile + evaluation diagnostics accumulated before display preparation. The fallback copies
    // these and appends its own display diagnostics, exactly as makeCompositionPreviewPipeline's
    // single accumulating vector did.
    [[nodiscard]] const std::vector<TaskDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

  private:
    PreviewRequestIdentity desiredIdentity_;
    std::shared_ptr<const ProcessFrame> processFrame_;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> displayProcessor_;
    std::size_t pixelStorageByteLimit_ = 0;
    std::vector<TaskDiagnostic> diagnostics_;
};

// A stage outcome is small enough to pass through TaskResult only as a handle: the value type is
// std::shared_ptr<const PreviewCpuStageOutcome> (16 bytes), not the struct itself, because
// TaskResultValue caps a value at four pointers. The handle mirrors PreviewPreparationResultHandle.
//
// For Evaluated the `stage` is non-null and `diagnostics` is empty (the stage owns them). For
// Unsupported the `stage` is null and `diagnostics` carries the compile diagnostics that explain
// the rejection, so a caller can still publish them.
struct PreviewCpuStageOutcome final {
    PreviewCpuStageStatus status = PreviewCpuStageStatus::Unsupported;
    std::shared_ptr<const PreviewCpuStage> stage;
    std::vector<TaskDiagnostic> diagnostics;
};

using PreviewCpuStageOutcomeHandle = std::shared_ptr<const PreviewCpuStageOutcome>;

// Compile -> evaluate -> select a CPU display processor. Returns a terminal TaskResult::cancelled
// or ::failed for cancellation and genuine failures (never a fake empty frame); returns
// PreviewCpuStageStatus::Unsupported as a SUCCEEDED outcome for a semantic compile rejection.
using PreviewCpuStageFunction = std::function<TaskResult<PreviewCpuStageOutcomeHandle>(
    const document::Snapshot&, const PreviewRequestIdentity&, std::size_t,
    const std::vector<SnapshotParameterOverride>&, TaskContext&)>;

// Apply a display product to an already-evaluated stage's ProcessFrame and produce the final
// PreviewPreparationResult. It must not compile or evaluate anything. The stage already owns the
// request identity and the display budget it was evaluated under, so the fallback reads them from
// the stage rather than taking duplicate parameters that could disagree with it.
using PreviewCpuDisplayFallback =
    std::function<TaskResult<PreviewPreparationResultHandle>(const PreviewCpuStage&, TaskContext&)>;

} // namespace bloom::runtime
