#pragma once

#include <cmath>

namespace bloom::core {

// Straight/unassociated authoring RGBA. The owning parameter schema defines the RGB encoding;
// Color4d itself deliberately carries no color-space or alpha-association conversion behavior.
struct Color4d {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 1.0;

    [[nodiscard]] bool isValid() const noexcept {
        return std::isfinite(red) && std::isfinite(green) && std::isfinite(blue) &&
               std::isfinite(alpha) && alpha >= 0.0 && alpha <= 1.0;
    }

    friend bool operator==(const Color4d&, const Color4d&) = default;
};

} // namespace bloom::core
