#pragma once

// The widgets of ExportDetailPane, shared by its translation units (the pane's construction and
// editing in export_detail_pane.cpp, its results and log in export_detail_results.cpp).

#include "export_editor_internal.hpp"

#include <QList>

class QVBoxLayout;

namespace bloom::ui::exportpanel {

struct ExportDetailPane::Fields final {
    // A refusal from the host for the last edit, until the next snapshot.
    kit::KLabel* refusal = nullptr;

    // Job actions.
    kit::KButton* start = nullptr;
    kit::KButton* pauseResume = nullptr;
    kit::KButton* stop = nullptr;
    kit::KButton* retry = nullptr;

    // Render.
    kit::KLineEdit* name = nullptr;
    kit::KDropdown* rangeKind = nullptr;
    kit::KValueField* first = nullptr;
    kit::KValueField* last = nullptr;
    kit::KLabel* rangeSpan = nullptr;
    kit::KLabel* rangeReason = nullptr;
    kit::KSwitch* jobEnabled = nullptr;
    kit::KColorChip* label = nullptr;
    kit::KButton* clearLabel = nullptr;
    kit::KPropertyRow* firstRow = nullptr;
    kit::KPropertyRow* rangeSpanRow = nullptr;
    kit::KDropdown* sequenceStartMode = nullptr;
    kit::KPropertyRow* lastRow = nullptr;
    kit::KPropertyRow* labelRow = nullptr;
    kit::KPropertyRow* rangeReasonRow = nullptr;

    // Outputs.
    kit::KDropdown* outputPicker = nullptr;
    kit::KPropertyRow* outputPickerRow = nullptr;
    kit::KDropdown* preset = nullptr;
    kit::KLabel* presetReason = nullptr;
    kit::KButton* remedy = nullptr;
    kit::KPropertyRow* presetReasonRow = nullptr;
    kit::KDropdown* profile = nullptr;
    kit::KValueField* bitrate = nullptr;
    kit::KDropdown* compression = nullptr;
    kit::KLineEdit* colourSpace = nullptr;
    kit::KLineEdit* display = nullptr;
    kit::KLineEdit* view = nullptr;
    kit::KSwitch* bakeLook = nullptr;
    kit::KSwitch* useCachedFrames = nullptr;
    kit::KSwitch* includeAudio = nullptr;
    kit::KDropdown* sampleRate = nullptr;
    kit::KDropdown* encoder = nullptr;
    kit::KValueField* sequenceStart = nullptr;
    kit::KValueField* padding = nullptr;
    kit::KLineEdit* pattern = nullptr;
    kit::KPathField* destination = nullptr;
    kit::KButton* browse = nullptr;
    kit::KLabel* destinationReason = nullptr;
    kit::KPropertyRow* destinationReasonRow = nullptr;
    kit::KLabel* tokenHelp = nullptr;
    kit::KDropdown* overwrite = nullptr;
    kit::KLabel* replaceText = nullptr;
    kit::KButton* replace = nullptr;
    kit::KPropertyRow* replaceRow = nullptr;
    kit::KLabel* estimate = nullptr;
    kit::KPropertyRow* estimateRow = nullptr;
    kit::KSwitch* outputEnabled = nullptr;
    kit::KButton* addOutput = nullptr;
    kit::KButton* removeOutput = nullptr;
    // Rows that exist only for some formats, with the formats' test.
    kit::KPropertyRow* profileRow = nullptr;
    kit::KPropertyRow* bitrateRow = nullptr;
    kit::KPropertyRow* compressionRow = nullptr;
    kit::KPropertyRow* colourSpaceRow = nullptr;
    kit::KPropertyRow* displayRow = nullptr;
    kit::KPropertyRow* viewRow = nullptr;
    kit::KPropertyRow* bakeLookRow = nullptr;
    kit::KPropertyRow* useCachedFramesRow = nullptr;
    kit::KPropertyRow* includeAudioRow = nullptr;
    kit::KPropertyRow* sampleRateRow = nullptr;
    kit::KPropertyRow* encoderRow = nullptr;
    kit::KPropertyRow* sequenceStartRow = nullptr;
    kit::KPropertyRow* sequenceStartNumberRow = nullptr;
    kit::KPropertyRow* paddingRow = nullptr;
    kit::KPropertyRow* patternRow = nullptr;

    // Post-render.
    kit::KSwitch* importIntoAssets = nullptr;
    kit::KLabel* importResult = nullptr;
    kit::KPropertyRow* importResultRow = nullptr;

    // Log and verification.
    QVBoxLayout* verification = nullptr;
    QString verificationSignature;
    kit::KTextArea* log = nullptr;
    kit::KButton* detailsToggle = nullptr;
    kit::KButton* copyDetails = nullptr;
    QWidget* detailsRow = nullptr;
    kit::KTextArea* details = nullptr;

    // Every control that edits project truth, disabled with the host's reason while the job runs.
    QList<QWidget*> editors;
    QList<QWidget*> outputEditors;
};

} // namespace bloom::ui::exportpanel
