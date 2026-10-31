#pragma once

// Keyframe handle modes (document 1.32): Blender's Auto Clamped handle computation and the
// Aligned/Auto bookkeeping that keeps stored handles consistent with each key's mode.
//
// The mode never reaches the sampler. Auto and Aligned are maintained by rewriting the STORED
// handle values, so a sampled value is always a pure function of the handles in the document and a
// mode-only edit is render-neutral. See docs/architecture/animation-and-time.md, "Handle modes".

#include <bloom/document/animation.hpp>

#include <optional>
#include <span>
#include <vector>

namespace bloom::document {

// One key as the Auto computation sees it: its time in seconds and its value on the sampled axis.
// A Path key's axis is its index (each segment runs 0 -> 1 in progress), which makes a handle value
// offset a fraction of the segment's progress, exactly as the Path sampler reads it.
struct AutoHandleKey {
    double time = 0.0;
    double value = 0.0;
};

// The handles computed for one key. A side whose segment does not exist (the incoming side of the
// first key, the outgoing side of the last) is absent, because no sample ever reads it.
struct AutoHandles {
    std::optional<KeyframeHandle> outgoing;
    std::optional<KeyframeHandle> incoming;
};

// Blender's Auto Clamped handles (BKE_nurb_handle_calc_ex for F-Curves with auto-handle smoothing
// off, plus the first/last-key flattening of BKE_fcurve_handles_recalc under constant
// extrapolation), transcribed in double. The handle direction follows the average of the two
// adjacent slopes, an interior extremum and any handle that would pass a neighbour's value are
// clamped flat so the curve never overshoots, and the opposite handle is re-aligned when only one
// side was clamped. `keys` must have strictly increasing times.
[[nodiscard]] std::vector<AutoHandles>
computeAutoClampedHandles(std::span<const AutoHandleKey> keys);

// Re-establishes the handle-mode invariants of one key sequence after an edit. `after` is the
// sequence as the edit left it; `before` is the sequence before the edit (empty when it is new).
// Keys are matched by id. In order:
//   1. An Auto key whose handle was changed by the edit becomes Aligned (Blender: dragging an Auto
//      handle converts the key).
//   2. An Aligned key whose one handle was changed gets the other handle made collinear (its length
//      is kept, its direction follows). If the key was collinear before and its adjacent segments
//      changed length, its incoming handle follows its outgoing one again.
//   3. Every Auto key's handles are recomputed from its neighbors.
// A sequence without Aligned or Auto keys is left untouched.
void settleKeyframeHandles(std::vector<ScalarKeyframe>& after,
                           const std::vector<ScalarKeyframe>& before);
void settleKeyframeHandles(std::vector<PathKeyframe>& after,
                           const std::vector<PathKeyframe>& before);

// Settles every key sequence of a curve record against the same curve before the edit (nullptr
// when the curve is new). A curve of a different shape is left untouched.
void settleAnimationCurveHandles(AnimationCurveRecord& after, const AnimationCurveRecord* before);

// True when any key of the sequence is Aligned or Auto.
[[nodiscard]] bool hasNonFreeHandleMode(std::span<const ScalarKeyframe> keys) noexcept;
[[nodiscard]] bool hasNonFreeHandleMode(std::span<const PathKeyframe> keys) noexcept;

// Makes `target` collinear with `anchor` through a key: `target` keeps its length (in time-value
// space, over its own segment's duration) and takes the opposite direction of `anchor`. The
// handle's time fraction is capped at 1 by shortening it. Either handle may be the outgoing or
// incoming one; `anchorSegmentSeconds` and `targetSegmentSeconds` are the durations of the
// segments they belong to. Returns `target` unchanged when `anchor` has no length.
[[nodiscard]] KeyframeHandle alignedHandle(const KeyframeHandle& anchor,
                                           double anchorSegmentSeconds,
                                           const KeyframeHandle& target,
                                           double targetSegmentSeconds) noexcept;

} // namespace bloom::document
