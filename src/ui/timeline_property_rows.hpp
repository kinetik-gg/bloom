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
class KButton;
class KLabel;
} // namespace kit

// Task DRIVE-1. The collapse key of the twirl-down group that lists one upstream value node's
// parameters. It is the node's identity rather than its display name, which two nodes may share.
[[nodiscard]] QString upstreamGroupKey(document::NodeId nodeId);

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
    void bindDriven(bool driven);
    CompositionSession& session_;
    TimelineLayerEntry entry_;
    QLabel* label_;
    KeyframeDiamond* diamond_;
    // Three cells, because a Vector 3 parameter has three components and task DRIVE-1's upstream
    // groups put one in the twirl-down: a row that showed two of them would be showing a value
    // that is not the parameter's.
    std::array<kit::KValueField*, 3> fields_{};
    std::array<QWidget*, 3> cells_{};
    std::array<QLabel*, 3> components_{};
    kit::KDropdown* blending_;
    kit::KDropdown* alignment_;
    kit::KColorChip* color_;
    // Task DRIVE-1: what a DRIVEN parameter shows instead of an editor -- the driver node's name
    // behind a link glyph, and the value the graph resolves it to, read-only. Never an empty cell.
    QWidget* driven_ = nullptr;
    kit::KButton* driverLink_ = nullptr;
    kit::KLabel* drivenValue_ = nullptr;
    kit::KIconButton* disclosure_ = nullptr;
    bool binding_ = false;
};
} // namespace bloom::ui
