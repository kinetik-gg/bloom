#pragma once

#include <cstdint>
#include <optional>

namespace bloom::output::detail {

inline constexpr std::uint32_t kOutputAnalysisPngMaximumDimensionV1 = 2'147'483'647U;

struct OutputAnalysisPositiveRational32V1 final {
    std::uint32_t numerator;
    std::uint32_t denominator;
};

struct OutputAnalysisNumericWindowV1 final {
    std::uint32_t height;
    std::int64_t originX;
    std::int64_t originY;
    std::uint32_t width;
};

struct OutputAnalysisRoundedBinary32V1 final {
    std::uint32_t bits;
    bool exact;
};

[[nodiscard]] constexpr bool
outputAnalysisInclusiveAxisFitsSigned32V1(const std::int64_t origin,
                                          const std::uint32_t extent) noexcept {
    constexpr std::int64_t minimum = -2'147'483'648LL;
    constexpr std::int64_t maximum = 2'147'483'647LL;
    if (extent == 0 || origin < minimum || origin > maximum) {
        return false;
    }
    return static_cast<std::uint64_t>(extent - 1U) <= static_cast<std::uint64_t>(maximum - origin);
}

[[nodiscard]] constexpr bool
outputAnalysisInclusiveWindowFitsSigned32V1(const OutputAnalysisNumericWindowV1& window) noexcept {
    return outputAnalysisInclusiveAxisFitsSigned32V1(window.originX, window.width) &&
           outputAnalysisInclusiveAxisFitsSigned32V1(window.originY, window.height);
}

// Pixel-aspect terms are positive uint32 values, so their ratio is always a normal binary32. The
// implementation is integer-only and independent of host floating-point mode.
[[nodiscard]] std::optional<OutputAnalysisRoundedBinary32V1>
roundOutputAnalysisPositiveRationalToBinary32V1(OutputAnalysisPositiveRational32V1 value) noexcept;

} // namespace bloom::output::detail
