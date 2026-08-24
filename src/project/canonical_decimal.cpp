#include <bloom/project/canonical_decimal.hpp>

#include <charconv>
#include <cstdint>
#include <exception>
#include <limits>
#include <numeric>
#include <string_view>

namespace {

using bloom::project::CanonicalDecimalError;
using bloom::project::CanonicalDecimalField;

template <typename Result>
[[nodiscard]] constexpr Result failure(const CanonicalDecimalError error,
                                       const CanonicalDecimalField field) noexcept {
    return Result::failure(error, field);
}

[[nodiscard]] constexpr bool isAsciiDigit(const char character) noexcept {
    return character >= '0' && character <= '9';
}

[[nodiscard]] CanonicalDecimalError
validateUnsignedLexicalForm(const std::string_view text) noexcept {
    if (text.empty()) {
        return CanonicalDecimalError::InvalidLexicalForm;
    }
    if (text.front() == '0') {
        return text.size() == 1 ? CanonicalDecimalError::None
                                : CanonicalDecimalError::InvalidLexicalForm;
    }
    for (const char character : text) {
        if (!isAsciiDigit(character)) {
            return CanonicalDecimalError::InvalidLexicalForm;
        }
    }
    return CanonicalDecimalError::None;
}

[[nodiscard]] CanonicalDecimalError
validateSignedLexicalForm(const std::string_view text) noexcept {
    if (text.empty()) {
        return CanonicalDecimalError::InvalidLexicalForm;
    }

    std::size_t digitOffset = 0;
    if (text.front() == '-') {
        if (text.size() == 1) {
            return CanonicalDecimalError::InvalidLexicalForm;
        }
        digitOffset = 1;
    } else if (text.front() == '+') {
        return CanonicalDecimalError::InvalidLexicalForm;
    }

    if (text[digitOffset] == '0') {
        return text.size() == digitOffset + 1 && digitOffset == 0
                   ? CanonicalDecimalError::None
                   : CanonicalDecimalError::InvalidLexicalForm;
    }
    for (std::size_t index = digitOffset; index < text.size(); ++index) {
        if (!isAsciiDigit(text[index])) {
            return CanonicalDecimalError::InvalidLexicalForm;
        }
    }
    return CanonicalDecimalError::None;
}

[[nodiscard]] bloom::project::CanonicalUInt64Result
parseUnsigned(const std::string_view text, const CanonicalDecimalField field) noexcept {
    const auto lexicalError = validateUnsignedLexicalForm(text);
    if (lexicalError != CanonicalDecimalError::None) {
        return failure<bloom::project::CanonicalUInt64Result>(lexicalError, field);
    }

    std::uint64_t value = 0;
    const auto conversion = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (conversion.ec == std::errc::result_out_of_range) {
        return failure<bloom::project::CanonicalUInt64Result>(CanonicalDecimalError::OutOfRange,
                                                              field);
    }
    if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size()) {
        return failure<bloom::project::CanonicalUInt64Result>(
            CanonicalDecimalError::InvalidLexicalForm, field);
    }
    return bloom::project::CanonicalUInt64Result::success(value);
}

[[nodiscard]] bloom::project::CanonicalInt64Result
parseSigned(const std::string_view text, const CanonicalDecimalField field) noexcept {
    const auto lexicalError = validateSignedLexicalForm(text);
    if (lexicalError != CanonicalDecimalError::None) {
        return failure<bloom::project::CanonicalInt64Result>(lexicalError, field);
    }

    std::int64_t value = 0;
    const auto conversion = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (conversion.ec == std::errc::result_out_of_range) {
        return failure<bloom::project::CanonicalInt64Result>(CanonicalDecimalError::OutOfRange,
                                                             field);
    }
    if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size()) {
        return failure<bloom::project::CanonicalInt64Result>(
            CanonicalDecimalError::InvalidLexicalForm, field);
    }
    return bloom::project::CanonicalInt64Result::success(value);
}

[[nodiscard]] constexpr std::uint64_t magnitude(const std::int64_t value) noexcept {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }
    return static_cast<std::uint64_t>(-(value + 1)) + 1;
}

} // namespace

