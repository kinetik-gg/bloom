#pragma once

// Post-confirmation anchor editing shared by every path the Viewer edits -- a Pen shape-layer path
// and a layer mask alike. Pure functions over document::PathValue in the path's own space: the
// Viewer maps a gesture into that space, applies one of these, previews the result through its
// session-only override and commits it as one command.

#include <bloom/document/parameter.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <utility>

namespace bloom::ui::path_editing {

using AnchorSet = std::set<std::size_t>;

// The anchor's editing mode. A Corner-labelled anchor that nevertheless carries handles (built by
// code that predates the mode) reads as the mode its geometry implies.
[[nodiscard]] document::PathAnchorMode modeOf(const document::PathAnchor& anchor) noexcept;

// The one mode every listed anchor shares; nullopt for a mixed or empty selection.
[[nodiscard]] std::optional<document::PathAnchorMode> commonMode(const document::PathValue& path,
                                                                 const AnchorSet& anchors);

// Catmull-Rom handles from the neighbouring anchors: (point - prev, next - point) tangent over six
// for an interior anchor, a third of the one neighbour's offset at an open end. nullopt when the
// anchor has no neighbour.
[[nodiscard]] std::optional<std::pair<document::Vec2d, document::Vec2d>>
autoHandles(const document::PathValue& path, std::size_t index);

// Recomputes every Auto anchor's handles from its current neighbours.
void refreshAuto(document::PathValue& path);

// Converts the listed anchors. Corner drops the handles; a curved mode on a handle-less anchor
// pulls symmetric handles sized from its neighbours; Mirrored and Aligned straighten existing
// handles onto their common tangent (Mirrored also equalises their lengths); Free keeps them.
void setMode(document::PathValue& path, const AnchorSet& anchors, document::PathAnchorMode mode);

// Moves anchors with their handles; neighbouring Auto anchors follow.
void moveAnchors(document::PathValue& path, const AnchorSet& anchors, document::Vec2d delta);

enum class HandleSide : std::uint8_t { In, Out };

// Places one handle. `free` (the Alt drag) leaves the opposite handle alone and makes the anchor
// Free. Otherwise the mode decides: Mirrored mirrors the opposite handle, Aligned rotates it and
// keeps its length, Free leaves it, and an Auto anchor becomes Aligned.
void dragHandle(document::PathValue& path, std::size_t index, HandleSide side,
                document::Vec2d position, bool free);

// Pulls new symmetric handles out of an anchor (the Alt drag from a point): Mirrored.
void pullHandles(document::PathValue& path, std::size_t index, document::Vec2d outgoing);

// The Alt click: a Corner becomes `curved`; any curved anchor becomes a Corner.
void toggleCorner(document::PathValue& path, std::size_t index, document::PathAnchorMode curved);

[[nodiscard]] std::size_t segmentCount(const document::PathValue& path) noexcept;
// The cubic control points of segment `segment` (anchor `segment` to the next one).
[[nodiscard]] std::array<document::Vec2d, 4> segmentControls(const document::PathValue& path,
                                                             std::size_t segment);
[[nodiscard]] document::Vec2d segmentPoint(const std::array<document::Vec2d, 4>& controls,
                                           double t) noexcept;

struct SegmentHit {
    std::size_t segment = 0;
    double t = 0.0;
    double distance = 0.0;
};
// The closest point on any segment to `target`, measured after `toScreen` maps path space to the
// space `target` is in.
[[nodiscard]] std::optional<SegmentHit>
nearestSegmentPoint(const document::PathValue& path,
                    const std::function<document::Vec2d(document::Vec2d)>& toScreen,
                    document::Vec2d target);

// Splits segment `hit.segment` at `hit.t` (De Casteljau), so the drawn shape is unchanged, and
// returns the new anchor's index. nullopt at the anchor limit.
std::optional<std::size_t> insertAnchor(document::PathValue& path, const SegmentHit& hit);

// The fewest anchors a path may keep: a mask encloses an area (three), a shape path draws a line.
[[nodiscard]] constexpr std::size_t minimumAnchors(const bool mask) noexcept {
    return mask ? 3 : 2;
}

// Removes the listed anchors. Refused (false, path unchanged) when fewer than `minimum` would
// remain; a closed path left with fewer than three anchors opens.
bool deleteAnchors(document::PathValue& path, const AnchorSet& anchors, std::size_t minimum);

// Closing needs at least three anchors; opening is always allowed.
bool setClosed(document::PathValue& path, bool closed);

// Reverses the anchor order (swapping each anchor's handles), so drawing can continue from what
// was the first anchor.
void reverse(document::PathValue& path);

} // namespace bloom::ui::path_editing
