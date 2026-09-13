#pragma once

// Internal to src/ui, not a public header: the helpers more than one composition editor reads off a
// CompositionSession. Split out of composition_editors.cpp (task F1, item F0) so the timeline and
// properties editors own separate translation units; every declaration below names a definition
// that was moved verbatim, never rewritten.

#include <bloom/ui/composition_session.hpp>

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/project.hpp>

#include <QString>

#include <cstdint>
#include <optional>
#include <string_view>

namespace bloom::document {
struct NodeRecord;
} // namespace bloom::document

namespace bloom::ui {

// The layer's authored name, or a "Layer <id>" placeholder when the graph carries none.
[[nodiscard]] QString layerName(const document::Composition& composition,
                                document::LayerId layerId);

// The node feeding `layerId` directly, or nullptr when the layer or its source is unavailable.
[[nodiscard]] const document::NodeRecord* directSourceNode(const CompositionSession& session,
                                                           document::LayerId layerId);

[[nodiscard]] bool isKnownSource(const document::NodeRecord* node, std::string_view typeId,
                                 std::uint32_t schemaVersion);

// Frame stepping / readout (issue #108): the frame rate, duration, and checked maximum frame index
// every stepFrame()/stepToStart()/stepToEnd()/updateTimeReadout() call needs, resolved the one
// place so they cannot silently drift from each other's notion of the composition's valid frame
// range -- mirroring mappingForComposition() in playback_controller.cpp, which this deliberately
// does NOT reuse (it is private to that translation unit and this task's fence forbids touching the
// playback controller beyond the smallest justified accessor -- see stepFrame()'s own comment on
// why none was needed). std::nullopt covers no live composition or a rate/duration
// bloom::core::FrameTimeMapping itself refuses, exactly like every other caller of
// timeline_frame_math.hpp's adapters.
struct TimelineFrameContext final {
    document::FrameRate frameRate;
    core::RationalTime duration;
    std::uint64_t maxFrameIndexValue;
};

[[nodiscard]] std::optional<TimelineFrameContext>
frameContextFor(const CompositionSession& session);

// Formats an exact RationalTime as seconds with EXACTLY 3 truncated decimal digits (millisecond
// resolution), computed purely from the integer numerator/denominator -- never through
// RationalTime::toSeconds()'s binary64 conversion -- so the digits shown are always the value's
// true leading digits, never a rounded/binary64-approximated one (design decision 3: "no
// floating-point accumulation... a subframe time must display honestly"). Three places is a
// deliberately BOUNDED cut of what can be an infinite decimal expansion (e.g. 1 s / 3 has no exact
// finite decimal form); truncating rather than rounding means the displayed digits never overstate
// the exact value. The widening multiply uses a 128-bit intermediate purely so an extreme
// duration's denominator cannot silently overflow a 64-bit product -- unreachable for any realistic
// composition, kept checked rather than UB regardless; this is ordinary display arithmetic, not
// part of the sampling contract's own "no compiler-specific extended integers" rule
// (docs/architecture/animation-and-time.md, "Sampling Semantics Version 1"), which governs curve
// evaluation only.
[[nodiscard]] QString formatExactSeconds(core::RationalTime time);

} // namespace bloom::ui
