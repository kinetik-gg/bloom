#include <bloom/ui/composition_preview_pipeline.hpp>

#include <bloom/ui/composition_preview_cpu_stage.hpp>

#include <utility>

namespace bloom::ui {

PreviewPreparationFunction makeCompositionPreviewPipeline(
    const runtime::SnapshotCompiler& compiler, const runtime::CpuCompositionEvaluator& evaluator,
    const runtime::CpuReferenceDisplayPreparer& displayPreparer,
    const runtime::QualifiedDisplayProcessorProvider& qualifiedProcessorProvider,
    CompiledPlanCacheHandle planCache) {
    // The compile/evaluate/display-selection half and the display-application half each live in
    // composition_preview_cpu_stage.cpp, where they are also reachable on their own. This function
    // is their composition: one CPU stage per request, then the display product for the frame the
    // stage evaluated.
    auto stage = makeCompositionPreviewCpuStage(compiler, evaluator, qualifiedProcessorProvider,
                                                std::move(planCache));
    auto fallback = makeCompositionPreviewCpuDisplayFallback(displayPreparer);
    return makeCompositionPreviewPipelineFromCpuStage(std::move(stage), std::move(fallback));
}

} // namespace bloom::ui
