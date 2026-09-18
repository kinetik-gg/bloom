#pragma once
#include <bloom/ui/frame_export_controller.hpp>

namespace bloom::ui {
[[nodiscard]] std::optional<CompositionExportRequest>
compositionExportDialog(std::uint64_t maximumFrame, std::uint32_t sampleRate,
                        const QString& projectKey = {});
}
