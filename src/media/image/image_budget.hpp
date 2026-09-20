#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace bloom::media::detail {

// Overflow-safe product for decoder working-set sizes. Returns false instead of wrapping.
[[nodiscard]] constexpr bool checkedSizeProduct(const std::uint64_t a, const std::uint64_t b,
                                                std::uint64_t& result) noexcept {
    if (b != 0 && a > std::numeric_limits<std::uint64_t>::max() / b)
        return false;
    result = a * b;
    return true;
}

// True when final + staging + row fits `pixelBudget` without overflowing. Admission is computed by
// subtraction so an arbitrarily large caller budget or scratch term can never wrap the comparison
// back into the admitted range. The terms are separate so each decoder phase's real scratch is
// accounted for explicitly.
[[nodiscard]] constexpr bool decodeWorkingSetFits(const std::uint64_t finalBytes,
                                                  const std::uint64_t stagingBytes,
                                                  const std::uint64_t rowBytes,
                                                  const std::size_t pixelBudget) noexcept {
    const auto budget = static_cast<std::uint64_t>(pixelBudget);
    if (finalBytes > budget)
        return false;
    const auto afterFinal = budget - finalBytes;
    if (stagingBytes > afterFinal)
        return false;
    return rowBytes <= afterFinal - stagingBytes;
}

} // namespace bloom::media::detail
