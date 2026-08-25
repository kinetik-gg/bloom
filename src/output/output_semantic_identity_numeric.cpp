#include "output_semantic_identity_internal.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>

namespace bloom::output::detail {

namespace {

[[nodiscard]] constexpr std::optional<std::uint64_t>
multiplyChecked(const std::uint64_t left, const std::uint64_t right) noexcept {
    if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right) {
        return std::nullopt;
    }
    return left * right;
}

} // namespace

bool validOutputSemanticExrPixelBitsV1(
    const std::span<const std::uint32_t, 4> components) noexcept {
    const auto red = std::bit_cast<float>(components[0]);
    const auto green = std::bit_cast<float>(components[1]);
    const auto blue = std::bit_cast<float>(components[2]);
    const auto alpha = std::bit_cast<float>(components[3]);
    if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue) ||
        !std::isfinite(alpha) || alpha < 0.0F || alpha > 1.0F) {
        return false;
    }
    return alpha != 0.0F ||
           std::ranges::all_of(components, [](const std::uint32_t bits) { return bits == 0U; });
}

std::optional<std::uint64_t>
flatExrInclusiveWindowPixelCountV1(const FlatExrInclusiveWindowV1& window) noexcept {
    const auto width = static_cast<std::int64_t>(window.xMax) - window.xMin + 1;
    const auto height = static_cast<std::int64_t>(window.yMax) - window.yMin + 1;
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    return multiplyChecked(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height));
}

} // namespace bloom::output::detail
