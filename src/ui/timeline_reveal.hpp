#pragma once

// After Effects' Timeline keys, as Qt-free decisions the TimelineEditor's actions apply
// (docs/ux/interaction-model.md, "Timeline"): the property reveals (A anchor, P position, S scale,
// R rotation, T opacity, U keyed, UU modified, M masks, E effects; Shift adds), J/K keyframe
// navigation, and [ / ] / Alt+[ / Alt+] moving or trimming a layer to the playhead.

#include <bloom/ui/timeline_editor.hpp>

#include <bloom/commands/layer_timing_gesture.hpp>

#include <optional>
#include <set>
#include <vector>

namespace bloom::ui::timeline_reveal {

// The keys a parameter or component row shows, with their composition times: every key of the
// parameter's curve, or of the row's own component.
struct RowKey final {
    KeyframeSelection key;
    core::RationalTime time;
};
[[nodiscard]] std::vector<RowKey> rowKeys(const CompositionSession& session,
                                          const TimelineLayerEntry& row);

enum class Reveal { Anchor, Position, Scale, Rotation, Opacity, Keyed, Modified, Masks, Effects };

// The parameters of `layer` the reveal shows (empty: nothing to show).
[[nodiscard]] std::set<document::ParameterId>
revealedParameters(const CompositionSession& session, document::LayerId layer, Reveal reveal);

// The layer's next reveal. Plain: show exactly `wanted`, or nothing when that is already what
// shows. Shift (`additive`): add `wanted` to what shows, or take it away when all of it already
// shows. nullopt: the layer collapses.
[[nodiscard]] std::optional<std::set<document::ParameterId>>
nextReveal(const std::optional<std::set<document::ParameterId>>& current,
           const std::set<document::ParameterId>& wanted, bool additive);

// J / K: the nearest of `times` strictly before (or after) `now`.
[[nodiscard]] std::optional<core::RationalTime>
adjacentTime(const std::vector<core::RationalTime>& times, core::RationalTime now, bool forward);

// [ and ] move the layer so its in point (or its last frame) is at the playhead; Alt+[ and Alt+]
// trim the in point (or the out point, keeping the playhead's frame) to the playhead.
enum class PlayheadEdit { MoveIn, MoveOut, TrimIn, TrimOut };
[[nodiscard]] std::optional<commands::LayerTimingProposal>
playheadTiming(const CompositionSession& session, document::LayerId layer, PlayheadEdit edit);

} // namespace bloom::ui::timeline_reveal
