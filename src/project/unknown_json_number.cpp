#include <bloom/project/unknown_json_number.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <exception>

namespace bloom::project {

UnknownJsonNumberResult parseUnknownJsonNumber(const std::string_view text) noexcept {
    const auto integer = parseCanonicalInt64(text);
    if (integer) {
        return UnknownJsonNumberResult::success(UnknownJsonNumber(
            UnknownJsonNumberKind::Integer, std::bit_cast<std::uint64_t>(*integer.value())));
    }

    // A canonical integer token is always an integer, including when its value is outside int64.
    // Only a lexical mismatch permits classification as the distinct canonical Float64 form.
    if (integer.error() != CanonicalDecimalError::InvalidLexicalForm) {
        return UnknownJsonNumberResult::failure(integer.error(), integer.field());
    }

    const auto float64 = parseCanonicalFloat64(text);
    if (!float64) {
        return UnknownJsonNumberResult::failure(float64.error(), float64.field());
    }
    return UnknownJsonNumberResult::success(UnknownJsonNumber(
        UnknownJsonNumberKind::Float64, std::bit_cast<std::uint64_t>(*float64.value())));
}

UnknownJsonNumberText formatUnknownJsonNumber(const UnknownJsonNumber& value) noexcept {
    UnknownJsonNumberText result;
    std::string_view canonical;

    const auto integer = value.integerValue();
    if (integer.has_value()) {
        const auto text = formatCanonicalInt64(*integer);
        canonical = text.view();
        std::copy(canonical.begin(), canonical.end(), result.characters_.begin());
        result.size_ = static_cast<std::uint8_t>(canonical.size());
        return result;
    }

    const auto bits = value.float64Bits();
    if (!bits.has_value()) {
        std::terminate();
    }
    const auto text = formatCanonicalFloat64(std::bit_cast<double>(*bits));
    if (!text) {
        std::terminate();
    }
    canonical = text.value()->view();
    std::copy(canonical.begin(), canonical.end(), result.characters_.begin());
    result.size_ = static_cast<std::uint8_t>(canonical.size());
    return result;
}

} // namespace bloom::project
