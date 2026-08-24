#include <bloom/project/canonical_json_string.hpp>

#include "canonical_json_string_detail.hpp"

#include <bloom/core/utf8.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

[[nodiscard]] constexpr std::uint8_t byteValue(const char value) noexcept {
    return static_cast<unsigned char>(value);
}

[[nodiscard]] constexpr bool usesShortEscape(const std::uint8_t value) noexcept {
    return value == 0x08U || value == 0x09U || value == 0x0AU || value == 0x0CU || value == 0x0DU ||
           value == static_cast<std::uint8_t>('"') || value == static_cast<std::uint8_t>('\\');
}

[[nodiscard]] constexpr char shortEscapeCode(const std::uint8_t value) noexcept {
    switch (value) {
    case 0x08U:
        return 'b';
    case 0x09U:
        return 't';
    case 0x0AU:
        return 'n';
    case 0x0CU:
        return 'f';
    case 0x0DU:
        return 'r';
    case static_cast<std::uint8_t>('"'):
        return '"';
    case static_cast<std::uint8_t>('\\'):
        return '\\';
    default:
        return '\0';
    }
}

} // namespace

namespace bloom::project {

CanonicalJsonStringSizeResult canonicalJsonStringTokenSize(const std::string_view value) noexcept {
    if (!core::isValidUtf8(value)) {
        return CanonicalJsonStringSizeResult::failure(CanonicalJsonStringError::InvalidUtf8);
    }

    std::size_t directBytes = 0;
    std::size_t shortEscapeCount = 0;
    std::size_t unicodeEscapeCount = 0;
    for (const char character : value) {
        const auto byte = byteValue(character);
        if (usesShortEscape(byte)) {
            ++shortEscapeCount;
        } else if (byte <= 0x1FU) {
            ++unicodeEscapeCount;
        } else {
            ++directBytes;
        }
    }
    return detail::canonicalJsonStringTokenSizeFromCounts(directBytes, shortEscapeCount,
                                                          unicodeEscapeCount);
}

CanonicalJsonStringWriteResult
encodeCanonicalJsonStringToken(const std::string_view value,
                               const std::span<char> output) noexcept {
    const auto sizeResult = canonicalJsonStringTokenSize(value);
    if (!sizeResult) {
        return CanonicalJsonStringWriteResult::failure(sizeResult.error());
    }
    const auto requiredSize = *sizeResult.value();
    if (output.size() != requiredSize) {
        return CanonicalJsonStringWriteResult::failure(CanonicalJsonStringError::OutputSizeMismatch,
                                                       requiredSize);
    }

    constexpr std::string_view hexadecimal = "0123456789abcdef";
    std::size_t offset = 0;
    output[offset++] = '"';
    for (const char character : value) {
        const auto byte = byteValue(character);
        if (usesShortEscape(byte)) {
            output[offset++] = '\\';
            output[offset++] = shortEscapeCode(byte);
        } else if (byte <= 0x1FU) {
            output[offset++] = '\\';
            output[offset++] = 'u';
            output[offset++] = '0';
            output[offset++] = '0';
            output[offset++] = hexadecimal[byte >> 4U];
            output[offset++] = hexadecimal[byte & 0x0FU];
        } else {
            output[offset++] = character;
        }
    }
    output[offset] = '"';
    return CanonicalJsonStringWriteResult::success(requiredSize);
}

} // namespace bloom::project
