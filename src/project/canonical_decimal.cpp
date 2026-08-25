#include <bloom/project/canonical_decimal.hpp>

#include <charconv>
#include <cmath>
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

[[nodiscard]] bool validateJsonNumberLexicalForm(const std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }

    std::size_t cursor = 0;
    if (text[cursor] == '-') {
        ++cursor;
        if (cursor == text.size()) {
            return false;
        }
    }

    if (text[cursor] == '0') {
        ++cursor;
        if (cursor < text.size() && isAsciiDigit(text[cursor])) {
            return false;
        }
    } else {
        if (text[cursor] < '1' || text[cursor] > '9') {
            return false;
        }
        do {
            ++cursor;
        } while (cursor < text.size() && isAsciiDigit(text[cursor]));
    }

    if (cursor < text.size() && text[cursor] == '.') {
        ++cursor;
        const auto fractionBegin = cursor;
        while (cursor < text.size() && isAsciiDigit(text[cursor])) {
            ++cursor;
        }
        if (cursor == fractionBegin) {
            return false;
        }
    }

    if (cursor < text.size() && (text[cursor] == 'e' || text[cursor] == 'E')) {
        ++cursor;
        if (cursor < text.size() && (text[cursor] == '+' || text[cursor] == '-')) {
            ++cursor;
        }
        const auto exponentBegin = cursor;
        while (cursor < text.size() && isAsciiDigit(text[cursor])) {
            ++cursor;
        }
        if (cursor == exponentBegin) {
            return false;
        }
    }

    return cursor == text.size();
}

template <std::size_t Capacity>
void appendCharacter(std::array<char, Capacity>& output, std::size_t& size,
                     const char character) noexcept {
    if (size >= output.size()) {
        std::terminate();
    }
    output[size++] = character;
}

template <std::size_t Capacity>
void appendCharacters(std::array<char, Capacity>& output, std::size_t& size,
                      const std::string_view characters) noexcept {
    for (const char character : characters) {
        appendCharacter(output, size, character);
    }
}

template <std::size_t Capacity>
void appendZeroes(std::array<char, Capacity>& output, std::size_t& size,
                  const std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        appendCharacter(output, size, '0');
    }
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

