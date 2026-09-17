#pragma once

#include <bloom/document/animation.hpp>

#include <array>
#include <type_traits>

namespace bloom::ui {

// The component names of one vector or colour curve kind, in the curve's own component order.
//
// Every UI reader of an animated vector or colour parameter needs this mapping, because since
// KEY-2 a key IS a component key: `components[i]` and the AnimationComponent that names it are two
// halves of one address, and a reader that guessed either half would be inventing a second answer
// to "which axis is this key on". One definition, so a lane, a graph curve, a paste and a
// selection cannot disagree.
template <typename Curve> [[nodiscard]] constexpr auto animationComponentsOf() noexcept {
    if constexpr (std::is_same_v<Curve, document::Vec2AnimationCurve>) {
        return std::array{document::AnimationComponent::X, document::AnimationComponent::Y};
    } else if constexpr (std::is_same_v<Curve, document::Vec3AnimationCurve>) {
        return std::array{document::AnimationComponent::X, document::AnimationComponent::Y,
                          document::AnimationComponent::Z};
    } else {
        return std::array{document::AnimationComponent::Red, document::AnimationComponent::Green,
                          document::AnimationComponent::Blue, document::AnimationComponent::Alpha};
    }
}

} // namespace bloom::ui
