#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>

#include <cstdint>
#include <optional>

namespace bloom::ui {

// Thin, widget-free adapters over bloom::core::FrameTimeMapping (src/core/include/bloom/core/
// frame_time_mapping.hpp) implementing docs/architecture/animation-and-time.md, "Session Time And
// Scrubbing," verbatim:
//
//   Frame index i maps to exact time i * frameRate.denominator / frameRate.numerator; the maximum
//   is the greatest non-negative i whose mapped time is strictly less than duration. Products and
//   comparisons use checked multiword arithmetic. A UI scrub gesture clamps to frame indices whose
//   exact time is in [0, duration), then selects the nearest index with an exact halfway tie going
//   to the greater index.
//
// FrameTimeMapping already implements this contract exactly (timeForFrame, maximumFrameIndex,
// nearestFrameIndex use the same private fixed-width UInt128 checked arithmetic as the sampling
// interval factor, and are pinned by src/core/tests/frame_time_mapping_tests.cpp). Per AGENTS.md
// and this task's instruction to reuse the core rational surfaces rather than hand-roll arithmetic
// where it exists, these three functions do not reimplement any of that math -- each one only
// constructs a bloom::core::FrameTimeMapping from the composition's document::FrameRate/duration
// and delegates. A refused mapping (invalid rate, non-positive duration, or an unrepresentable
// frame range/time) surfaces as std::nullopt rather than UB, matching FrameTimeMapping's own
// structured-failure contract.
//
// Unlike the doc's standalone per-index formula, frameTimeForIndex here also takes the composition
// duration: FrameTimeMapping::create requires it to build the checked mapping it delegates to, and
// every real call site (the timeline ruler, keyframe rows) already has a concrete duration in hand.
// The extra bound is strictly safer, not a behavior change for any in-range index.

// Exact frame index -> exact time, refused (std::nullopt) for an invalid rate/duration, an index
// beyond the composition's valid range, or an index whose exact time cannot be represented.
[[nodiscard]] std::optional<core::RationalTime> frameTimeForIndex(document::FrameRate frameRate,
                                                                  core::RationalTime duration,
                                                                  std::uint64_t index) noexcept;

// The greatest non-negative frame index whose mapped time is strictly less than duration, refused
// for an invalid rate/duration or an unrepresentable complete frame range.
[[nodiscard]] std::optional<std::uint64_t> maxFrameIndex(document::FrameRate frameRate,
                                                         core::RationalTime duration) noexcept;

// Clamps time into [0, duration) and selects the nearest frame index, with an exact halfway tie
// going to the greater index; exact rational comparison only, refused for an invalid rate/duration.
[[nodiscard]] std::optional<std::uint64_t>
nearestFrameIndexForTime(document::FrameRate frameRate, core::RationalTime duration,
                         core::RationalTime time) noexcept;

} // namespace bloom::ui