CanonicalFloat64TextResult formatCanonicalFloat64(const double value) noexcept {
    static_assert(std::numeric_limits<double>::is_iec559);
    static_assert(std::numeric_limits<double>::digits == 53);

    if (!std::isfinite(value)) {
        return CanonicalFloat64TextResult::failure(CanonicalDecimalError::NonFinite,
                                                   CanonicalDecimalField::Value);
    }

    CanonicalFloat64Text result;
    std::size_t outputSize = 0;
    if (std::signbit(value)) {
        appendCharacter(result.characters_, outputSize, '-');
    }
    const double magnitudeValue = std::fabs(value);
    if (magnitudeValue == 0.0) {
        appendCharacters(result.characters_, outputSize, "0.0");
        result.size_ = static_cast<std::uint8_t>(outputSize);
        return CanonicalFloat64TextResult::success(result);
    }

    std::array<char, 24> shortest{};
    const auto conversion = std::to_chars(shortest.data(), shortest.data() + shortest.size(),
                                          magnitudeValue, std::chars_format::general);
    if (conversion.ec != std::errc{}) {
        std::terminate();
    }
    const std::string_view shortestView(shortest.data(),
                                        static_cast<std::size_t>(conversion.ptr - shortest.data()));

    std::array<char, 17> digits{};
    std::size_t digitCount = 0;
    std::size_t digitsBeforePoint = 0;
    std::size_t cursor = 0;
    bool sawPoint = false;
    while (cursor < shortestView.size() && shortestView[cursor] != 'e' &&
           shortestView[cursor] != 'E') {
        if (shortestView[cursor] == '.') {
            sawPoint = true;
            digitsBeforePoint = digitCount;
        } else {
            if (digitCount >= digits.size()) {
                std::terminate();
            }
            digits[digitCount++] = shortestView[cursor];
        }
        ++cursor;
    }
    if (!sawPoint) {
        digitsBeforePoint = digitCount;
    }

    int explicitExponent = 0;
    if (cursor < shortestView.size()) {
        ++cursor;
        bool negativeExponent = false;
        if (cursor < shortestView.size() &&
            (shortestView[cursor] == '+' || shortestView[cursor] == '-')) {
            negativeExponent = shortestView[cursor] == '-';
            ++cursor;
        }
        while (cursor < shortestView.size()) {
            explicitExponent = explicitExponent * 10 + (shortestView[cursor] - '0');
            ++cursor;
        }
        if (negativeExponent) {
            explicitExponent = -explicitExponent;
        }
    }

    const int scientificExponent = explicitExponent + static_cast<int>(digitsBeforePoint) - 1;
    if (scientificExponent >= -6 && scientificExponent < 21) {
        const int decimalPoint = scientificExponent + 1;
        if (decimalPoint <= 0) {
            appendCharacters(result.characters_, outputSize, "0.");
            appendZeroes(result.characters_, outputSize, static_cast<std::size_t>(-decimalPoint));
            appendCharacters(result.characters_, outputSize,
                             std::string_view(digits.data(), digitCount));
        } else if (static_cast<std::size_t>(decimalPoint) >= digitCount) {
            appendCharacters(result.characters_, outputSize,
                             std::string_view(digits.data(), digitCount));
            appendZeroes(result.characters_, outputSize,
                         static_cast<std::size_t>(decimalPoint) - digitCount);
            appendCharacters(result.characters_, outputSize, ".0");
        } else {
            appendCharacters(
                result.characters_, outputSize,
                std::string_view(digits.data(), static_cast<std::size_t>(decimalPoint)));
            appendCharacter(result.characters_, outputSize, '.');
            appendCharacters(result.characters_, outputSize,
                             std::string_view(digits.data() + decimalPoint,
                                              digitCount - static_cast<std::size_t>(decimalPoint)));
        }
    } else {
        appendCharacter(result.characters_, outputSize, digits[0]);
        if (digitCount > 1) {
            appendCharacter(result.characters_, outputSize, '.');
            appendCharacters(result.characters_, outputSize,
                             std::string_view(digits.data() + 1, digitCount - 1));
        }
        appendCharacter(result.characters_, outputSize, 'e');
        appendCharacter(result.characters_, outputSize, scientificExponent < 0 ? '-' : '+');

        std::array<char, 3> exponentText{};
        const auto exponentMagnitude = static_cast<unsigned int>(
            scientificExponent < 0 ? -scientificExponent : scientificExponent);
        const auto exponentConversion = std::to_chars(
            exponentText.data(), exponentText.data() + exponentText.size(), exponentMagnitude, 10);
        if (exponentConversion.ec != std::errc{}) {
            std::terminate();
        }
        appendCharacters(
            result.characters_, outputSize,
            std::string_view(exponentText.data(), static_cast<std::size_t>(exponentConversion.ptr -
                                                                           exponentText.data())));
    }

    result.size_ = static_cast<std::uint8_t>(outputSize);
    return CanonicalFloat64TextResult::success(result);
}

CanonicalFloat64Result parseCanonicalFloat64(const std::string_view text) noexcept {
    if (!validateJsonNumberLexicalForm(text)) {
        return CanonicalFloat64Result::failure(CanonicalDecimalError::InvalidLexicalForm,
                                               CanonicalDecimalField::Value);
    }

    double value = 0.0;
    const auto conversion =
        std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (conversion.ec == std::errc::result_out_of_range) {
        return CanonicalFloat64Result::failure(CanonicalDecimalError::OutOfRange,
                                               CanonicalDecimalField::Value);
    }
    if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size()) {
        return CanonicalFloat64Result::failure(CanonicalDecimalError::InvalidLexicalForm,
                                               CanonicalDecimalField::Value);
    }
    if (!std::isfinite(value)) {
        return CanonicalFloat64Result::failure(CanonicalDecimalError::NonFinite,
                                               CanonicalDecimalField::Value);
    }

    const auto canonical = formatCanonicalFloat64(value);
    if (!canonical || canonical.value()->view() != text) {
        return CanonicalFloat64Result::failure(CanonicalDecimalError::NonCanonical,
                                               CanonicalDecimalField::Value);
    }
    return CanonicalFloat64Result::success(value);
}

} // namespace bloom::project
