#include <bloom/core/utf8.hpp>

#include <algorithm>
#include <cstddef>

namespace {

[[nodiscard]] bool isContinuationByte(const unsigned char byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

} // namespace

namespace bloom::core {

bool isValidUtf8(const std::string_view value) noexcept {
    const auto byteAt = [&value](const std::size_t offset) {
        return static_cast<unsigned char>(value[offset]);
    };
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto lead = byteAt(offset);
        if (lead <= 0x7FU) {
            ++offset;
            continue;
        }

        if (lead >= 0xC2U && lead <= 0xDFU) {
            if (offset + 1 >= value.size() || !isContinuationByte(byteAt(offset + 1))) {
                return false;
            }
            offset += 2;
            continue;
        }

        if (lead >= 0xE0U && lead <= 0xEFU) {
            if (offset + 2 >= value.size() || !isContinuationByte(byteAt(offset + 1)) ||
                !isContinuationByte(byteAt(offset + 2))) {
                return false;
            }
            if ((lead == 0xE0U && byteAt(offset + 1) < 0xA0U) ||
                (lead == 0xEDU && byteAt(offset + 1) >= 0xA0U)) {
                return false;
            }
            offset += 3;
            continue;
        }

        if (lead >= 0xF0U && lead <= 0xF4U) {
            if (offset + 3 >= value.size() || !isContinuationByte(byteAt(offset + 1)) ||
                !isContinuationByte(byteAt(offset + 2)) ||
                !isContinuationByte(byteAt(offset + 3))) {
                return false;
            }
            if ((lead == 0xF0U && byteAt(offset + 1) < 0x90U) ||
                (lead == 0xF4U && byteAt(offset + 1) >= 0x90U)) {
                return false;
            }
            offset += 4;
            continue;
        }
        return false;
    }
    return true;
}

std::strong_ordering compareUtf8Bytes(const std::string_view left,
                                      const std::string_view right) noexcept {
    const auto mismatch = std::mismatch(left.begin(), left.end(), right.begin(), right.end());
    if (mismatch.first == left.end() || mismatch.second == right.end()) {
        return left.size() <=> right.size();
    }
    return static_cast<unsigned char>(*mismatch.first) <=>
           static_cast<unsigned char>(*mismatch.second);
}

Utf8Scalar decodeUtf8Scalar(const std::string_view value, const std::size_t offset) noexcept {
    if (offset >= value.size()) {
        return {};
    }
    const auto byteAt = [&value](const std::size_t index) {
        return static_cast<unsigned char>(value[index]);
    };
    const auto continuation = [&](const std::size_t index, const char32_t accumulated) {
        return (accumulated << 6U) | static_cast<char32_t>(byteAt(index) & 0x3FU);
    };

    const auto lead = byteAt(offset);
    if (lead <= 0x7FU) {
        return {static_cast<char32_t>(lead), 1};
    }
    // The acceptance rules below are isValidUtf8()'s, byte for byte: the same lead ranges (0xC2
    // excludes the two-byte overlongs), the same 0xE0/0xED second-byte floors and ceilings
    // (overlong three-byte forms and the surrogate block), and the same 0xF0/0xF4 second-byte
    // bounds (overlong four-byte forms and everything above U+10FFFF). Keeping them in one file is
    // why this decoder lives beside the validator instead of in the one module that first needed
    // it.
    if (lead >= 0xC2U && lead <= 0xDFU) {
        if (offset + 1 >= value.size() || !isContinuationByte(byteAt(offset + 1))) {
            return {};
        }
        return {continuation(offset + 1, static_cast<char32_t>(lead & 0x1FU)), 2};
    }
    if (lead >= 0xE0U && lead <= 0xEFU) {
        if (offset + 2 >= value.size() || !isContinuationByte(byteAt(offset + 1)) ||
            !isContinuationByte(byteAt(offset + 2))) {
            return {};
        }
        if ((lead == 0xE0U && byteAt(offset + 1) < 0xA0U) ||
            (lead == 0xEDU && byteAt(offset + 1) >= 0xA0U)) {
            return {};
        }
        auto scalar = static_cast<char32_t>(lead & 0x0FU);
        scalar = continuation(offset + 1, scalar);
        return {continuation(offset + 2, scalar), 3};
    }
    if (lead >= 0xF0U && lead <= 0xF4U) {
        if (offset + 3 >= value.size() || !isContinuationByte(byteAt(offset + 1)) ||
            !isContinuationByte(byteAt(offset + 2)) || !isContinuationByte(byteAt(offset + 3))) {
            return {};
        }
        if ((lead == 0xF0U && byteAt(offset + 1) < 0x90U) ||
            (lead == 0xF4U && byteAt(offset + 1) >= 0x90U)) {
            return {};
        }
        auto scalar = static_cast<char32_t>(lead & 0x07U);
        scalar = continuation(offset + 1, scalar);
        scalar = continuation(offset + 2, scalar);
        return {continuation(offset + 3, scalar), 4};
    }
    return {};
}

} // namespace bloom::core
