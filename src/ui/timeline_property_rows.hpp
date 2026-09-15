#pragma once

#include <array>
#include <bloom/ui/timeline_editor.hpp>
#include <functional>

class QLabel;
namespace bloom::ui {
class KeyframeDiamond;
namespace kit {
class KValueField;
class KDropdown;
class KColorChip;
class KIconButton;
} // namespace kit

[[nodiscard]] std::vector<TimelineLayerEntry> timelinePropertyEntries(
    const CompositionSession& session, const std::vector<TimelineLayerEntry>& layers,
    const std::set<document::LayerId>& expanded,
    const std::set<std::pair<document::LayerId, QString>>& collapsedGroups = {});

class TimelinePropertyRow final : public QWidget {
  public:
    TimelinePropertyRow(CompositionSession& session, QWidget* parent);
    void bind(const TimelineLayerEntry& entry);
    std::function<void(document::LayerId, QString)> toggleGroup;

  private:
    void commitValues();
    CompositionSession& session_;
    TimelineLayerEntry entry_;
    QLabel* label_;
    KeyframeDiamond* diamond_;
    std::array<kit::KValueField*, 2> fields_{};
    std::array<QWidget*, 2> cells_{};
    std::array<QLabel*, 2> components_{};
    kit::KDropdown* blending_;
    kit::KDropdown* alignment_;
    kit::KColorChip* color_;
    kit::KIconButton* disclosure_ = nullptr;
    bool binding_ = false;
};
} // namespace bloom::ui
