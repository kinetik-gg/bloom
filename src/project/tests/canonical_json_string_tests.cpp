#include <bloom/project/canonical_json_string.hpp>

#include "canonical_json_string_detail.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using bloom::project::CanonicalJsonStringError;

template <typename Value>
concept ExposesRvalueSizePointer = requires { std::declval<const Value&&>().value(); };

static_assert(!ExposesRvalueSizePointer<bloom::project::CanonicalJsonStringSizeResult>);

void expectEncoding(Expectations& expectations, const std::string_view input,
                    const std::string_view expected, const std::string_view message) {
    std::array<char, 256> output{};
    const auto size = bloom::project::canonicalJsonStringTokenSize(input);
    if (!size || *size.value() > output.size()) {
        expectations.expect(false, message);
        return;
    }
    const auto destination = std::span(output).first(*size.value());
    const auto result = bloom::project::encodeCanonicalJsonStringToken(input, destination);
    expectations.expect(result && result.requiredSize() == expected.size() &&
                            result.bytesWritten() == expected.size() &&
                            std::string_view(output.data(), result.bytesWritten()) == expected,
                        message);
}

void testAsciiSpelling(Expectations& expectations) {
    expectEncoding(expectations, "", "\"\"", "an empty value emits two quotes only");
    expectEncoding(expectations, "Bloom 123", "\"Bloom 123\"",
                   "ordinary ASCII bytes are emitted directly");
    expectEncoding(expectations, "\"", "\"\\\"\"", "a quote uses its short JSON escape");
    expectEncoding(expectations, "\\", "\"\\\\\"", "a backslash uses its short JSON escape");
    expectEncoding(expectations, "/", "\"/\"", "a slash is never escaped");
    constexpr std::string_view deleteCharacter{"\x7F", 1};
    constexpr std::string_view quotedDelete{"\"\x7F\"", 3};
    expectEncoding(expectations, deleteCharacter, quotedDelete,
                   "DEL is emitted directly because it is not a JSON control escape");

    constexpr std::array<std::string_view, 32> expectedControls{
        "\\u0000", "\\u0001", "\\u0002", "\\u0003", "\\u0004", "\\u0005", "\\u0006", "\\u0007",
        "\\b",     "\\t",     "\\n",     "\\u000b", "\\f",     "\\r",     "\\u000e", "\\u000f",
        "\\u0010", "\\u0011", "\\u0012", "\\u0013", "\\u0014", "\\u0015", "\\u0016", "\\u0017",
        "\\u0018", "\\u0019", "\\u001a", "\\u001b", "\\u001c", "\\u001d", "\\u001e", "\\u001f",
    };
    for (std::size_t value = 0; value < expectedControls.size(); ++value) {
        const char inputCharacter = static_cast<char>(value);
        const std::string_view input(&inputCharacter, 1);
        std::array<char, 8> expected{};
        expected[0] = '"';
        std::ranges::copy(expectedControls[value], expected.begin() + 1);
        expected[expectedControls[value].size() + 1] = '"';
        expectEncoding(expectations, input,
                       std::string_view(expected.data(), expectedControls[value].size() + 2),
                       "every ASCII control uses its exact canonical escape");
    }
}

void testUnicodePreservation(Expectations& expectations) {
    constexpr std::array validScalars{
        std::string_view{"\xC2\x80", 2},         std::string_view{"\xDF\xBF", 2},
        std::string_view{"\xE0\xA0\x80", 3},     std::string_view{"\xED\x9F\xBF", 3},
        std::string_view{"\xEE\x80\x80", 3},     std::string_view{"\xEF\xBF\xBF", 3},
        std::string_view{"\xF0\x90\x80\x80", 4}, std::string_view{"\xF4\x8F\xBF\xBF", 4},
    };
    for (const auto scalar : validScalars) {
        std::array<char, 6> expected{};
        expected[0] = '"';
        std::ranges::copy(scalar, expected.begin() + 1);
        expected[scalar.size() + 1] = '"';
        expectEncoding(expectations, scalar, std::string_view(expected.data(), scalar.size() + 2),
                       "valid two-, three-, and four-byte scalar boundaries remain byte-identical");
    }

    constexpr std::string_view composed{"\xC3\xA9", 2};
    constexpr std::string_view decomposed{"e\xCC\x81", 3};
    expectEncoding(expectations, composed, std::string_view{"\"\xC3\xA9\"", 4},
                   "composed Unicode is not normalized");
    expectEncoding(expectations, decomposed, std::string_view{"\"e\xCC\x81\"", 5},
                   "decomposed Unicode is not normalized");
}

void expectInvalidUtf8(Expectations& expectations, const std::string_view input,
                       const std::string_view message) {
    const auto size = bloom::project::canonicalJsonStringTokenSize(input);
    expectations.expect(!size && size.error() == CanonicalJsonStringError::InvalidUtf8, message);

    constexpr char sentinel = '?';
    std::array<char, 16> output{};
    output.fill(sentinel);
    const auto result = bloom::project::encodeCanonicalJsonStringToken(input, output);
    expectations.expect(
        !result && result.error() == CanonicalJsonStringError::InvalidUtf8 &&
            !result.requiredSize().has_value() && result.bytesWritten() == 0 &&
            std::ranges::all_of(output, [](const char value) { return value == sentinel; }),
        "invalid UTF-8 leaves the destination untouched");
}

