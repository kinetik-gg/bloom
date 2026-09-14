#include <bloom/core/safe_parse.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <system_error>

namespace {

using bloom::core::kDefaultRadix;
using bloom::core::kMaximumRadix;
using bloom::core::kMinimumRadix;

[[nodiscard]] constexpr bool isAsciiWhitespace(const char character) noexcept {
    return character == ' ' || character == '\t' || character == '\n' || character == '\v' ||
           character == '\f' || character == '\r';
}

[[nodiscard]] constexpr bool isAsciiDigit(const char character) noexcept {
    return character >= '0' && character <= '9';
}

[[nodiscard]] constexpr char toAsciiLower(const char character) noexcept {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                : character;
}

// One digit's value in `radix`, or -1. ASCII only and deliberately so: a Devanagari or fullwidth
// digit is a different character, and folding one into a number here would make the same document
// read differently depending on how its text was typed.
[[nodiscard]] constexpr int digitValue(const char character, const std::int32_t radix) noexcept {
    int value = -1;
    if (isAsciiDigit(character)) {
        value = character - '0';
    } else {
        const char lowered = toAsciiLower(character);
        if (lowered >= 'a' && lowered <= 'z') {
            value = lowered - 'a' + 10;
        }
    }
    return value >= 0 && value < radix ? value : -1;
}

// Whether the trimmed text is EXACTLY the decimal grammar parseScalar() documents. Checked before
// any conversion runs, so the conversion below can be handed a string it is known to accept whole
// -- which is what makes "the whole string is consumed" a property of the grammar rather than of a
// pointer comparison after the fact.
[[nodiscard]] bool isDecimalScalarForm(const std::string_view text) noexcept {
    std::size_t index = 0;
    if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
        ++index;
    }
    std::size_t integerDigits = 0;
    while (index < text.size() && isAsciiDigit(text[index])) {
        ++index;
        ++integerDigits;
    }
    std::size_t fractionDigits = 0;
    if (index < text.size() && text[index] == '.') {
        ++index;
        while (index < text.size() && isAsciiDigit(text[index])) {
            ++index;
            ++fractionDigits;
        }
    }
    if (integerDigits == 0 && fractionDigits == 0) {
        return false;
    }
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
            ++index;
        }
        std::size_t exponentDigits = 0;
        while (index < text.size() && isAsciiDigit(text[index])) {
            ++index;
            ++exponentDigits;
        }
        if (exponentDigits == 0) {
            return false;
        }
    }
    return index == text.size();
}

[[nodiscard]] std::int32_t clampRadix(const std::int32_t radix) noexcept {
    return radix < kMinimumRadix || radix > kMaximumRadix ? kDefaultRadix : radix;
}

[[nodiscard]] std::int32_t clampWidth(const std::int32_t width) noexcept {
    return std::clamp(width, std::int32_t{0}, bloom::core::kMaximumFormattedWidth);
}

// Zero padding with the sign kept leftmost, which is what every `%0*d` an artist has ever typed
// does. Applied to an already-formatted body so one rule serves the scalar and the integer
// formatter alike.
[[nodiscard]] std::string padWithZeros(std::string body, const std::int32_t width) {
    const auto target = static_cast<std::size_t>(clampWidth(width));
    if (body.size() >= target) {
        return body;
    }
    const bool signed_ = !body.empty() && (body.front() == '-' || body.front() == '+');
    const std::size_t offset = signed_ ? 1 : 0;
    body.insert(offset, std::string(target - body.size(), '0'));
    return body;
}

[[nodiscard]] std::uint8_t hexChannel(const double value) noexcept {
    const double clamped = std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0));
}

} // namespace

