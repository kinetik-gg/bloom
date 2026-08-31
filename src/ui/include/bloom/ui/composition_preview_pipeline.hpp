#pragma once

#include <bloom/runtime/qualified_display_processor_provider.hpp>
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
[[nodiscard]] PreviewPreparationFunction makeCompositionPreviewPipeline(
    const runtime::SnapshotCompiler& compiler, const runtime::CpuCompositionEvaluator& evaluator,
    const runtime::CpuReferenceDisplayPreparer& displayPreparer,
    const runtime::QualifiedDisplayProcessorProvider& qualifiedProcessorProvider);

} // namespace bloom::ui
