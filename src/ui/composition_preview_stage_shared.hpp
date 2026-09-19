#pragma once

// Private helpers shared by the CPU stage and the GPU-scene stage. They are the single
// implementation of the compile/validate/request-build/processor-selection half both stages must
// agree on, extracted from composition_preview_cpu_stage.cpp so the two stages cannot drift. They
// are not a public seam: nothing outside src/ui includes this header.

#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/document/document.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/compiled_plan_cache.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_types.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::ui::detail {

[[nodiscard]] std::vector<runtime::TaskDiagnostic>
compileTaskDiagnostics(const runtime::SnapshotCompileResult& result);

[[nodiscard]] std::vector<runtime::TaskDiagnostic>
evaluationTaskDiagnostics(const runtime::EvaluationResult& result);

[[nodiscard]] runtime::TaskDiagnostic missingResultDiagnostic(std::string summary);

[[nodiscard]] bool isNeutralIntent(const runtime::EvaluationColorIntent& intent) noexcept;

// Nullopt when the request addresses the snapshot and carries a usable identity; otherwise the
// fail-closed diagnostic the stage returns as TaskResult::failed. Mirrors the CPU stage's exact
// project/composition/revision/generation/allowance checks.
[[nodiscard]] std::optional<runtime::TaskDiagnostic>
validateStageRequest(const document::Snapshot& snapshot,
                     const runtime::PreviewRequestIdentity& desiredIdentity,
                     std::size_t pixelStorageByteLimit);

// The one compiled-plan path both stages use: the UI-owned cache for a revision, the direct
// per-gesture plan for an override request, exactly as CompiledPlanCache documents.
[[nodiscard]] runtime::SnapshotCompileResult
compilePreviewPlan(const runtime::SnapshotCompiler& compiler,
                   const std::shared_ptr<runtime::CompiledPlanCache>& planCache,
                   const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& desiredIdentity,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides,
                   const runtime::CancellationToken& cancellation);

// The one EvaluationRequest construction both stages use, so time/output/resolution/quality/color/
// ROI/showLook/allowance cannot drift between them.
[[nodiscard]] runtime::EvaluationRequest
evaluationRequestFor(const runtime::PreviewRequestIdentity& desiredIdentity,
                     const runtime::CompiledCompositionPlan& plan,
                     std::size_t pixelStorageByteLimit) noexcept;

struct SelectedDisplayProcessor final {
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> handle;
    // True only for a non-neutral intent whose qualified processor could not be built: the
    // fail-closed case the caller must turn into a failed stage.
    bool failed = false;
};

// The one processor-selection decision: the Ready qualified handle for a default neutral request,
// null during the Pending reference window, and the explicitly built ACES/named-view handle
// otherwise. A non-neutral request that cannot build one reports failed.
[[nodiscard]] SelectedDisplayProcessor
selectDisplayProcessor(const runtime::EvaluationColorIntent& intent,
                       const runtime::QualifiedDisplayProcessorSnapshot& qualifiedSnapshot,
                       std::string_view displayName, std::string_view viewName) noexcept;

} // namespace bloom::ui::detail
