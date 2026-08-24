#pragma once

#include <bloom/ui/composition_preview_controller.hpp>

namespace bloom::runtime {
class CpuCompositionEvaluator;
class CpuReferenceDisplayPreparer;
class SnapshotCompiler;
} // namespace bloom::runtime

namespace bloom::ui {

[[nodiscard]] PreviewPreparationFunction
makeCompositionPreviewPipeline(const runtime::SnapshotCompiler& compiler,
                               const runtime::CpuCompositionEvaluator& evaluator,
                               const runtime::CpuReferenceDisplayPreparer& displayPreparer);

} // namespace bloom::ui