namespace bloom::core {

std::string_view trimAsciiWhitespace(std::string_view text) noexcept {
    while (!text.empty() && isAsciiWhitespace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isAsciiWhitespace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<double> parseScalar(const std::string_view text) noexcept {
    const auto trimmed = trimAsciiWhitespace(text);
    if (trimmed.empty() || !isDecimalScalarForm(trimmed)) {
        return std::nullopt;
    }
    double value = 0.0;
    // std::from_chars is the one conversion in the standard library that is locale-independent by
    // specification, correctly rounded, and reports its failures through a return value rather than
    // through errno or an exception. `general` is used rather than `fixed` because the grammar
    // above admits an exponent; the grammar, not this flag, is what decides which spellings exist.
    const auto* const first = trimmed.data();
    const auto* const last = first + trimmed.size();
    // A leading '+' is part of the documented grammar and not part of from_chars's, so it is
    // stepped over here and the sign reattached below.
    const bool positiveSign = trimmed.front() == '+';
    const auto result =
        std::from_chars(positiveSign ? first + 1 : first, last, value, std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != last) {
        return std::nullopt;
    }
    // A magnitude outside binary64's representable range -- too large in either direction, or too
    // small to be even a subnormal -- was already refused as std::errc::result_out_of_range above.
    // A SUBNORMAL parses: it is a representable finite answer. This check is the belt to that
    // brace, so no implementation's reading of the range can let a non-finite value through.
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> parseInteger(const std::string_view text,
                                         const std::int32_t radix) noexcept {
    if (radix < kMinimumRadix || radix > kMaximumRadix) {
        return std::nullopt;
    }
    auto trimmed = trimAsciiWhitespace(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    bool negative = false;
    if (trimmed.front() == '+' || trimmed.front() == '-') {
        negative = trimmed.front() == '-';
        trimmed.remove_prefix(1);
    }
    if (trimmed.empty()) {
        return std::nullopt;
    }
    // Accumulated as an unsigned MAGNITUDE against the limit for this sign, so the most negative
    // std::int64_t parses exactly and one past it is refused. Accumulating into a signed integer
    // and negating afterwards would be undefined at exactly that boundary.
    constexpr auto highest = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const std::uint64_t limit = negative ? highest + 1 : highest;
    const auto base = static_cast<std::uint64_t>(radix);
    std::uint64_t magnitude = 0;
    for (const char character : trimmed) {
        const int digit = digitValue(character, radix);
        if (digit < 0) {
            return std::nullopt;
        }
        if (magnitude > (limit - static_cast<std::uint64_t>(digit)) / base) {
            return std::nullopt;
        }
        magnitude = magnitude * base + static_cast<std::uint64_t>(digit);
    }
    if (negative) {
        return magnitude == highest + 1 ? std::numeric_limits<std::int64_t>::min()
                                        : -static_cast<std::int64_t>(magnitude);
    }
    return static_cast<std::int64_t>(magnitude);
}

std::optional<bool> parseBoolean(const std::string_view text) noexcept {
    const auto trimmed = trimAsciiWhitespace(text);
    if (trimmed.empty() || trimmed.size() > 5) {
        return std::nullopt;
    }
    std::array<char, 5> lowered{};
    for (std::size_t index = 0; index < trimmed.size(); ++index) {
        lowered[index] = toAsciiLower(trimmed[index]);
    }
    const std::string_view word{lowered.data(), trimmed.size()};
    if (word == "true" || word == "yes" || word == "on" || word == "1") {
        return true;
    }
    if (word == "false" || word == "no" || word == "off" || word == "0") {
        return false;
    }
    return std::nullopt;
}

std::optional<ParsedColorChannels> parseHexColor(const std::string_view text) noexcept {
    const auto trimmed = trimAsciiWhitespace(text);
    if (trimmed.size() != 7 && trimmed.size() != 9) {
        return std::nullopt;
    }
    if (trimmed.front() != '#') {
        return std::nullopt;
    }
    std::array<double, 4> channels{0.0, 0.0, 0.0, 1.0};
    const std::size_t pairs = (trimmed.size() - 1) / 2;
    for (std::size_t pair = 0; pair < pairs; ++pair) {
        const int high = digitValue(trimmed[1 + pair * 2], 16);
        const int low = digitValue(trimmed[2 + pair * 2], 16);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        channels[pair] = static_cast<double>(high * 16 + low) / 255.0;
    }
    return ParsedColorChannels{channels[0], channels[1], channels[2], channels[3]};
}

std::string formatScalar(const double value, const std::int32_t decimals,
                         const bool trimTrailingZeros, const std::int32_t padWidth) {
    if (!std::isfinite(value)) {
        return {};
    }
    const auto precision = std::clamp(decimals, std::int32_t{0}, kMaximumFormattedDecimals);
    // Wide enough for binary64's longest fixed spelling (309 integer digits) plus a sign, a point
    // and the widest precision this contract admits.
    std::array<char, 512> buffer{};
    const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                       std::chars_format::fixed, precision);
    if (written.ec != std::errc{}) {
        return {};
    }
    std::string body(buffer.data(), static_cast<std::size_t>(written.ptr - buffer.data()));
    if (trimTrailingZeros && body.find('.') != std::string::npos) {
        while (!body.empty() && body.back() == '0') {
            body.pop_back();
        }
        if (!body.empty() && body.back() == '.') {
            body.pop_back();
        }
        // "-" and "" are what stripping leaves of "-0.00" and "0.00"; neither is a number, so the
        // zero they meant is written back explicitly.
        if (body.empty() || body == "-" || body == "+") {
            body = "0";
        }
    }
    return padWithZeros(std::move(body), padWidth);
}

std::string formatInteger(const std::int64_t value, const std::int32_t radix,
                          const std::int32_t padWidth) {
    const auto base = static_cast<std::uint64_t>(clampRadix(radix));
    const bool negative = value < 0;
    // The magnitude as an unsigned value, so the most negative std::int64_t has one rather than
    // overflowing the negation it would otherwise need.
    auto magnitude =
        negative ? ~static_cast<std::uint64_t>(value) + 1 : static_cast<std::uint64_t>(value);
    std::string digits;
    do {
        const auto digit = static_cast<std::size_t>(magnitude % base);
        digits.push_back(static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10));
        magnitude /= base;
    } while (magnitude != 0);
    if (negative) {
        digits.push_back('-');
    }
    std::ranges::reverse(digits);
    return padWithZeros(std::move(digits), padWidth);
}

std::string formatHexColor(const double red, const double green, const double blue,
                           const double alpha, const bool includeAlpha, const bool uppercase) {
    static constexpr std::string_view kLowercase = "0123456789abcdef";
    static constexpr std::string_view kUppercase = "0123456789ABCDEF";
    const auto alphabet = uppercase ? kUppercase : kLowercase;
    std::string text;
    text.reserve(includeAlpha ? 9 : 7);
    text.push_back('#');
    const auto append = [&text, alphabet](const double channel) {
        const auto byte = hexChannel(channel);
        text.push_back(alphabet[static_cast<std::size_t>(byte >> 4U)]);
        text.push_back(alphabet[static_cast<std::size_t>(byte & 15U)]);
    };
    append(red);
    append(green);
    append(blue);
    if (includeAlpha) {
        append(alpha);
    }
    return text;
}

} // namespace bloom::core
