#pragma once

#include <bloom/ui/composition_preview_controller.hpp>

namespace bloom::runtime {
class CpuCompositionEvaluator;
class SnapshotCompiler;
} // namespace bloom::runtime

namespace bloom::ui {

[[nodiscard]] PreviewPreparationFunction
makeCompositionPreviewPipeline(const runtime::SnapshotCompiler& compiler,
                               const runtime::CpuCompositionEvaluator& evaluator);

} // namespace bloom::ui
