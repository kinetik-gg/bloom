#pragma once

#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/preview_cpu_stage.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/composition_plan_cache.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>

// UI-owned factories for the CPU stage and its display-only fallback. They live in bloom::ui
// because the compiled-plan cache is a UI-owned handle (CompiledPlanCacheHandle), exactly as
// makeCompositionPreviewPipeline already is; the functions they return are pure runtime types with
// no Qt or UI dependency.
namespace bloom::ui {

// Compile -> evaluate -> select a CPU display processor, reproducing
// makeCompositionPreviewPipeline's compile/evaluate/selection half: the same request validation,
// the same fail-closed check for a permanently failed qualified provider, the same
// Unsupported/Cancelled/Failed returns, the same Pending-reference window, the same
// ACES/non-default-view/viewAdjust selection, and the same plan identity through `planCache`.
[[nodiscard]] runtime::PreviewCpuStageFunction makeCompositionPreviewCpuStage(
    const runtime::SnapshotCompiler& compiler, const runtime::CpuCompositionEvaluator& evaluator,
    const runtime::QualifiedDisplayProcessorProvider& qualifiedProcessorProvider,
    CompiledPlanCacheHandle planCache = nullptr);

// Apply a display product to an already-evaluated stage's ProcessFrame, reproducing
// makeCompositionPreviewPipeline's display half: the same reference-vs-qualified branch (chosen by
// PreviewCpuStage::ocioQualified()), the same progress reporting, the same diagnostics, and the
// same PreparedPreviewFrame identity construction. It never compiles or evaluates.
[[nodiscard]] runtime::PreviewCpuDisplayFallback makeCompositionPreviewCpuDisplayFallback(
    const runtime::CpuReferenceDisplayPreparer& displayPreparer);

// The composition makeCompositionPreviewPipeline() installs: run the stage, then either publish an
// Unsupported result (carrying the stage's diagnostics) or hand the immutable stage to the display
// fallback. The public PreviewPreparationFunction alias is unchanged.
[[nodiscard]] PreviewPreparationFunction
makeCompositionPreviewPipelineFromCpuStage(runtime::PreviewCpuStageFunction stage,
                                           runtime::PreviewCpuDisplayFallback fallback);

} // namespace bloom::ui
