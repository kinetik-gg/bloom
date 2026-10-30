#pragma once

// Output Module, opened from an output's "Output Module" link (After Effects' Output Module
// Settings, with only what an export output carries). Grouped like After Effects:
//
//   “ProRes 422 HQ”  Format, If a file is there       | Post-render, Cached frames
//   Quality          the format's profile or compression,| its colour space, or display and view
//                    the look and audio
//   Numbering        start number (image sequences)    | digits
//
// Only the settings that apply to the chosen format show, and every format is offered alike.
// Choosing another format takes that preset's settings. OK writes every change as one undoable edit; Cancel changes nothing.

#include "export_colour_choices.hpp"

#include <bloom/host/export_queue.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/kit/dialog.hpp>

#include <functional>
#include <optional>

namespace bloom::ui::kit {
class KButton;
class KCheckBox;
class KDropdown;
class KSection;
class KValueField;
} // namespace bloom::ui::kit

namespace bloom::ui::exportpanel {

class OutputModuleDialog final : public kit::KDialog {
    Q_OBJECT

  public:
    OutputModuleDialog(const host::ExportOutputView& output,
                       std::shared_ptr<const host::ExportPresetCatalogueSnapshot> catalogue,
                       ExportColourChoices colours, QWidget& owner);

    // Every change the fields make, as one edit; nullopt when nothing changed.
    [[nodiscard]] std::optional<ExportOutputEdit> edit() const;
    // The settings the fields describe.
    [[nodiscard]] document::ExportOutputSettings settings() const;
    [[nodiscard]] QString presetId() const;

    // Test seams.
    [[nodiscard]] kit::KDropdown* format() const noexcept { return format_; }
    [[nodiscard]] kit::KDropdown* overwrite() const noexcept { return overwrite_; }
    [[nodiscard]] kit::KDropdown* profile() const noexcept { return profile_; }
    [[nodiscard]] kit::KDropdown* compression() const noexcept { return compression_; }
    [[nodiscard]] kit::KDropdown* colourSpace() const noexcept { return colourSpace_; }
    [[nodiscard]] kit::KDropdown* display() const noexcept { return display_; }
    [[nodiscard]] kit::KDropdown* view() const noexcept { return view_; }
    [[nodiscard]] kit::KCheckBox* bakeLook() const noexcept { return bakeLook_; }
    [[nodiscard]] kit::KCheckBox* includeAudio() const noexcept { return includeAudio_; }
    [[nodiscard]] kit::KCheckBox* cachedFrames() const noexcept { return cachedFrames_; }
    [[nodiscard]] kit::KCheckBox* importIntoAssets() const noexcept { return import_; }
    [[nodiscard]] kit::KValueField* startNumber() const noexcept { return startNumber_; }
    [[nodiscard]] kit::KCheckBox* frameNumbers() const noexcept { return frameNumbers_; }
    [[nodiscard]] kit::KValueField* padding() const noexcept { return padding_; }
    [[nodiscard]] kit::KSection* mainSection() const noexcept { return main_; }
    // Whether the row holding `field` shows.
    [[nodiscard]] bool shows(QWidget* field) const;

  private:
    void fillFormats();
    void showSettings(const document::ExportOutputSettings& settings);
    void fillColour(const document::ExportOutputSettings& settings);
    void formatChosen();
    [[nodiscard]] const host::ExportPreset* preset(const QString& id) const;

    host::ExportOutputView output_;
    std::shared_ptr<const host::ExportPresetCatalogueSnapshot> catalogue_;
    ExportColourChoices colours_;
    // The settings the dialog started from for the current format (the output's own, or the chosen
    // preset's after a format change); the fields override what applies.
    document::ExportOutputSettings base_;
    kit::KSection* main_ = nullptr;
    kit::KSection* quality_ = nullptr;
    kit::KSection* numbering_ = nullptr;
    kit::KForm* mainLeft_ = nullptr;
    kit::KForm* qualityLeft_ = nullptr;
    kit::KForm* qualityRight_ = nullptr;
    kit::KDropdown* format_ = nullptr;
    kit::KDropdown* overwrite_ = nullptr;
    kit::KCheckBox* import_ = nullptr;
    kit::KCheckBox* cachedFrames_ = nullptr;
    kit::KDropdown* profile_ = nullptr;
    kit::KDropdown* compression_ = nullptr;
    kit::KDropdown* colourSpace_ = nullptr;
    kit::KDropdown* display_ = nullptr;
    kit::KDropdown* view_ = nullptr;
    kit::KCheckBox* bakeLook_ = nullptr;
    kit::KCheckBox* includeAudio_ = nullptr;
    kit::KValueField* startNumber_ = nullptr;
    kit::KCheckBox* frameNumbers_ = nullptr;
    kit::KValueField* padding_ = nullptr;
    QWidget* profileRow_ = nullptr;
    QWidget* compressionRow_ = nullptr;
    QWidget* colourSpaceRow_ = nullptr;
    QWidget* displayRow_ = nullptr;
    QWidget* viewRow_ = nullptr;
    QWidget* lookRow_ = nullptr;
    QWidget* audioRow_ = nullptr;
    bool refreshing_ = false;
};

} // namespace bloom::ui::exportpanel
