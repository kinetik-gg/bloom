#pragma once

#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/ui/composition_plan_cache.hpp>
#include <bloom/ui/composition_preview_controller.hpp>

namespace bloom::runtime {
class CpuCompositionEvaluator;
class CpuReferenceDisplayPreparer;
class SnapshotCompiler;
} // namespace bloom::runtime

namespace bloom::ui {

// `qualifiedProcessorProvider` implements design decisions 3-5 (issue #97, task C3): every request
// routes through the qualified preparer once the provider reports Ready; before that (the honest
// startup window) it routes through `displayPreparer`, labeled unqualified exactly as it already
// is today; once the provider reports Failed, no further preview request is prepared at all (the
// pipeline returns a typed failure so CompositionPreviewController retains its last-good frame and
// surfaces the diagnostic -- see composition_preview_controller.cpp's existing Failed handling,
// unchanged) -- Bloom never auto-substitutes the reference transform for a qualified request once
// qualification is known to have failed.
// `planCache` is where the compiled plan for the live document revision is kept, so that scrubbing
// or playing a composition compiles nothing (task PERF1; a compiled plan is time-independent -- see
// CompiledPlanCache). Pass one to share it with another surface or to read its statistics; omit it
// and the pipeline owns a cache of its own.
//
// The per-row kernels of both evaluation and display preparation run across the row-band pool the
// TaskScheduler handed to this request's own TaskContext, so preview frames use the machine's cores
// without this function needing to know how many there are.
[[nodiscard]] PreviewPreparationFunction makeCompositionPreviewPipeline(
    const runtime::SnapshotCompiler& compiler, const runtime::CpuCompositionEvaluator& evaluator,
    const runtime::CpuReferenceDisplayPreparer& displayPreparer,
    const runtime::QualifiedDisplayProcessorProvider& qualifiedProcessorProvider,
    CompiledPlanCacheHandle planCache = nullptr);

} // namespace bloom::ui
