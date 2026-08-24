#include <bloom/core/utf8.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {

class ExpectationContext final {
  public:
    bool expect(const bool condition, std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return true;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
        return false;
    }

    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

[[nodiscard]] std::string bytes(std::initializer_list<std::uint8_t> values) {
    std::string result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

[[nodiscard]] std::string encodeScalar(const std::uint32_t scalar) {
    std::string result;
    if (scalar <= 0x7FU) {
        result.push_back(static_cast<char>(scalar));
    } else if (scalar <= 0x7FFU) {
        result.push_back(static_cast<char>(0xC0U | (scalar >> 6U)));
        result.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
    } else if (scalar <= 0xFFFFU) {
        result.push_back(static_cast<char>(0xE0U | (scalar >> 12U)));
        result.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
    } else {
        result.push_back(static_cast<char>(0xF0U | (scalar >> 18U)));
        result.push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
    }
    return result;
}

void testEveryUnicodeScalar(ExpectationContext& expectations) {
    bool everyScalarAccepted = true;
    for (std::uint32_t scalar = 0; scalar <= 0x10FFFFU; ++scalar) {
        if (scalar >= 0xD800U && scalar <= 0xDFFFU) {
            continue;
        }
        if (!bloom::core::isValidUtf8(encodeScalar(scalar))) {
            everyScalarAccepted = false;
            break;
        }
    }
    expectations.expect(everyScalarAccepted,
                        "the canonical encoding of every Unicode scalar is accepted");
}

void testBoundariesAndInvalidSequences(ExpectationContext& expectations) {
    const std::array validBoundaries{
        bytes({0x00}),
        bytes({0x7F}),
        bytes({0xC2, 0x80}),
        bytes({0xDF, 0xBF}),
        bytes({0xE0, 0xA0, 0x80}),
        bytes({0xED, 0x9F, 0xBF}),
        bytes({0xEE, 0x80, 0x80}),
        bytes({0xEF, 0xBF, 0xBF}),
        bytes({0xF0, 0x90, 0x80, 0x80}),
        bytes({0xF4, 0x8F, 0xBF, 0xBF}),
    };
    expectations.expect(std::ranges::all_of(validBoundaries, bloom::core::isValidUtf8),
                        "one-, two-, three-, and four-byte scalar boundaries are accepted");

    std::vector<std::string> invalid{
        bytes({0x80}),
        bytes({0xBF}),
        bytes({0xC0, 0x80}),
        bytes({0xC1, 0xBF}),
        bytes({0xE0, 0x80, 0x80}),
        bytes({0xE0, 0x9F, 0xBF}),
        bytes({0xF0, 0x80, 0x80, 0x80}),
        bytes({0xF0, 0x8F, 0xBF, 0xBF}),
        bytes({0xED, 0xA0, 0x80}),
        bytes({0xED, 0xBF, 0xBF}),
        bytes({0xF4, 0x90, 0x80, 0x80}),
        bytes({0xF5, 0x80, 0x80, 0x80}),
        bytes({0xFF}),
        bytes({0xC2}),
        bytes({0xE0, 0xA0}),
        bytes({0xF0, 0x90, 0x80}),
        bytes({0xC2, 0x20}),
        bytes({0xE1, 0x80, 0x20}),
        bytes({0xF1, 0x80, 0x80, 0x20}),
        std::string("valid") + bytes({0xED, 0xA0, 0x80}),
    };
    expectations.expect(
        std::ranges::none_of(invalid, bloom::core::isValidUtf8),
        "overlong, surrogate, out-of-range, malformed, and truncated sequences are rejected");

    bool standaloneBytesCorrect = true;
    for (std::uint32_t value = 0; value <= 0xFFU; ++value) {
        const auto candidate = bytes({static_cast<std::uint8_t>(value)});
        if (bloom::core::isValidUtf8(candidate) != (value <= 0x7FU)) {
            standaloneBytesCorrect = false;
            break;
        }
    }
    expectations.expect(standaloneBytesCorrect,
                        "all 256 possible standalone bytes have the exact validity result");
}

void testUnsignedByteOrderingAndNoNormalization(ExpectationContext& expectations) {
    const auto composed = bytes({0xC3, 0xA9});
    const auto decomposed = std::string("e") + bytes({0xCC, 0x81});
    expectations.expect(
        bloom::core::compareUtf8Bytes("", "a") == std::strong_ordering::less &&
            bloom::core::compareUtf8Bytes("a", "aa") == std::strong_ordering::less &&
            bloom::core::compareUtf8Bytes("same", "same") == std::strong_ordering::equal &&
            bloom::core::compareUtf8Bytes(bytes({0x7F}), bytes({0x80})) ==
                std::strong_ordering::less &&
            bloom::core::compareUtf8Bytes(bytes({0xFF}), bytes({0x00})) ==
                std::strong_ordering::greater,
        "byte ordering is prefix-aware and compares octets as unsigned values");
    expectations.expect(bloom::core::isValidUtf8(composed) &&
                            bloom::core::isValidUtf8(decomposed) && composed != decomposed &&
                            bloom::core::compareUtf8Bytes(decomposed, composed) !=
                                std::strong_ordering::equal,
                        "validation and ordering do not normalize canonically equivalent text");
}

} // namespace

int main() {
    ExpectationContext expectations;
    testEveryUnicodeScalar(expectations);
    testBoundariesAndInvalidSequences(expectations);
    testUnsignedByteOrderingAndNoNormalization(expectations);
    return expectations.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}
