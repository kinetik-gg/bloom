#include <bloom/output/output_facet_descriptor.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string_view>

namespace {

using bloom::output::OutputFacetDescriptorErrorCode;
using bloom::output::OutputFacetDescriptorValidation;
using bloom::output::OutputFacetDescriptorValueTagV1;
using bloom::output::OutputFacetDescriptorValueValidation;

using ValueTag = OutputFacetDescriptorValueTagV1;

enum class NumericDomain : std::uint8_t { None, Unsigned32, Signed64 };

struct FieldRule final {
    std::string_view key;
    ValueTag tag;
    NumericDomain numericDomain = NumericDomain::None;
};

struct UnsignedDecimalResult final {
    std::uint64_t value = 0;
    OutputFacetDescriptorErrorCode error = OutputFacetDescriptorErrorCode::None;
    std::size_t relativeOffset = 0;
};

struct SignedDecimalResult final {
    OutputFacetDescriptorErrorCode error = OutputFacetDescriptorErrorCode::None;
    std::size_t relativeOffset = 0;
};

[[nodiscard]] constexpr bool isAsciiLower(const char value) noexcept {
    return value >= 'a' && value <= 'z';
}

[[nodiscard]] constexpr bool isAsciiDigit(const char value) noexcept {
    return value >= '0' && value <= '9';
}

[[nodiscard]] constexpr bool isLowerHex(const char value) noexcept {
    return isAsciiDigit(value) || (value >= 'a' && value <= 'f');
}

[[nodiscard]] constexpr std::uint8_t hexValue(const char value) noexcept {
    return isAsciiDigit(value) ? static_cast<std::uint8_t>(value - '0')
                               : static_cast<std::uint8_t>(value - 'a' + 10);
}

[[nodiscard]] constexpr bool isIdentifierByte(const char value) noexcept {
    return (value >= 'A' && value <= 'Z') || isAsciiLower(value) || isAsciiDigit(value) ||
           value == '.' || value == '_' || value == ':' || value == '/' || value == '-';
}

[[nodiscard]] std::size_t invalidKeyByte(const std::string_view key) noexcept {
    if (key.empty() || !isAsciiLower(key.front())) {
        return 0;
    }
    for (std::size_t index = 1; index < key.size(); ++index) {
        const auto value = key[index];
        if (!isAsciiLower(value) && !isAsciiDigit(value) && value != '-') {
            return index;
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] UnsignedDecimalResult parseUnsignedDecimal(
    const std::string_view text,
    const std::uint64_t limit = std::numeric_limits<std::uint64_t>::max()) noexcept {
    if (text.empty()) {
        return {.error = OutputFacetDescriptorErrorCode::InvalidUnsignedDecimal};
    }
    if (text.front() == '0' && text.size() != 1) {
        return {.error = OutputFacetDescriptorErrorCode::InvalidUnsignedDecimal,
                .relativeOffset = 1};
    }
    if (!isAsciiDigit(text.front())) {
        return {.error = OutputFacetDescriptorErrorCode::InvalidUnsignedDecimal};
    }

    for (std::size_t index = 0; index < text.size(); ++index) {
        if (!isAsciiDigit(text[index])) {
            return {.error = OutputFacetDescriptorErrorCode::InvalidUnsignedDecimal,
                    .relativeOffset = index};
        }
    }

    std::uint64_t value = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto digit = static_cast<std::uint64_t>(text[index] - '0');
        if (value > (limit - digit) / 10U) {
            return {.error = OutputFacetDescriptorErrorCode::UnsignedDecimalOutOfRange,
                    .relativeOffset = index};
        }
        value = value * 10U + digit;
    }
    return {.value = value};
}

[[nodiscard]] SignedDecimalResult parseSignedDecimal(const std::string_view text) noexcept {
    if (text.empty()) {
        return {.error = OutputFacetDescriptorErrorCode::InvalidSignedDecimal};
    }

    const bool negative = text.front() == '-';
    const std::size_t digitsOffset = negative ? 1 : 0;
    if (digitsOffset == text.size()) {
        return {.error = OutputFacetDescriptorErrorCode::InvalidSignedDecimal,
                .relativeOffset = digitsOffset};
    }
    const auto digits = text.substr(digitsOffset);
    if (digits.front() == '0') {
        if (digits.size() != 1 || negative) {
            return {.error = OutputFacetDescriptorErrorCode::InvalidSignedDecimal,
                    .relativeOffset = digitsOffset + (digits.size() == 1 ? 0 : 1)};
        }
        return {};
    }
    if (digits.front() < '1' || digits.front() > '9') {
        return {.error = OutputFacetDescriptorErrorCode::InvalidSignedDecimal,
                .relativeOffset = digitsOffset};
    }

    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (!isAsciiDigit(digits[index])) {
            return {.error = OutputFacetDescriptorErrorCode::InvalidSignedDecimal,
                    .relativeOffset = digitsOffset + index};
        }
    }

    constexpr auto positiveLimit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    constexpr auto negativeLimit = positiveLimit + 1U;
    const auto limit = negative ? negativeLimit : positiveLimit;
    std::uint64_t magnitude = 0;
    for (std::size_t index = 0; index < digits.size(); ++index) {
        const auto digit = static_cast<std::uint64_t>(digits[index] - '0');
        if (magnitude > (limit - digit) / 10U) {
            return {.error = OutputFacetDescriptorErrorCode::SignedDecimalOutOfRange,
                    .relativeOffset = digitsOffset + index};
        }
        magnitude = magnitude * 10U + digit;
    }
    return {};
}

[[nodiscard]] OutputFacetDescriptorValidation failure(const OutputFacetDescriptorErrorCode error,
                                                      const std::size_t offset) noexcept {
    return OutputFacetDescriptorValidation::failure(error, offset);
}

[[nodiscard]] OutputFacetDescriptorValidation
validateUnsignedValue(const std::string_view payload, const std::size_t payloadOffset) noexcept {
    const auto result = parseUnsignedDecimal(payload);
    if (result.error != OutputFacetDescriptorErrorCode::None &&
        result.error != OutputFacetDescriptorErrorCode::UnsignedDecimalOutOfRange) {
        return failure(result.error, payloadOffset + result.relativeOffset);
    }
    return OutputFacetDescriptorValidation::success();
}

[[nodiscard]] OutputFacetDescriptorValidation
validateSignedValue(const std::string_view payload, const std::size_t payloadOffset) noexcept {
    const auto result = parseSignedDecimal(payload);
    if (result.error != OutputFacetDescriptorErrorCode::None &&
        result.error != OutputFacetDescriptorErrorCode::SignedDecimalOutOfRange) {
        return failure(result.error, payloadOffset + result.relativeOffset);
    }
    return OutputFacetDescriptorValidation::success();
}

[[nodiscard]] OutputFacetDescriptorValidation
validateRationalValue(const std::string_view payload, const std::size_t payloadOffset) noexcept {
    const auto separator = payload.find('/');
    if (separator == std::string_view::npos) {
        return failure(OutputFacetDescriptorErrorCode::InvalidRational,
                       payloadOffset + payload.size());
    }
    if (separator == 0) {
        return failure(OutputFacetDescriptorErrorCode::InvalidRational, payloadOffset);
    }
    if (separator + 1 == payload.size()) {
        return failure(OutputFacetDescriptorErrorCode::InvalidRational,
                       payloadOffset + payload.size());
    }
    if (const auto secondSeparator = payload.find('/', separator + 1);
        secondSeparator != std::string_view::npos) {
        return failure(OutputFacetDescriptorErrorCode::InvalidRational,
                       payloadOffset + secondSeparator);
    }

    const auto numeratorText = payload.substr(0, separator);
    const auto denominatorText = payload.substr(separator + 1);
    const auto numerator = parseSignedDecimal(numeratorText);
    if (numerator.error != OutputFacetDescriptorErrorCode::None &&
        numerator.error != OutputFacetDescriptorErrorCode::SignedDecimalOutOfRange) {
        return failure(OutputFacetDescriptorErrorCode::InvalidRational,
                       payloadOffset + numerator.relativeOffset);
    }
    const auto denominator = parseUnsignedDecimal(denominatorText);
    if (denominator.error != OutputFacetDescriptorErrorCode::None &&
        denominator.error != OutputFacetDescriptorErrorCode::UnsignedDecimalOutOfRange) {
        return failure(OutputFacetDescriptorErrorCode::InvalidRational,
                       payloadOffset + separator + 1 + denominator.relativeOffset);
    }

    const auto numeratorMagnitudeText =
        numeratorText.starts_with('-') ? numeratorText.substr(1) : numeratorText;
    const auto numeratorMagnitude = parseUnsignedDecimal(numeratorMagnitudeText);
    if (denominatorText == "0") {
        return failure(OutputFacetDescriptorErrorCode::InvalidRational,
                       payloadOffset + separator + 1);
    }
    if (numeratorMagnitudeText == "0") {
        return denominatorText == "1"
                   ? OutputFacetDescriptorValidation::success()
                   : failure(OutputFacetDescriptorErrorCode::NonNormalizedRational, payloadOffset);
    }
    if (numeratorMagnitudeText == "1" || denominatorText == "1") {
        return OutputFacetDescriptorValidation::success();
    }
    if (numeratorMagnitude.error == OutputFacetDescriptorErrorCode::UnsignedDecimalOutOfRange ||
        denominator.error == OutputFacetDescriptorErrorCode::UnsignedDecimalOutOfRange) {
        return failure(OutputFacetDescriptorErrorCode::NumericProofUnavailable, payloadOffset);
    }
    if (std::gcd(numeratorMagnitude.value, denominator.value) != 1) {
        return failure(OutputFacetDescriptorErrorCode::NonNormalizedRational, payloadOffset);
    }
    return OutputFacetDescriptorValidation::success();
}

[[nodiscard]] OutputFacetDescriptorValidation
validateFloatBits(const std::string_view payload, const std::size_t payloadOffset,
                  const std::size_t requiredDigits) noexcept {
    const auto presentDigits = std::min(payload.size(), requiredDigits);
    for (std::size_t index = 0; index < presentDigits; ++index) {
        if (!isLowerHex(payload[index])) {
            return failure(OutputFacetDescriptorErrorCode::InvalidFloatBits, payloadOffset + index);
        }
    }
    if (payload.size() != requiredDigits) {
        return failure(OutputFacetDescriptorErrorCode::InvalidFloatBits,
                       payloadOffset + presentDigits);
    }
    return OutputFacetDescriptorValidation::success();
}

[[nodiscard]] OutputFacetDescriptorValidation
validateIdentifier(const std::string_view payload, const std::size_t payloadOffset) noexcept {
    if (payload.empty()) {
        return failure(OutputFacetDescriptorErrorCode::InvalidIdentifier, payloadOffset);
    }
    for (std::size_t index = 0; index < payload.size(); ++index) {
        if (!isIdentifierByte(payload[index])) {
            return failure(OutputFacetDescriptorErrorCode::InvalidIdentifier,
                           payloadOffset + index);
        }
    }
    return OutputFacetDescriptorValidation::success();
}

[[nodiscard]] OutputFacetDescriptorValidation
validateUtf8Hex(const std::string_view payload, const std::size_t payloadOffset) noexcept {
    for (std::size_t index = 0; index < payload.size(); ++index) {
        if (!isLowerHex(payload[index])) {
            return failure(OutputFacetDescriptorErrorCode::InvalidUtf8Hex, payloadOffset + index);
        }
    }
    if ((payload.size() % 2U) != 0U) {
        return failure(OutputFacetDescriptorErrorCode::InvalidUtf8Hex,
                       payloadOffset + payload.size());
    }

    const auto decodedSize = payload.size() / 2U;
    const auto byteAt = [&payload](const std::size_t index) noexcept {
        const auto hexOffset = index * 2U;
        return static_cast<std::uint8_t>((hexValue(payload[hexOffset]) << 4U) |
                                         hexValue(payload[hexOffset + 1]));
    };
    const auto decodedOffset = [payloadOffset](const std::size_t index) noexcept {
        return payloadOffset + index * 2U;
    };
    const auto isContinuation = [](const std::uint8_t value) noexcept {
        return (value & 0xC0U) == 0x80U;
    };

    std::size_t firstNonAscii = decodedSize;
    std::size_t index = 0;
    while (index < decodedSize) {
        const auto lead = byteAt(index);
        if (lead == 0) {
            return failure(OutputFacetDescriptorErrorCode::EmbeddedNul, decodedOffset(index));
        }
        if (lead <= 0x7FU) {
            ++index;
            continue;
        }
        if (firstNonAscii == decodedSize) {
            firstNonAscii = index;
        }

        std::size_t sequenceBytes = 0;
        if (lead >= 0xC2U && lead <= 0xDFU) {
            sequenceBytes = 2;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            sequenceBytes = 3;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            sequenceBytes = 4;
        } else {
            return failure(OutputFacetDescriptorErrorCode::InvalidUtf8, decodedOffset(index));
        }
        if (sequenceBytes > decodedSize - index) {
            return failure(OutputFacetDescriptorErrorCode::InvalidUtf8,
                           payloadOffset + payload.size());
        }
        for (std::size_t continuation = 1; continuation < sequenceBytes; ++continuation) {
            if (!isContinuation(byteAt(index + continuation))) {
                return failure(OutputFacetDescriptorErrorCode::InvalidUtf8,
                               decodedOffset(index + continuation));
            }
        }
        const auto second = byteAt(index + 1);
        if ((lead == 0xE0U && second < 0xA0U) || (lead == 0xEDU && second >= 0xA0U) ||
            (lead == 0xF0U && second < 0x90U) || (lead == 0xF4U && second >= 0x90U)) {
            return failure(OutputFacetDescriptorErrorCode::InvalidUtf8, decodedOffset(index + 1));
        }
        index += sequenceBytes;
    }

    if (firstNonAscii != decodedSize) {
        return failure(OutputFacetDescriptorErrorCode::NormalizationUnavailable,
                       decodedOffset(firstNonAscii));
    }
    return OutputFacetDescriptorValidation::success();
}

[[nodiscard]] std::string_view tagPrefix(const ValueTag tag) noexcept {
    switch (tag) {
    case ValueTag::Boolean:
        return "b:";
    case ValueTag::SignedInteger:
        return "i:";
    case ValueTag::UnsignedInteger:
        return "u:";
    case ValueTag::Rational:
        return "r:";
    case ValueTag::Float32:
        return "f32:";
    case ValueTag::Float64:
        return "f64:";
    case ValueTag::Identifier:
        return "id:";
    case ValueTag::Utf8:
        return "utf8:";
    }
    return {};
}

[[nodiscard]] OutputFacetDescriptorValueValidation
valueFailure(const OutputFacetDescriptorErrorCode error, const std::size_t offset) noexcept {
    return OutputFacetDescriptorValueValidation::failure(error, offset);
}

[[nodiscard]] OutputFacetDescriptorValueValidation
validateTaggedValue(const std::string_view encoded) noexcept {
    constexpr std::array tags{
        ValueTag::Boolean, ValueTag::SignedInteger, ValueTag::UnsignedInteger, ValueTag::Rational,
        ValueTag::Float32, ValueTag::Float64,       ValueTag::Identifier,      ValueTag::Utf8};
    ValueTag tag{};
    bool found = false;
    for (const auto candidate : tags) {
        if (encoded.starts_with(tagPrefix(candidate))) {
            tag = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        return valueFailure(OutputFacetDescriptorErrorCode::InvalidValueTag, 0);
    }

    const auto prefix = tagPrefix(tag);
    const auto payload = encoded.substr(prefix.size());
    OutputFacetDescriptorValidation result = OutputFacetDescriptorValidation::success();
    switch (tag) {
    case ValueTag::Boolean:
        if (payload.empty() || (payload.front() != '0' && payload.front() != '1')) {
            result = failure(OutputFacetDescriptorErrorCode::InvalidBoolean, prefix.size());
        } else if (payload.size() != 1) {
            result = failure(OutputFacetDescriptorErrorCode::InvalidBoolean, prefix.size() + 1);
        }
        break;
    case ValueTag::SignedInteger:
        result = validateSignedValue(payload, prefix.size());
        break;
    case ValueTag::UnsignedInteger:
        result = validateUnsignedValue(payload, prefix.size());
        break;
    case ValueTag::Rational:
        result = validateRationalValue(payload, prefix.size());
        break;
    case ValueTag::Float32:
        result = validateFloatBits(payload, prefix.size(), 8);
        break;
    case ValueTag::Float64:
        result = validateFloatBits(payload, prefix.size(), 16);
        break;
    case ValueTag::Identifier:
        result = validateIdentifier(payload, prefix.size());
        break;
    case ValueTag::Utf8:
        result = validateUtf8Hex(payload, prefix.size());
        break;
    }
    return result ? OutputFacetDescriptorValueValidation::success(tag)
                  : valueFailure(result.error(), result.errorOffset());
}

[[nodiscard]] OutputFacetDescriptorValidation
validateValue(const std::string_view encoded, const FieldRule& rule,
              const std::size_t valueOffset) noexcept {
    const auto generic = validateTaggedValue(encoded);
    if (!generic) {
        return failure(generic.error(), valueOffset + generic.errorOffset());
    }
    if (generic.valueTag() != rule.tag) {
        return failure(OutputFacetDescriptorErrorCode::InvalidValueTag, valueOffset);
    }

    const auto prefixSize = tagPrefix(rule.tag).size();
    const auto payload = encoded.substr(prefixSize);
    switch (rule.numericDomain) {
    case NumericDomain::None:
        return OutputFacetDescriptorValidation::success();
    case NumericDomain::Unsigned32: {
        const auto parsed =
            parseUnsignedDecimal(payload, std::numeric_limits<std::uint32_t>::max());
        return parsed.error == OutputFacetDescriptorErrorCode::None
                   ? OutputFacetDescriptorValidation::success()
                   : failure(parsed.error, valueOffset + prefixSize + parsed.relativeOffset);
    }
    case NumericDomain::Signed64: {
        const auto parsed = parseSignedDecimal(payload);
        return parsed.error == OutputFacetDescriptorErrorCode::None
                   ? OutputFacetDescriptorValidation::success()
                   : failure(parsed.error, valueOffset + prefixSize + parsed.relativeOffset);
    }
    }
    return failure(OutputFacetDescriptorErrorCode::InternalInvariant, valueOffset);
}

[[nodiscard]] constexpr std::array<FieldRule, 4> pixelsRules() noexcept {
    return {{{"height", ValueTag::UnsignedInteger, NumericDomain::Unsigned32},
             {"packing", ValueTag::Identifier},
             {"sample-type", ValueTag::Identifier},
             {"width", ValueTag::UnsignedInteger, NumericDomain::Unsigned32}}};
}

[[nodiscard]] constexpr std::array<FieldRule, 1> precisionRules() noexcept {
    return {{{"component-type", ValueTag::Identifier}}};
}

[[nodiscard]] constexpr std::array<FieldRule, 1> colorRules() noexcept {
    return {{{"color-id", ValueTag::Identifier}}};
}

[[nodiscard]] constexpr std::array<FieldRule, 2> alphaRules() noexcept {
    return {{{"association", ValueTag::Identifier}, {"zero-alpha", ValueTag::Identifier}}};
}

[[nodiscard]] constexpr std::array<FieldRule, 4> windowRules() noexcept {
    return {{{"height", ValueTag::UnsignedInteger, NumericDomain::Unsigned32},
             {"origin-x", ValueTag::SignedInteger, NumericDomain::Signed64},
             {"origin-y", ValueTag::SignedInteger, NumericDomain::Signed64},
             {"width", ValueTag::UnsignedInteger, NumericDomain::Unsigned32}}};
}

[[nodiscard]] constexpr std::array<FieldRule, 2> rationalPixelAspectRules() noexcept {
    return {{{"denominator", ValueTag::UnsignedInteger, NumericDomain::Unsigned32},
             {"numerator", ValueTag::UnsignedInteger, NumericDomain::Unsigned32}}};
}

[[nodiscard]] std::uint64_t nextLexicalIndex(const std::uint64_t current,
                                             const std::uint64_t count) noexcept {
    if (current == 0) {
        return 1;
    }
    if (current <= (count - 1U) / 10U) {
        return current * 10U;
    }

    auto ancestor = current;
    while ((ancestor % 10U) == 9U || ancestor >= count - 1U) {
        ancestor /= 10U;
    }
    return ancestor + 1U;
}

[[nodiscard]] constexpr std::array<FieldRule, 1> binary32PixelAspectRules() noexcept {
    return {{{"value", ValueTag::Float32}}};
}

[[nodiscard]] constexpr std::array<FieldRule, 1> compressionRules() noexcept {
    return {{{"method", ValueTag::Identifier}}};
}

[[nodiscard]] constexpr std::array<FieldRule, 1> metadataRules() noexcept {
    return {{{"profile", ValueTag::Identifier}}};
}

[[nodiscard]] constexpr std::array<FieldRule, 2> externalDependencyRules() noexcept {
    return {{{"kind", ValueTag::Identifier}, {"revision", ValueTag::Identifier}}};
}

struct ParsedField final {
    std::string_view key;
    std::string_view value;
    std::size_t fieldOffset = 0;
    std::size_t valueOffset = 0;
    std::size_t nextOffset = 0;
};

[[nodiscard]] OutputFacetDescriptorValidation parseField(const std::string_view descriptor,
                                                         const std::size_t offset,
                                                         ParsedField& field) noexcept {
    const auto separator = descriptor.find(';', offset);
    const auto fieldEnd = separator == std::string_view::npos ? descriptor.size() : separator;
    if (fieldEnd == offset) {
        return failure(OutputFacetDescriptorErrorCode::EmptyField, offset);
    }
    const auto fieldText = descriptor.substr(offset, fieldEnd - offset);
    const auto equals = fieldText.find('=');
    if (equals == std::string_view::npos) {
        return failure(OutputFacetDescriptorErrorCode::MissingEquals, fieldEnd);
    }
    if (equals == 0) {
        return failure(OutputFacetDescriptorErrorCode::InvalidKey, offset);
    }

    field = {.key = fieldText.substr(0, equals),
             .value = fieldText.substr(equals + 1),
             .fieldOffset = offset,
             .valueOffset = offset + equals + 1,
             .nextOffset = separator == std::string_view::npos ? descriptor.size() : separator + 1};
    if (const auto invalidByte = invalidKeyByte(field.key); invalidByte != std::string_view::npos) {
        return failure(OutputFacetDescriptorErrorCode::InvalidKey, offset + invalidByte);
    }
    return OutputFacetDescriptorValidation::success();
}

[[nodiscard]] OutputFacetDescriptorValidation validateOrder(const std::string_view previousKey,
                                                            const ParsedField& field) noexcept {
    if (previousKey.empty()) {
        return OutputFacetDescriptorValidation::success();
    }
    const auto comparison = field.key.compare(previousKey);
    if (comparison == 0) {
        return failure(OutputFacetDescriptorErrorCode::DuplicateKey, field.fieldOffset);
    }
    if (comparison < 0) {
        return failure(OutputFacetDescriptorErrorCode::OutOfOrderKey, field.fieldOffset);
    }
    return OutputFacetDescriptorValidation::success();
}

template <std::size_t Size>
[[nodiscard]] OutputFacetDescriptorValidation
validateFixedSchema(const std::string_view descriptor,
                    const std::array<FieldRule, Size>& rules) noexcept {
    static_assert(Size <= 64);
    std::uint64_t seen = 0;
    std::string_view previousKey;
    std::size_t offset = 0;
    while (offset < descriptor.size()) {
        ParsedField field;
        if (const auto parsed = parseField(descriptor, offset, field); !parsed) {
            return parsed;
        }
        if (const auto ordered = validateOrder(previousKey, field); !ordered) {
            return ordered;
        }

        std::size_t ruleIndex = Size;
        for (std::size_t index = 0; index < Size; ++index) {
            if (rules[index].key == field.key) {
                ruleIndex = index;
                break;
            }
        }
        if (ruleIndex == Size) {
            return failure(OutputFacetDescriptorErrorCode::UnknownKey, field.fieldOffset);
        }
        const auto bit = std::uint64_t{1} << ruleIndex;
        if ((seen & bit) != 0) {
            return failure(OutputFacetDescriptorErrorCode::DuplicateKey, field.fieldOffset);
        }
        if (const auto value = validateValue(field.value, rules[ruleIndex], field.valueOffset);
            !value) {
            return value;
        }

        seen |= bit;
        previousKey = field.key;
        offset = field.nextOffset;
        if (offset == descriptor.size() && descriptor.back() == ';') {
            return failure(OutputFacetDescriptorErrorCode::EmptyField, offset);
        }
    }

    const auto expected =
        Size == 64 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{1} << Size) - 1U;
    if (seen != expected) {
        return failure(OutputFacetDescriptorErrorCode::MissingKey, descriptor.size());
    }
    return OutputFacetDescriptorValidation::success();
}

[[nodiscard]] OutputFacetDescriptorValidation
validateRationalPixelAspect(const std::string_view descriptor) noexcept {
    if (const auto syntax = validateFixedSchema(descriptor, rationalPixelAspectRules()); !syntax) {
        return syntax;
    }

    ParsedField denominatorField;
    ParsedField numeratorField;
    if (!parseField(descriptor, 0, denominatorField) ||
        !parseField(descriptor, denominatorField.nextOffset, numeratorField)) {
        return failure(OutputFacetDescriptorErrorCode::InternalInvariant, 0);
    }

    constexpr std::size_t unsignedTagSize = 2;
    const auto denominator = parseUnsignedDecimal(denominatorField.value.substr(unsignedTagSize),
                                                  std::numeric_limits<std::uint32_t>::max());
    const auto numerator = parseUnsignedDecimal(numeratorField.value.substr(unsignedTagSize),
                                                std::numeric_limits<std::uint32_t>::max());
    if (denominator.error != OutputFacetDescriptorErrorCode::None ||
        numerator.error != OutputFacetDescriptorErrorCode::None) {
        return failure(OutputFacetDescriptorErrorCode::InternalInvariant, 0);
    }

    if (denominator.value == 0) {
        return failure(OutputFacetDescriptorErrorCode::InvalidRational,
                       denominatorField.valueOffset + unsignedTagSize);
    }
    if (numerator.value == 0) {
        return failure(OutputFacetDescriptorErrorCode::InvalidRational,
                       numeratorField.valueOffset + unsignedTagSize);
    }
    if (std::gcd(numerator.value, denominator.value) != 1) {
        return failure(OutputFacetDescriptorErrorCode::NonNormalizedRational,
                       denominatorField.valueOffset + unsignedTagSize);
    }
    return OutputFacetDescriptorValidation::success();
}

[[nodiscard]] OutputFacetDescriptorValidation
validateChannelIndex(const std::string_view key, const std::string_view prefix,
                     const std::string_view channelCount, const std::size_t keyOffset) noexcept {
    const auto suffix = key.substr(prefix.size());
    const auto index = parseUnsignedDecimal(suffix);
    if (index.error != OutputFacetDescriptorErrorCode::None &&
        index.error != OutputFacetDescriptorErrorCode::UnsignedDecimalOutOfRange) {
        return failure(OutputFacetDescriptorErrorCode::InvalidChannelIndex,
                       keyOffset + prefix.size() + index.relativeOffset);
    }
    if (suffix.size() > channelCount.size() ||
        (suffix.size() == channelCount.size() && suffix >= channelCount)) {
        return failure(OutputFacetDescriptorErrorCode::ChannelIndexOutOfRange,
                       keyOffset + prefix.size());
    }
    return OutputFacetDescriptorValidation::success();
}

[[nodiscard]] OutputFacetDescriptorValidation
validateChannels(const std::string_view descriptor) noexcept {
    std::string_view previousKey;
    std::string_view channelCount;
    UnsignedDecimalResult declaredCount;
    std::uint64_t nameCount = 0;
    std::uint64_t roleCount = 0;
    std::uint64_t expectedNameIndex = 0;
    std::uint64_t expectedRoleIndex = 0;
    bool sawCount = false;
    std::size_t offset = 0;

    while (offset < descriptor.size()) {
        ParsedField field;
        if (const auto parsed = parseField(descriptor, offset, field); !parsed) {
            return parsed;
        }
        if (const auto ordered = validateOrder(previousKey, field); !ordered) {
            return ordered;
        }

        ValueTag tag{};
        if (field.key == "count") {
            if (sawCount || nameCount != 0 || roleCount != 0) {
                return failure(OutputFacetDescriptorErrorCode::DuplicateKey, field.fieldOffset);
            }
            constexpr FieldRule countRule{"count", ValueTag::UnsignedInteger};
            if (const auto value = validateValue(field.value, countRule, field.valueOffset);
                !value) {
                return value;
            }
            tag = ValueTag::UnsignedInteger;
            channelCount = field.value.substr(2);
            declaredCount = parseUnsignedDecimal(channelCount);
            sawCount = true;
        } else if (field.key.starts_with("name-")) {
            if (!sawCount) {
                return failure(OutputFacetDescriptorErrorCode::MissingKey, field.fieldOffset);
            }
            if (const auto index =
                    validateChannelIndex(field.key, "name-", channelCount, field.fieldOffset);
                !index) {
                return index;
            }
            const auto index = parseUnsignedDecimal(field.key.substr(5));
            if (declaredCount.error == OutputFacetDescriptorErrorCode::None &&
                index.value != expectedNameIndex) {
                return failure(OutputFacetDescriptorErrorCode::MissingKey, field.fieldOffset);
            }
            if (nameCount == std::numeric_limits<std::uint64_t>::max()) {
                return failure(OutputFacetDescriptorErrorCode::InternalInvariant,
                               field.fieldOffset);
            }
            ++nameCount;
            if (declaredCount.error == OutputFacetDescriptorErrorCode::None &&
                nameCount < declaredCount.value) {
                expectedNameIndex = nextLexicalIndex(expectedNameIndex, declaredCount.value);
            }
            tag = ValueTag::Utf8;
        } else if (field.key.starts_with("role-")) {
            if (!sawCount) {
                return failure(OutputFacetDescriptorErrorCode::MissingKey, field.fieldOffset);
            }
            if (declaredCount.error == OutputFacetDescriptorErrorCode::None &&
                nameCount != declaredCount.value) {
                return failure(OutputFacetDescriptorErrorCode::MissingKey, field.fieldOffset);
            }
            if (const auto index =
                    validateChannelIndex(field.key, "role-", channelCount, field.fieldOffset);
                !index) {
                return index;
            }
            const auto index = parseUnsignedDecimal(field.key.substr(5));
            if (declaredCount.error == OutputFacetDescriptorErrorCode::None &&
                index.value != expectedRoleIndex) {
                return failure(OutputFacetDescriptorErrorCode::MissingKey, field.fieldOffset);
            }
            if (roleCount == std::numeric_limits<std::uint64_t>::max()) {
                return failure(OutputFacetDescriptorErrorCode::InternalInvariant,
                               field.fieldOffset);
            }
            ++roleCount;
            if (declaredCount.error == OutputFacetDescriptorErrorCode::None &&
                roleCount < declaredCount.value) {
                expectedRoleIndex = nextLexicalIndex(expectedRoleIndex, declaredCount.value);
            }
            tag = ValueTag::Identifier;
        } else {
            return failure(OutputFacetDescriptorErrorCode::UnknownKey, field.fieldOffset);
        }

        const FieldRule rule{field.key, tag};
        if (const auto value = validateValue(field.value, rule, field.valueOffset); !value) {
            return value;
        }
        previousKey = field.key;
        offset = field.nextOffset;
        if (offset == descriptor.size() && descriptor.back() == ';') {
            return failure(OutputFacetDescriptorErrorCode::EmptyField, offset);
        }
    }

    if (!sawCount || declaredCount.error != OutputFacetDescriptorErrorCode::None ||
        nameCount != declaredCount.value || roleCount != declaredCount.value) {
        return failure(OutputFacetDescriptorErrorCode::MissingKey, descriptor.size());
    }
    return OutputFacetDescriptorValidation::success();
}

} // namespace