namespace bloom::project {

CanonicalUInt64Result parseCanonicalObjectId(const std::string_view text) noexcept {
    auto result = parseUnsigned(text, CanonicalDecimalField::Value);
    if (!result) {
        return result;
    }
    if (*result.value() == 0) {
        return CanonicalUInt64Result::failure(CanonicalDecimalError::ZeroNotAllowed,
                                              CanonicalDecimalField::Value);
    }
    return result;
}

CanonicalUInt64Result parseCanonicalAllocatorHighWater(const std::string_view text) noexcept {
    return parseUnsigned(text, CanonicalDecimalField::Value);
}

CanonicalInt64Result parseCanonicalInt64(const std::string_view text) noexcept {
    return parseSigned(text, CanonicalDecimalField::Value);
}

CanonicalUInt32Result parseCanonicalJsonUInt32(const std::string_view text,
                                               const std::uint32_t maximum) noexcept {
    const auto parsed = parseUnsigned(text, CanonicalDecimalField::Value);
    if (!parsed) {
        return CanonicalUInt32Result::failure(parsed.error(), parsed.field());
    }
    if (*parsed.value() > maximum) {
        return CanonicalUInt32Result::failure(CanonicalDecimalError::OutOfRange,
                                              CanonicalDecimalField::Value);
    }
    return CanonicalUInt32Result::success(static_cast<std::uint32_t>(*parsed.value()));
}

CanonicalRationalTimeResult
parseCanonicalRationalTime(const std::string_view numerator,
                           const std::string_view denominator) noexcept {
    const auto parsedNumerator = parseSigned(numerator, CanonicalDecimalField::Numerator);
    if (!parsedNumerator) {
        return CanonicalRationalTimeResult::failure(parsedNumerator.error(),
                                                    parsedNumerator.field());
    }
    const auto parsedDenominator = parseSigned(denominator, CanonicalDecimalField::Denominator);
    if (!parsedDenominator) {
        return CanonicalRationalTimeResult::failure(parsedDenominator.error(),
                                                    parsedDenominator.field());
    }

    const auto numeratorValue = *parsedNumerator.value();
    const auto denominatorValue = *parsedDenominator.value();
    if (denominatorValue == 0) {
        return CanonicalRationalTimeResult::failure(CanonicalDecimalError::ZeroNotAllowed,
                                                    CanonicalDecimalField::Denominator);
    }
    if (denominatorValue < 0) {
        return CanonicalRationalTimeResult::failure(CanonicalDecimalError::OutOfRange,
                                                    CanonicalDecimalField::Denominator);
    }
    if (numeratorValue == 0 && denominatorValue != 1) {
        return CanonicalRationalTimeResult::failure(CanonicalDecimalError::NonCanonicalZero,
                                                    CanonicalDecimalField::Denominator);
    }
    if (std::gcd(magnitude(numeratorValue), static_cast<std::uint64_t>(denominatorValue)) != 1) {
        return CanonicalRationalTimeResult::failure(CanonicalDecimalError::NotReduced,
                                                    CanonicalDecimalField::Value);
    }

    const auto value = core::RationalTime::create(numeratorValue, denominatorValue);
    if (!value.has_value()) {
        return CanonicalRationalTimeResult::failure(CanonicalDecimalError::OutOfRange,
                                                    CanonicalDecimalField::Value);
    }
    return CanonicalRationalTimeResult::success(*value);
}

CanonicalPositiveRatioResult
parseCanonicalPositiveRatio(const std::string_view numerator,
                            const std::string_view denominator) noexcept {
    const auto parsedNumerator = parseSigned(numerator, CanonicalDecimalField::Numerator);
    if (!parsedNumerator) {
        return CanonicalPositiveRatioResult::failure(parsedNumerator.error(),
                                                     parsedNumerator.field());
    }
    const auto parsedDenominator = parseSigned(denominator, CanonicalDecimalField::Denominator);
    if (!parsedDenominator) {
        return CanonicalPositiveRatioResult::failure(parsedDenominator.error(),
                                                     parsedDenominator.field());
    }

    const auto numeratorValue = *parsedNumerator.value();
    const auto denominatorValue = *parsedDenominator.value();
    if (numeratorValue == 0) {
        return CanonicalPositiveRatioResult::failure(CanonicalDecimalError::ZeroNotAllowed,
                                                     CanonicalDecimalField::Numerator);
    }
    if (denominatorValue == 0) {
        return CanonicalPositiveRatioResult::failure(CanonicalDecimalError::ZeroNotAllowed,
                                                     CanonicalDecimalField::Denominator);
    }
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    if (numeratorValue < 0 || static_cast<std::uint64_t>(numeratorValue) > maximum) {
        return CanonicalPositiveRatioResult::failure(CanonicalDecimalError::OutOfRange,
                                                     CanonicalDecimalField::Numerator);
    }
    if (denominatorValue < 0 || static_cast<std::uint64_t>(denominatorValue) > maximum) {
        return CanonicalPositiveRatioResult::failure(CanonicalDecimalError::OutOfRange,
                                                     CanonicalDecimalField::Denominator);
    }

    const auto positiveNumerator = static_cast<std::uint32_t>(numeratorValue);
    const auto positiveDenominator = static_cast<std::uint32_t>(denominatorValue);
    if (std::gcd(positiveNumerator, positiveDenominator) != 1) {
        return CanonicalPositiveRatioResult::failure(CanonicalDecimalError::NotReduced,
                                                     CanonicalDecimalField::Value);
    }
    return CanonicalPositiveRatioResult::success({positiveNumerator, positiveDenominator});
}

CanonicalPixelAspectRatioResult
parseCanonicalPixelAspectRatio(const std::string_view numerator,
                               const std::string_view denominator) noexcept {
    const auto parsed = parseCanonicalPositiveRatio(numerator, denominator);
    if (!parsed) {
        return CanonicalPixelAspectRatioResult::failure(parsed.error(), parsed.field());
    }
    const auto value =
        core::PixelAspectRatio::create(parsed.value()->numerator, parsed.value()->denominator);
    if (!value.has_value()) {
        return CanonicalPixelAspectRatioResult::failure(CanonicalDecimalError::OutOfRange,
                                                        CanonicalDecimalField::Value);
    }
    return CanonicalPixelAspectRatioResult::success(*value);
}

CanonicalDecimalText formatCanonicalUInt64(const std::uint64_t value) noexcept {
    CanonicalDecimalText result;
    const auto conversion =
        std::to_chars(result.characters_.data(),
                      result.characters_.data() + result.characters_.size(), value, 10);
    if (conversion.ec != std::errc{}) {
        std::terminate();
    }
    result.size_ = static_cast<std::uint8_t>(conversion.ptr - result.characters_.data());
    return result;
}

CanonicalDecimalText formatCanonicalInt64(const std::int64_t value) noexcept {
    CanonicalDecimalText result;
    const auto conversion =
        std::to_chars(result.characters_.data(),
                      result.characters_.data() + result.characters_.size(), value, 10);
    if (conversion.ec != std::errc{}) {
        std::terminate();
    }
    result.size_ = static_cast<std::uint8_t>(conversion.ptr - result.characters_.data());
    return result;
}

} // namespace bloom::project
