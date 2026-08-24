#include <bloom/project/canonical_base64.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace {

using bloom::project::CanonicalBase64Error;
using bloom::project::CanonicalBase64SizeResult;

constexpr std::string_view alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct EncodedAnalysis final {
    CanonicalBase64Error error = CanonicalBase64Error::None;
    std::size_t decodedSize = 0;
};

[[nodiscard]] constexpr std::int16_t sextet(const char encoded) noexcept {
    const auto byte = static_cast<unsigned char>(encoded);
    if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) {
        return static_cast<std::int16_t>(byte - static_cast<unsigned char>('A'));
    }
    if (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z')) {
        return static_cast<std::int16_t>(byte - static_cast<unsigned char>('a') + 26U);
    }
    if (byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9')) {
        return static_cast<std::int16_t>(byte - static_cast<unsigned char>('0') + 52U);
    }
    if (byte == static_cast<unsigned char>('+')) {
        return 62;
    }
    if (byte == static_cast<unsigned char>('/')) {
        return 63;
    }
    return -1;
}

[[nodiscard]] EncodedAnalysis analyzeEncoded(const std::string_view encoded) noexcept {
    if (encoded.empty()) {
        return {};
    }
    if (encoded.size() % 4 != 0) {
        return {.error = CanonicalBase64Error::InvalidEncodedLength};
    }

    const auto finalQuartet = encoded.size() - 4;
    for (std::size_t offset = 0; offset < finalQuartet; offset += 4) {
        for (std::size_t index = 0; index < 4; ++index) {
            const char character = encoded[offset + index];
            if (character == '=') {
                return {.error = CanonicalBase64Error::InvalidPadding};
            }
            if (sextet(character) < 0) {
                return {.error = CanonicalBase64Error::InvalidAlphabet};
            }
        }
    }

    std::array<std::int16_t, 4> values{};
    for (std::size_t index = 0; index < 2; ++index) {
        if (encoded[finalQuartet + index] == '=') {
            return {.error = CanonicalBase64Error::InvalidPadding};
        }
        values[index] = sextet(encoded[finalQuartet + index]);
        if (values[index] < 0) {
            return {.error = CanonicalBase64Error::InvalidAlphabet};
        }
    }

    std::uint8_t padding = 0;
    const char third = encoded[finalQuartet + 2];
    const char fourth = encoded[finalQuartet + 3];
    if (third == '=') {
        if (fourth != '=') {
            return {.error = CanonicalBase64Error::InvalidPadding};
        }
        padding = 2;
    } else {
        values[2] = sextet(third);
        if (values[2] < 0) {
            return {.error = CanonicalBase64Error::InvalidAlphabet};
        }
        if (fourth == '=') {
            padding = 1;
        } else {
            values[3] = sextet(fourth);
            if (values[3] < 0) {
                return {.error = CanonicalBase64Error::InvalidAlphabet};
            }
        }
    }

    if ((padding == 2 && (values[1] & 0x0F) != 0) || (padding == 1 && (values[2] & 0x03) != 0)) {
        return {.error = CanonicalBase64Error::NonZeroTailBits};
    }

    const auto quartetCount = encoded.size() / 4;
    const auto decodedSize = quartetCount * 3 - padding;
    return {.error = CanonicalBase64Error::None, .decodedSize = decodedSize};
}

[[nodiscard]] constexpr std::uint8_t byteValue(const std::byte value) noexcept {
    return std::to_integer<std::uint8_t>(value);
}

} // namespace

