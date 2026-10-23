#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/layer_time.hpp>

#include <optional>

// ADR 0024 Timeline bar gestures, as Qt-free math so they are testable without a widget:
//   body drag        -> move:    start, in and out shift together (content and keys move)
//   Alt + body drag  -> slip:    start only (content slides under a fixed range)
//   edge drag        -> trim:    one endpoint
//   Alt + edge drag  -> stretch: speed changes by the exact frame ratio about the opposite edge,
//                                whose content time stays fixed
// Every result is on the composition frame grid (start keeps a subframe offset it already had),
// keeps at least one frame inside [0, duration), and trims may pass a composition bound only as far
// as the content extent reaches there.
namespace bloom::ui {

struct LayerTimingGestureInput final {
    core::RationalTime start;
    core::RationalTime in;
    core::RationalTime out;
    document::LayerSpeed speed;
    document::FrameRate rate;
    core::RationalTime duration;
    // The content extent in placement time, when the layer has one.
    std::optional<core::RationalTime> extentStart;
    std::optional<core::RationalTime> extentEnd;
    int handle = 0; // -1 in edge, 0 body, 1 out edge
    bool alt = false;
    double deltaSeconds = 0.0;
};

struct LayerTimingProposal final {
    core::RationalTime start;
    core::RationalTime in;
    core::RationalTime out;
    document::LayerSpeed speed;
    friend bool operator==(const LayerTimingProposal&, const LayerTimingProposal&) = default;
};

[[nodiscard]] std::optional<LayerTimingProposal>
proposeLayerTiming(const LayerTimingGestureInput& input);

} // namespace bloom::ui