void testInvalidUtf8(Expectations& expectations) {
    constexpr std::array invalid{
        std::string_view{"\x80", 1},
        std::string_view{"\xC0\x80", 2},
        std::string_view{"\xE0\x80\x80", 3},
        std::string_view{"\xF0\x80\x80\x80", 4},
        std::string_view{"\xED\xA0\x80", 3},
        std::string_view{"\xED\xBF\xBF", 3},
        std::string_view{"\xF4\x90\x80\x80", 4},
        std::string_view{"\xF5\x80\x80\x80", 4},
        std::string_view{"\xC2", 1},
        std::string_view{"\xE0\xA0", 2},
        std::string_view{"\xF0\x90\x80", 3},
        std::string_view{"\xC2\x20", 2},
    };
    for (const auto value : invalid) {
        expectInvalidUtf8(
            expectations, value,
            "overlong, surrogate, out-of-range, truncated, and malformed UTF-8 fails");
    }
}

void testExactOutputSpans(Expectations& expectations) {
    constexpr std::string_view input{"A\n", 2};
    constexpr std::string_view expected{"\"A\\n\"", 5};
    std::array<char, 6> exactBacking{};
    exactBacking.fill('?');
    const auto exact = std::span(exactBacking).first(expected.size());
    const auto exactResult = bloom::project::encodeCanonicalJsonStringToken(input, exact);
    expectations.expect(exactResult && exactResult.requiredSize() == expected.size() &&
                            exactResult.bytesWritten() == expected.size() &&
                            std::string_view(exact.data(), exact.size()) == expected &&
                            exactBacking.back() == '?',
                        "an exact destination receives the complete token without a trailing NUL");

    std::array<char, 4> small{};
    small.fill('?');
    const auto smallResult = bloom::project::encodeCanonicalJsonStringToken(input, small);
    expectations.expect(
        !smallResult && smallResult.error() == CanonicalJsonStringError::OutputSizeMismatch &&
            smallResult.requiredSize() == expected.size() && smallResult.bytesWritten() == 0 &&
            std::ranges::all_of(small, [](const char value) { return value == '?'; }),
        "a small destination reports exact required size without partial output");

    std::array<char, 6> large{};
    large.fill('?');
    const auto largeResult = bloom::project::encodeCanonicalJsonStringToken(input, large);
    expectations.expect(
        !largeResult && largeResult.error() == CanonicalJsonStringError::OutputSizeMismatch &&
            largeResult.requiredSize() == expected.size() && largeResult.bytesWritten() == 0 &&
            std::ranges::all_of(large, [](const char value) { return value == '?'; }),
        "a large destination is also rejected without partial output");
}

void testSizeArithmetic(Expectations& expectations) {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    const auto largestDirect =
        bloom::project::detail::canonicalJsonStringTokenSizeFromCounts(maximum - 2, 0, 0);
    expectations.expect(largestDirect && *largestDirect.value() == maximum,
                        "direct bytes can fill the exact size_t token boundary");
    const auto directOverflow =
        bloom::project::detail::canonicalJsonStringTokenSizeFromCounts(maximum - 1, 0, 0);
    expectations.expect(!directOverflow &&
                            directOverflow.error() == CanonicalJsonStringError::SizeOverflow,
                        "surrounding quotes overflow before direct-byte addition");

    const auto maximumShortCount = (maximum - 2) / 2;
    const auto largestShort =
        bloom::project::detail::canonicalJsonStringTokenSizeFromCounts(0, maximumShortCount, 0);
    expectations.expect(largestShort && *largestShort.value() == 2 + maximumShortCount * 2,
                        "short escapes preflight at their exact multiplication boundary");
    const auto shortOverflow =
        bloom::project::detail::canonicalJsonStringTokenSizeFromCounts(0, maximumShortCount + 1, 0);
    expectations.expect(!shortOverflow &&
                            shortOverflow.error() == CanonicalJsonStringError::SizeOverflow,
                        "short-escape multiplication overflow is rejected");

    const auto maximumUnicodeCount = (maximum - 2) / 6;
    const auto largestUnicode =
        bloom::project::detail::canonicalJsonStringTokenSizeFromCounts(0, 0, maximumUnicodeCount);
    expectations.expect(largestUnicode && *largestUnicode.value() == 2 + maximumUnicodeCount * 6,
                        "unicode escapes preflight at their exact multiplication boundary");
    const auto unicodeOverflow = bloom::project::detail::canonicalJsonStringTokenSizeFromCounts(
        0, 0, maximumUnicodeCount + 1);
    expectations.expect(!unicodeOverflow &&
                            unicodeOverflow.error() == CanonicalJsonStringError::SizeOverflow,
                        "unicode-escape multiplication overflow is rejected");
}

} // namespace

int main() {
    Expectations expectations;
    testAsciiSpelling(expectations);
    testUnicodePreservation(expectations);
    testInvalidUtf8(expectations);
    testExactOutputSpans(expectations);
    testSizeArithmetic(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
