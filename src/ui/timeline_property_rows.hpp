#pragma once

#include <array>
#include <bloom/ui/timeline_editor.hpp>
#include <functional>

class QLabel;
class QSpacerItem;
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

[[nodiscard]] std::vector<TimelineLayerEntry>
timelinePropertyEntries(const CompositionSession& session,
                        const std::vector<TimelineLayerEntry>& layers,
                        const std::set<document::LayerId>& expanded,
                        const std::set<std::pair<document::LayerId, QString>>& collapsedGroups = {},
                        const std::set<document::ParameterId>& expandedParameters = {});

class TimelinePropertyRow final : public QWidget {
  public:
    TimelinePropertyRow(CompositionSession& session, QWidget* parent);
    void bind(const TimelineLayerEntry& entry);
    std::function<void(document::LayerId, QString)> toggleGroup;
    std::function<void(document::ParameterId)> toggleParameter;

  private:
    void commitValues(std::size_t index);
    void bindDriven(bool driven);
    CompositionSession& session_;
    TimelineLayerEntry entry_;
    QLabel* label_;
    // task TL-FIX2: the row's OWN single diamond -- the aggregate tri-state diamond on a parameter
    // row, or (bound with entry.component set) that one component's diamond on an expanded
    // component row. There is never a second diamond in a value cell any more; see bind().
    KeyframeDiamond* diamond_;
    // Four colour channels, or the first two/three cells for a vector.
    std::array<kit::KValueField*, 4> fields_{};
    std::array<QWidget*, 4> cells_{};
    std::array<QLabel*, 4> components_{};
    kit::KDropdown* blending_;
    kit::KDropdown* alignment_;
    kit::KColorChip* color_;
    // Task DRIVE-1: what a DRIVEN parameter shows instead of an editor -- the driver node's name
    // behind a link glyph, and the value the graph resolves it to, read-only. Never an empty cell.
    QWidget* driven_ = nullptr;
    kit::KButton* driverLink_ = nullptr;
    kit::KLabel* drivenValue_ = nullptr;
    kit::KIconButton* disclosure_ = nullptr;
    // task TL-FIX2: the depth-driven indent before the row's own KPropertyRow -- see bind() and
    // propertyRowIndent() in timeline_editor.cpp for the step and the label-width compensation that
    // keeps every value column aligned regardless of depth.
    QSpacerItem* indent_ = nullptr;
    bool binding_ = false;
};
} // namespace bloom::ui