namespace bloom::output {

OutputFacetDescriptorValueValidation
validateOutputFacetDescriptorValueV1(const std::string_view taggedValue) noexcept {
    return validateTaggedValue(taggedValue);
}

OutputFacetDescriptorValidation
validateOutputFacetDescriptorV1(const OutputFacetDescriptorSchemaV1 schema,
                                const std::string_view descriptor) noexcept {
    if (static_cast<std::uint8_t>(schema) >
        static_cast<std::uint8_t>(OutputFacetDescriptorSchemaV1::ExternalDependencies)) {
        return failure(OutputFacetDescriptorErrorCode::InvalidSchema, 0);
    }
    if (schema == OutputFacetDescriptorSchemaV1::Absent) {
        return descriptor.empty() ? OutputFacetDescriptorValidation::success()
                                  : failure(OutputFacetDescriptorErrorCode::ExpectedAbsent, 0);
    }
    if (descriptor.empty()) {
        return failure(OutputFacetDescriptorErrorCode::UnexpectedEmpty, 0);
    }

    switch (schema) {
    case OutputFacetDescriptorSchemaV1::Absent:
        return failure(OutputFacetDescriptorErrorCode::InternalInvariant, 0);
    case OutputFacetDescriptorSchemaV1::Pixels:
        return validateFixedSchema(descriptor, pixelsRules());
    case OutputFacetDescriptorSchemaV1::Precision:
        return validateFixedSchema(descriptor, precisionRules());
    case OutputFacetDescriptorSchemaV1::Color:
        return validateFixedSchema(descriptor, colorRules());
    case OutputFacetDescriptorSchemaV1::AlphaAssociation:
        return validateFixedSchema(descriptor, alphaRules());
    case OutputFacetDescriptorSchemaV1::Channels:
        return validateChannels(descriptor);
    case OutputFacetDescriptorSchemaV1::Window:
        return validateFixedSchema(descriptor, windowRules());
    case OutputFacetDescriptorSchemaV1::PixelAspectRational:
        return validateRationalPixelAspect(descriptor);
    case OutputFacetDescriptorSchemaV1::PixelAspectBinary32:
        return validateFixedSchema(descriptor, binary32PixelAspectRules());
    case OutputFacetDescriptorSchemaV1::Compression:
        return validateFixedSchema(descriptor, compressionRules());
    case OutputFacetDescriptorSchemaV1::Metadata:
        return validateFixedSchema(descriptor, metadataRules());
    case OutputFacetDescriptorSchemaV1::ExternalDependencies:
        return validateFixedSchema(descriptor, externalDependencyRules());
    }
    return failure(OutputFacetDescriptorErrorCode::InternalInvariant, 0);
}

} // namespace bloom::output
