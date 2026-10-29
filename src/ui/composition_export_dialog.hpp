#pragma once

#include <bloom/ui/frame_export_controller.hpp>

class QWidget;

namespace bloom::ui {

class H264EncoderService;

struct CompositionExportDialogOptions final {
    std::uint64_t maximumFrame = 0;
    std::uint32_t sampleRate = 48000;
    // Remembers the last named deliverable per project.
    QString projectKey;
    const document::ColorSettings* colorSettings = nullptr;
    // Which H.264 encoder the platform uses and whether OpenH264 is verified, refreshed on a
    // worker. Without one, an OpenH264 platform asks for consent or hardware (it never hashes on
    // the UI thread to find out).
    H264EncoderService* encoder = nullptr;
};

// Export Composition: one kit dialog, modal over `owner`. Synchronous (exec): the request goes
// straight back to FrameExportController::requestCompositionExport().
[[nodiscard]] std::optional<CompositionExportRequest>
compositionExportDialog(QWidget& owner, const CompositionExportDialogOptions& options);

// The file extension a preset writes (".mov", ".png", ...).
[[nodiscard]] QString compositionExportExtension(output::OutputPresetV1 preset);

} // namespace bloom::ui