namespace bloom::project {

CanonicalBase64SizeResult canonicalBase64EncodedSize(const std::size_t decodedSize) noexcept {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    const auto completeGroups = decodedSize / 3;
    if (completeGroups > maximum / 4) {
        return CanonicalBase64SizeResult::failure(CanonicalBase64Error::SizeOverflow);
    }

    auto encodedSize = completeGroups * 4;
    if (decodedSize % 3 != 0) {
        if (encodedSize > maximum - 4) {
            return CanonicalBase64SizeResult::failure(CanonicalBase64Error::SizeOverflow);
        }
        encodedSize += 4;
    }
    return CanonicalBase64SizeResult::success(encodedSize);
}

CanonicalBase64SizeResult canonicalBase64DecodedSize(const std::string_view encoded) noexcept {
    const auto analysis = analyzeEncoded(encoded);
    if (analysis.error != CanonicalBase64Error::None) {
        return CanonicalBase64SizeResult::failure(analysis.error);
    }
    return CanonicalBase64SizeResult::success(analysis.decodedSize);
}

CanonicalBase64WriteResult encodeCanonicalBase64(const std::span<const std::byte> decoded,
                                                 const std::span<char> encoded) noexcept {
    const auto sizeResult = canonicalBase64EncodedSize(decoded.size());
    if (!sizeResult) {
        return CanonicalBase64WriteResult::failure(sizeResult.error());
    }
    const auto requiredSize = *sizeResult.value();
    if (encoded.size() != requiredSize) {
        return CanonicalBase64WriteResult::failure(CanonicalBase64Error::OutputSizeMismatch,
                                                   requiredSize);
    }

    std::size_t inputOffset = 0;
    std::size_t outputOffset = 0;
    while (decoded.size() - inputOffset >= 3) {
        const auto first = byteValue(decoded[inputOffset]);
        const auto second = byteValue(decoded[inputOffset + 1]);
        const auto third = byteValue(decoded[inputOffset + 2]);
        encoded[outputOffset] = alphabet[first >> 2U];
        encoded[outputOffset + 1] =
            alphabet[static_cast<std::uint8_t>(((first & 0x03U) << 4U) | (second >> 4U))];
        encoded[outputOffset + 2] =
            alphabet[static_cast<std::uint8_t>(((second & 0x0FU) << 2U) | (third >> 6U))];
        encoded[outputOffset + 3] = alphabet[third & 0x3FU];
        inputOffset += 3;
        outputOffset += 4;
    }

    const auto remainder = decoded.size() - inputOffset;
    if (remainder != 0) {
        const auto first = byteValue(decoded[inputOffset]);
        encoded[outputOffset] = alphabet[first >> 2U];
        if (remainder == 1) {
            encoded[outputOffset + 1] = alphabet[(first & 0x03U) << 4U];
            encoded[outputOffset + 2] = '=';
            encoded[outputOffset + 3] = '=';
        } else {
            const auto second = byteValue(decoded[inputOffset + 1]);
            encoded[outputOffset + 1] =
                alphabet[static_cast<std::uint8_t>(((first & 0x03U) << 4U) | (second >> 4U))];
            encoded[outputOffset + 2] = alphabet[(second & 0x0FU) << 2U];
            encoded[outputOffset + 3] = '=';
        }
    }
    return CanonicalBase64WriteResult::success(requiredSize);
}

CanonicalBase64WriteResult decodeCanonicalBase64(const std::string_view encoded,
                                                 const std::span<std::byte> decoded) noexcept {
    const auto analysis = analyzeEncoded(encoded);
    if (analysis.error != CanonicalBase64Error::None) {
        return CanonicalBase64WriteResult::failure(analysis.error);
    }
    if (decoded.size() != analysis.decodedSize) {
        return CanonicalBase64WriteResult::failure(CanonicalBase64Error::OutputSizeMismatch,
                                                   analysis.decodedSize);
    }

    std::size_t outputOffset = 0;
    for (std::size_t inputOffset = 0; inputOffset < encoded.size(); inputOffset += 4) {
        const auto first = static_cast<std::uint8_t>(sextet(encoded[inputOffset]));
        const auto second = static_cast<std::uint8_t>(sextet(encoded[inputOffset + 1]));
        decoded[outputOffset] = static_cast<std::byte>((first << 2U) | (second >> 4U));
        ++outputOffset;

        const char thirdCharacter = encoded[inputOffset + 2];
        if (thirdCharacter == '=') {
            continue;
        }
        const auto third = static_cast<std::uint8_t>(sextet(thirdCharacter));
        decoded[outputOffset] = static_cast<std::byte>(((second & 0x0FU) << 4U) | (third >> 2U));
        ++outputOffset;

        const char fourthCharacter = encoded[inputOffset + 3];
        if (fourthCharacter == '=') {
            continue;
        }
        const auto fourth = static_cast<std::uint8_t>(sextet(fourthCharacter));
        decoded[outputOffset] = static_cast<std::byte>(((third & 0x03U) << 6U) | fourth);
        ++outputOffset;
    }
    return CanonicalBase64WriteResult::success(analysis.decodedSize);
}

} // namespace bloom::project
