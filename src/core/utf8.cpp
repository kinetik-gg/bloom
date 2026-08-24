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

} // namespace bloom::core
