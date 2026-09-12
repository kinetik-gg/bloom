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

// decodeUtf8Scalar() must accept exactly what isValidUtf8() accepts and fail at exactly the same
// byte: the CPU text rasterizer validates the whole string once and then decodes it scalar by
// scalar, so a disagreement between the two would be an unreachable-code failure in the middle of a
// render (src/render/text_raster.cpp).
void testScalarDecodingAgreesWithValidation(ExpectationContext& expectations) {
    bool everyScalarRoundTrips = true;
    std::uint32_t firstFailure = 0;
    for (std::uint32_t scalar = 0; scalar <= 0x10FFFFU; ++scalar) {
        if (scalar >= 0xD800U && scalar <= 0xDFFFU) {
            continue;
        }
        const auto encoded = encodeScalar(scalar);
        const auto decoded = bloom::core::decodeUtf8Scalar(encoded, 0);
        if (!decoded.isValid() || decoded.length != encoded.size() ||
            decoded.value != static_cast<char32_t>(scalar)) {
            everyScalarRoundTrips = false;
            firstFailure = scalar;
            break;
        }
    }
    expectations.expect(everyScalarRoundTrips,
                        "every Unicode scalar decodes back to itself with its exact byte length "
                        "(first failure U+" +
                            std::to_string(firstFailure) + ")");

    const std::array rejected{
        bytes({0xC0, 0x80}),             // overlong NUL
        bytes({0xC1, 0xBF}),             // overlong two-byte form
        bytes({0xE0, 0x80, 0x80}),       // overlong three-byte form
        bytes({0xED, 0xA0, 0x80}),       // UTF-16 high surrogate
        bytes({0xF0, 0x80, 0x80, 0x80}), // overlong four-byte form
        bytes({0xF4, 0x90, 0x80, 0x80}), // above U+10FFFF
        bytes({0xF5, 0x80, 0x80, 0x80}), // lead byte out of range
        bytes({0xE2, 0x82}),             // truncated three-byte sequence
        bytes({0x80}),                   // lone continuation byte
    };
    bool rejectionsAgree = true;
    for (const auto& sequence : rejected) {
        if (bloom::core::isValidUtf8(sequence) ||
            bloom::core::decodeUtf8Scalar(sequence, 0).isValid()) {
            rejectionsAgree = false;
            break;
        }
    }
    expectations.expect(rejectionsAgree,
                        "the decoder refuses every overlong, surrogate, out-of-range, truncated, "
                        "and stray-continuation sequence the validator refuses");

    const auto mixed = std::string("a") + bytes({0xC3, 0xA9}) + bytes({0xE2, 0x82, 0xAC}) +
                       bytes({0xF0, 0x9F, 0x98, 0x80});
    std::vector<char32_t> scalars;
    std::size_t offset = 0;
    while (offset < mixed.size()) {
        const auto decoded = bloom::core::decodeUtf8Scalar(mixed, offset);
        if (!decoded.isValid()) {
            break;
        }
        scalars.push_back(decoded.value);
        offset += decoded.length;
    }
    expectations.expect(offset == mixed.size() &&
                            scalars == std::vector<char32_t>{U'a', U'é', U'€', U'\U0001F600'},
                        "walking a mixed-width string consumes it exactly once, in order");

    expectations.expect(
        !bloom::core::decodeUtf8Scalar("abc", 3).isValid() &&
            !bloom::core::decodeUtf8Scalar("", 0).isValid() &&
            !bloom::core::decodeUtf8Scalar("abc", 99).isValid(),
        "an offset at or past the end is a plain failure, never a read past the end");
    expectations.expect(
        bloom::core::decodeUtf8Scalar(mixed, 1) == bloom::core::Utf8Scalar{U'é', 2},
        "decoding is positional: the same bytes at an interior offset decode alike");
}

} // namespace

int main() {
    ExpectationContext expectations;
    testEveryUnicodeScalar(expectations);
    testBoundariesAndInvalidSequences(expectations);
    testUnsignedByteOrderingAndNoNormalization(expectations);
    testScalarDecodingAgreesWithValidation(expectations);
    return expectations.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}
