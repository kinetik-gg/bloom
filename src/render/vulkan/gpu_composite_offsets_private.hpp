#pragma once

#include <cstdint>
#include <limits>

namespace bloom::render {

// The source-over shader adds a signed offset to every destination-local coordinate. Verify
// both the exact int64 origin subtraction and that entire int32 addition domain before narrowing.
[[nodiscard]] inline bool compositeSourceOffset(const std::int64_t destinationOrigin,
                                                const std::int64_t sourceOrigin,
                                                const std::uint32_t destinationExtent,
                                                std::int32_t& offset) noexcept {
    constexpr auto min64 = std::numeric_limits<std::int64_t>::min();
    constexpr auto max64 = std::numeric_limits<std::int64_t>::max();
    constexpr auto min32 = std::numeric_limits<std::int32_t>::min();
    constexpr auto max32 = std::numeric_limits<std::int32_t>::max();
    if (destinationExtent == 0 || destinationExtent > static_cast<std::uint32_t>(max32) ||
        (sourceOrigin > 0 && destinationOrigin < min64 + sourceOrigin) ||
        (sourceOrigin < 0 && destinationOrigin > max64 + sourceOrigin)) {
        return false;
    }
    const auto delta = destinationOrigin - sourceOrigin;
    const auto lastLocal = static_cast<std::int64_t>(destinationExtent) - 1;
    if (delta < min32 || delta > static_cast<std::int64_t>(max32) - lastLocal) {
        return false;
    }
    offset = static_cast<std::int32_t>(delta);
    return true;
}

} // namespace bloom::render
