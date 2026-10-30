#pragma once

// Render Settings, opened from a job's "Render Settings" link (After Effects' Render Settings
// dialog, with only what an export job carries: its range). One group, `Composition “SC020”`: the
// Range on the left, and on the right where it starts and ends (frame fields for a custom range or
// one frame, the timecode beside them) and how long it is. OK writes the range as one undoable
// edit; Cancel changes nothing.

#include <bloom/document/composition_settings.hpp>
#include <bloom/document/export_queue.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/ui/kit/dialog.hpp>

#include <optional>

namespace bloom::document {
class Project;
}

namespace bloom::ui::kit {
class KDropdown;
class KValueField;
} // namespace bloom::ui::kit

namespace bloom::ui::exportpanel {

class RenderSettingsDialog final : public kit::KDialog {
    Q_OBJECT

  public:
    RenderSettingsDialog(const document::Project& project, const host::ExportJobView& job,
                         QWidget& owner);

    // The range the fields describe, and whether it differs from the job's.
    [[nodiscard]] document::ExportRange range() const;
    [[nodiscard]] bool changed() const { return range() != initial_; }

    // Test seams.
    [[nodiscard]] kit::KDropdown* rangeKind() const noexcept { return kind_; }
    [[nodiscard]] kit::KValueField* startField() const noexcept { return first_; }
    [[nodiscard]] kit::KValueField* endField() const noexcept { return last_; }
    [[nodiscard]] QString startText() const;
    [[nodiscard]] QString endText() const;
    [[nodiscard]] QString durationText() const;
    [[nodiscard]] bool endShown() const;

  private:
    void refresh();
    [[nodiscard]] std::optional<host::ExportResolvedRange> resolved() const;

    document::ExportRange initial_;
    document::FrameRate rate_ = document::FrameRate::framesPerSecond24();
    std::optional<host::ExportResolvedRange> workArea_;
    std::optional<host::ExportResolvedRange> whole_;
    kit::KForm* times_ = nullptr;
    kit::KDropdown* kind_ = nullptr;
    kit::KValueField* first_ = nullptr;
    kit::KValueField* last_ = nullptr;
    kit::KLabel* firstTime_ = nullptr;
    kit::KLabel* lastTime_ = nullptr;
    kit::KLabel* duration_ = nullptr;
    QWidget* firstRow_ = nullptr;
    QWidget* lastRow_ = nullptr;
};

} // namespace bloom::ui::exportpanel
