#pragma once

#include <bloom/ui/composition_preview_controller.hpp>

namespace bloom::runtime {
class SnapshotCompiler;
}

namespace bloom::ui {

[[nodiscard]] PreviewPreparationFunction
makeSnapshotCompilePreparation(const runtime::SnapshotCompiler& compiler);

} // namespace bloom::ui
