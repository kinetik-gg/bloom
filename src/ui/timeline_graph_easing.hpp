#pragma once

// The graph editor's Easing presets (T1, item 9), in menu order: Linear and Constant, then
// Blender's easing functions (document 1.30's evaluated equations), each as Ease In / Ease Out /
// Ease In-Out. This table is the ONE place the menu reads them from.

#include <bloom/document/animation.hpp>

#include <array>
#include <optional>
#include <string_view>

namespace bloom::ui {

// One Blender easing function: the interpolation that evaluates it. Every family is offered as
// Ease In / Ease Out / Ease In-Out, the document's KeyframeEasing written on the same keys.
struct GraphEasingFamily final {
    std::string_view name;
    document::KeyframeInterpolation interpolation;
};

// Linear and Constant are single presets.
inline constexpr std::array<std::pair<std::string_view, document::KeyframeInterpolation>, 2>
    kGraphEasingBasics{{{"Linear", document::KeyframeInterpolation::Linear},
                        {"Constant", document::KeyframeInterpolation::Hold}}};

inline constexpr std::array<GraphEasingFamily, 10> kGraphEasingFamilies{{
    {"Sine", document::KeyframeInterpolation::Sine},
    {"Quad", document::KeyframeInterpolation::Quad},
    {"Cubic", document::KeyframeInterpolation::Cubic},
    {"Quart", document::KeyframeInterpolation::Quart},
    {"Quint", document::KeyframeInterpolation::Quint},
    {"Expo", document::KeyframeInterpolation::Expo},
    {"Circ", document::KeyframeInterpolation::Circ},
    {"Back", document::KeyframeInterpolation::Back},
    {"Bounce", document::KeyframeInterpolation::Bounce},
    {"Elastic", document::KeyframeInterpolation::Elastic},
}};

inline constexpr std::array<std::pair<std::string_view, document::KeyframeEasing>, 3>
    kGraphEasingModes{{{"Ease In", document::KeyframeEasing::In},
                       {"Ease Out", document::KeyframeEasing::Out},
                       {"Ease In-Out", document::KeyframeEasing::InOut}}};

// Handles > Auto / Aligned / Free (item 5) need a persisted per-key handle mode the document does
// not carry yet (handoff-T1.md). The submenu is not offered until it does.
inline constexpr bool kGraphHandleModesAvailable = false;
inline constexpr std::array<std::string_view, 3> kGraphHandleModeNames{"Auto", "Aligned", "Free"};

} // namespace bloom::ui
