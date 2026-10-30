#pragma once

// Strict parsing and bounds for the memory headroom preferences (RAM and VRAM that Bloom leaves
// free for the rest of the computer). Qt-free so the engine's parseHeadroomBytes can replace
// parseHeadroomText in one line.

#include <cstdint>
#include <optional>
#include <string_view>

namespace bloom::ui {

inline constexpr std::uint64_t kHeadroomDefaultBytes = std::uint64_t{2} << 30;
inline constexpr std::uint64_t kHeadroomMaximumBytes = std::uint64_t{256} << 30;

// Decimal digits only (no sign, space, or suffix). Overflow, empty or garbage text is nullopt; a
// value above the maximum clamps to 256 GiB.
[[nodiscard]] constexpr std::optional<std::uint64_t> parseHeadroomText(std::string_view text) {
    if (text.empty())
        return std::nullopt;
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return std::nullopt;
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return std::nullopt;
        value = value * 10 + digit;
    }
    return value > kHeadroomMaximumBytes ? kHeadroomMaximumBytes : value;
}

} // namespace bloom::ui
