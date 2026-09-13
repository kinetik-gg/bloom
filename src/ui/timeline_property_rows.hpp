#pragma once

#include <bloom/ui/timeline_editor.hpp>
#include <array>

class QLabel;
namespace bloom::ui {
class KeyframeDiamond;
namespace kit {
class KValueField;
class KDropdown;
class KColorChip;
}

[[nodiscard]] std::vector<TimelineLayerEntry> timelinePropertyEntries(
    const CompositionSession& session, const std::vector<TimelineLayerEntry>& layers,
    const std::set<document::LayerId>& expanded);

class TimelinePropertyRow final : public QWidget {
  public:
    TimelinePropertyRow(CompositionSession& session, QWidget* parent);
    void bind(const TimelineLayerEntry& entry);
  private:
    void commitValues();
    CompositionSession& session_;
    TimelineLayerEntry entry_;
    QLabel* label_;
    KeyframeDiamond* diamond_;
    std::array<kit::KValueField*, 4> fields_{};
    kit::KDropdown* blending_;
    kit::KColorChip* color_;
    bool binding_ = false;
};
}
