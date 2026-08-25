#include "strict_json_preflight.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>

namespace {

std::atomic<std::size_t> allocationCount = 0;

} // namespace

void* operator new(const std::size_t size) {
    allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* const storage = std::malloc(size == 0 ? 1 : size); storage != nullptr) {
        return storage;
    }
    throw std::bad_alloc{};
}

void* operator new[](const std::size_t size) { return ::operator new(size); }

void operator delete(void* const storage) noexcept { std::free(storage); }

void operator delete[](void* const storage) noexcept { ::operator delete(storage); }

namespace {

using bloom::project::detail::kStrictJsonCheckpointCadenceBytes;
using bloom::project::detail::kStrictJsonDocumentMaximumInputBytes;
using bloom::project::detail::kStrictJsonManifestMaximumInputBytes;
using bloom::project::detail::kStrictJsonMaximumContainerEntries;
using bloom::project::detail::kStrictJsonMaximumDecodedStringBytes;
using bloom::project::detail::kStrictJsonMaximumDepth;
using bloom::project::detail::kStrictJsonMaximumValues;
using bloom::project::detail::preflightStrictJson;
using bloom::project::detail::StrictJsonCheckpoint;
using bloom::project::detail::strictJsonDocumentPreflightLimits;
using bloom::project::detail::strictJsonManifestPreflightLimits;
using bloom::project::detail::StrictJsonPreflightError;
using bloom::project::detail::StrictJsonPreflightLimits;
using bloom::project::detail::StrictJsonPreflightResult;

static_assert(kStrictJsonManifestMaximumInputBytes == 1'048'576);
static_assert(kStrictJsonDocumentMaximumInputBytes == 268'435'456);
static_assert(kStrictJsonMaximumDepth == 128);
static_assert(kStrictJsonMaximumValues == 4'000'000);
static_assert(kStrictJsonMaximumContainerEntries == 1'000'000);
static_assert(kStrictJsonMaximumDecodedStringBytes == 89'478'488);
static_assert(kStrictJsonCheckpointCadenceBytes == 65'536);

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

[[nodiscard]] std::span<const std::byte> bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

[[nodiscard]] StrictJsonPreflightResult
scan(const std::string_view value,
     const StrictJsonPreflightLimits limits =
         strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues),
     const StrictJsonCheckpoint checkpoint = {}) noexcept {
    return preflightStrictJson(bytes(value), limits, checkpoint);
}

void expectSuccess(Expectations& expectations, const std::string_view input,
                   const std::uint64_t expectedValues, const std::uint32_t expectedDepth,
                   const std::string_view message,
                   const StrictJsonPreflightLimits limits =
                       strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues)) {
    const auto result = scan(input, limits);
    expectations.expect(result && result.error == StrictJsonPreflightError::None &&
                            result.errorOffset == input.size() &&
                            result.valueCount == expectedValues &&
                            result.maximumObservedDepth == expectedDepth,
                        message);
}

void expectError(Expectations& expectations, const std::string_view input,
                 const StrictJsonPreflightError error, const std::size_t offset,
                 const std::string_view message,
                 const StrictJsonPreflightLimits limits =
                     strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues)) {
    const auto result = scan(input, limits);
    expectations.expect(!result && result.error == error && result.errorOffset == offset, message);
}

void testLimitFactoriesAndValidation(Expectations& expectations) {
    const auto manifest = strictJsonManifestPreflightLimits(17);
    const auto document = strictJsonDocumentPreflightLimits(23);
    expectations.expect(
        manifest.maximumInputBytes == kStrictJsonManifestMaximumInputBytes &&
            manifest.maximumValues == 17 &&
            manifest.maximumContainerEntries == kStrictJsonMaximumContainerEntries &&
            manifest.maximumDepth == kStrictJsonMaximumDepth &&
            manifest.maximumDecodedStringBytes == kStrictJsonManifestMaximumInputBytes,
        "manifest limits retain the caller's shared value budget and closed entry ceilings");
    expectations.expect(
        document.maximumInputBytes == kStrictJsonDocumentMaximumInputBytes &&
            document.maximumValues == 23 &&
            document.maximumContainerEntries == kStrictJsonMaximumContainerEntries &&
            document.maximumDepth == kStrictJsonMaximumDepth &&
            document.maximumDecodedStringBytes == kStrictJsonMaximumDecodedStringBytes,
        "document limits retain the payload spelling exception without raising other ceilings");

    const auto expectInvalid = [&](StrictJsonPreflightLimits limits,
                                   const std::string_view message) {
        const auto result = scan("null", limits);
        expectations.expect(!result && result.error == StrictJsonPreflightError::InvalidLimits &&
                                result.errorOffset == 0,
                            message);
    };

    auto limits = document;
    limits.maximumInputBytes = kStrictJsonDocumentMaximumInputBytes + 1;
    expectInvalid(limits, "an input-byte ceiling above the hard maximum is invalid");
    limits = document;
    limits.maximumValues = kStrictJsonMaximumValues + 1;
    expectInvalid(limits, "a shared value budget above the hard maximum is invalid");
    limits = document;
    limits.maximumContainerEntries = kStrictJsonMaximumContainerEntries + 1;
    expectInvalid(limits, "a container ceiling above the hard maximum is invalid");
    limits = document;
    limits.maximumDepth = kStrictJsonMaximumDepth + 1;
    expectInvalid(limits, "a depth ceiling above the hard maximum is invalid");
    limits = document;
    limits.maximumDecodedStringBytes = kStrictJsonMaximumDecodedStringBytes + 1;
    expectInvalid(limits, "a decoded-string ceiling above the payload exception is invalid");

    expectError(expectations, "null", StrictJsonPreflightError::ValueLimitExceeded, 0,
                "a zero remaining shared value budget rejects the root value",
                strictJsonDocumentPreflightLimits(0));

    limits = document;
    limits.maximumInputBytes = 4;
    expectSuccess(expectations, "null", 1, 1,
                  "an input exactly at a lowered byte ceiling is accepted", limits);
    expectError(expectations, "null ", StrictJsonPreflightError::InputTooLarge, 4,
                "the first byte beyond a lowered input ceiling is reported", limits);
}

void testGoldenGrammarAndBoundary(Expectations& expectations) {
    expectSuccess(expectations, "null", 1, 1, "a null scalar is one root-inclusive value");
    expectSuccess(expectations, " \t\r\ntrue \n", 1, 1,
                  "only RFC 8259 whitespace is accepted around a root");
    expectSuccess(expectations, "{}", 1, 1, "an empty object is one value");
    expectSuccess(expectations, "[]", 1, 1, "an empty array is one value");
    expectSuccess(expectations, R"({"a":[true,{"b":"x"}],"c":-0.25e+2})", 6, 4,
                  "nested containers, values, and member names have the frozen counting model");
    expectSuccess(expectations, R"({"z":0,"a":1})", 3, 2,
                  "reader preflight accepts noncanonical member order");
    expectSuccess(expectations, R"({"a":1,"\u0061":2})", 3, 2,
                  "syntax preflight deliberately does not claim decoded duplicate-key rejection");

    expectError(expectations, "", StrictJsonPreflightError::EmptyInput, 0,
                "an empty entry is rejected at EOF");
    expectError(expectations, " \n", StrictJsonPreflightError::EmptyInput, 2,
                "a whitespace-only entry reports its EOF offset");
    expectError(expectations, "[1,]", StrictJsonPreflightError::InvalidSyntax, 3,
                "an array trailing comma is rejected at the closing bracket");
    expectError(expectations, R"({"a":1,})", StrictJsonPreflightError::InvalidSyntax, 7,
                "an object trailing comma is rejected at the closing brace");
    expectError(expectations, R"({"a" 1})", StrictJsonPreflightError::InvalidSyntax, 5,
                "a missing colon reports the value byte");
    expectError(expectations, R"({"a":})", StrictJsonPreflightError::InvalidSyntax, 5,
                "a missing object value reports the closing brace");
    expectError(expectations, "[}", StrictJsonPreflightError::InvalidSyntax, 1,
                "a mismatched container close is rejected");
    expectError(expectations, "true false", StrictJsonPreflightError::TrailingData, 5,
                "a second whitespace-delimited root is trailing data");
    expectError(expectations, "truefalse", StrictJsonPreflightError::InvalidSyntax, 4,
                "a literal requires a value delimiter");

    const std::string bom{"\xEF\xBB\xBF{}"};
    expectError(expectations, bom, StrictJsonPreflightError::BomForbidden, 0,
                "a UTF-8 BOM is rejected before grammar scanning");
}

void testRootDelimitingAttackSurfaces(Expectations& expectations) {
    expectError(expectations, "null {}", StrictJsonPreflightError::TrailingData, 5,
                "a container after the root scalar is trailing data");
    expectError(expectations, "null{}", StrictJsonPreflightError::InvalidSyntax, 4,
                "a literal demands a delimiter byte before any following container");
    expectError(expectations, "[0]0", StrictJsonPreflightError::TrailingData, 3,
                "a number glued after the root array is trailing data");
    expectError(expectations, "{}{}", StrictJsonPreflightError::TrailingData, 2,
                "a second root container is trailing data");
    expectError(expectations, "null,", StrictJsonPreflightError::TrailingData, 4,
                "a comma after the root scalar is trailing data");
    expectSuccess(expectations, "[0] \n\t ", 2, 2,
                  "trailing RFC 8259 whitespace after a container root is absorbed");

    std::string trailingNullByte{"null"};
    trailingNullByte.push_back(static_cast<char>(0x00U));
    expectError(expectations, trailingNullByte, StrictJsonPreflightError::InvalidSyntax, 4,
                "a NUL byte after the root literal is a delimiter failure, not termination");
}

void testNumberGrammar(Expectations& expectations) {
    constexpr std::array valid{
        "0", "-0", "1", "-1", "0.0", "1.25", "1e0", "1E+2", "-1.2e-3",
    };
    for (const std::string_view number : valid) {
        expectSuccess(expectations, number, 1, 1, "a valid RFC 8259 number spelling passes");
    }

    struct InvalidNumber final {
        std::string_view text;
        StrictJsonPreflightError error;
        std::size_t offset;
    };
    constexpr std::array invalid{
        InvalidNumber{"+1", StrictJsonPreflightError::InvalidSyntax, 0},
        InvalidNumber{".1", StrictJsonPreflightError::InvalidSyntax, 0},
        InvalidNumber{"01", StrictJsonPreflightError::InvalidNumber, 1},
        InvalidNumber{"-", StrictJsonPreflightError::InvalidNumber, 1},
        InvalidNumber{"1.", StrictJsonPreflightError::InvalidNumber, 2},
        InvalidNumber{"1e", StrictJsonPreflightError::InvalidNumber, 2},
        InvalidNumber{"1e+", StrictJsonPreflightError::InvalidNumber, 3},
        InvalidNumber{"1x", StrictJsonPreflightError::InvalidNumber, 1},
        InvalidNumber{"--1", StrictJsonPreflightError::InvalidNumber, 1},
    };
    for (const auto& fixture : invalid) {
        expectError(expectations, fixture.text, fixture.error, fixture.offset,
                    "an invalid JSON number reports its first invalid or EOF byte");
    }

    expectSuccess(expectations, "1e999999999999999999999999999999", 1, 1,
                  "syntax preflight does not claim finite typed-number conversion");
}

void testExtendedNumberGrammarAttacks(Expectations& expectations) {
    expectError(expectations, "Infinity", StrictJsonPreflightError::InvalidSyntax, 0,
                "an Infinity literal is not a recognized JSON value start");
    expectError(expectations, "NaN", StrictJsonPreflightError::InvalidSyntax, 0,
                "a NaN literal is not a recognized JSON value start");
    expectError(expectations, "-Infinity", StrictJsonPreflightError::InvalidNumber, 1,
                "a signed Infinity spelling reports its non-digit after the sign");
    expectError(expectations, "-.", StrictJsonPreflightError::InvalidNumber, 1,
                "a sign followed by a decimal point has no integer digit");
    expectError(expectations, "0x10", StrictJsonPreflightError::InvalidNumber, 1,
                "a hexadecimal spelling reports its non-delimiter suffix");
    expectError(expectations, "1.2.3", StrictJsonPreflightError::InvalidNumber, 3,
                "a second decimal point reports the stray byte");
    expectError(expectations, "1e-", StrictJsonPreflightError::InvalidNumber, 3,
                "an exponent sign without digits at EOF reports EOF");
}

void testStringsAndUnicode(Expectations& expectations) {
    expectSuccess(expectations, R"("\"\\\/\b\f\n\r\t")", 1, 1,
                  "every RFC 8259 short escape is accepted");
    expectSuccess(expectations, "\"Bloom \xF0\x9F\x8C\xB8\"", 1, 1,
                  "a shortest-form raw astral scalar is accepted");
    expectSuccess(expectations, R"("\uD83D\uDE00")", 1, 1,
                  "a valid escaped surrogate pair is one Unicode scalar");
    expectSuccess(expectations, R"("\u0000")", 1, 1,
                  "generic syntax preflight permits escaped NUL for schema-level rejection");

    expectError(expectations, R"("\x")", StrictJsonPreflightError::InvalidEscape, 2,
                "an unknown escape reports its escape code");
    expectError(expectations, R"("\u12G4")", StrictJsonPreflightError::InvalidEscape, 5,
                "a malformed Unicode escape reports its first non-hex byte");
    expectError(expectations, R"("\uDE00")", StrictJsonPreflightError::InvalidUnicodeScalar, 1,
                "a lone low surrogate reports its escape start");
    expectError(expectations, R"("\uD83D")", StrictJsonPreflightError::InvalidUnicodeScalar, 7,
                "a high surrogate without a pair reports the following byte");
    expectError(expectations, R"("\uD83D\u0041")", StrictJsonPreflightError::InvalidUnicodeScalar,
                7, "a non-low second code unit reports the second escape");
    expectError(expectations, "\"abc", StrictJsonPreflightError::InvalidSyntax, 4,
                "an unterminated ordinary string reports EOF");
    expectError(expectations, "\"abc\\", StrictJsonPreflightError::InvalidEscape, 5,
                "a terminal backslash reports EOF");
    expectError(expectations, "\"a\nb\"", StrictJsonPreflightError::InvalidSyntax, 2,
                "an unescaped control byte is rejected in a string");

    std::string badContinuation{"\""};
    badContinuation.push_back(static_cast<char>(0xC2U));
    badContinuation.push_back('A');
    badContinuation.push_back('"');
    expectError(expectations, badContinuation, StrictJsonPreflightError::InvalidUtf8, 2,
                "an invalid UTF-8 continuation reports that byte");

    std::string truncated{"\""};
    truncated.push_back(static_cast<char>(0xE2U));
    truncated.push_back(static_cast<char>(0x82U));
    expectError(expectations, truncated, StrictJsonPreflightError::InvalidUtf8, truncated.size(),
                "a truncated raw UTF-8 scalar reports EOF");

    std::string overlong{"\""};
    overlong.push_back(static_cast<char>(0xC0U));
    overlong.push_back(static_cast<char>(0x80U));
    overlong.push_back('"');
    expectError(expectations, overlong, StrictJsonPreflightError::InvalidUtf8, 1,
                "an overlong UTF-8 lead byte is rejected");

    std::string loneContinuation{"\""};
    loneContinuation.push_back(static_cast<char>(0x80U));
    loneContinuation.push_back('"');
    expectError(expectations, loneContinuation, StrictJsonPreflightError::InvalidUtf8, 1,
                "a lone UTF-8 continuation byte is rejected at its lead position");

    std::string overlongThreeByte{"\""};
    overlongThreeByte.push_back(static_cast<char>(0xE0U));
    overlongThreeByte.push_back(static_cast<char>(0x80U));
    overlongThreeByte.push_back(static_cast<char>(0x80U));
    overlongThreeByte.push_back('"');
    expectError(expectations, overlongThreeByte, StrictJsonPreflightError::InvalidUtf8, 2,
                "an overlong three-byte UTF-8 scalar reports its constrained second byte");

    std::string overlongFourByte{"\""};
    overlongFourByte.push_back(static_cast<char>(0xF0U));
    overlongFourByte.push_back(static_cast<char>(0x80U));
    overlongFourByte.push_back(static_cast<char>(0x80U));
    overlongFourByte.push_back(static_cast<char>(0x80U));
    overlongFourByte.push_back('"');
    expectError(expectations, overlongFourByte, StrictJsonPreflightError::InvalidUtf8, 2,
                "an overlong four-byte UTF-8 scalar reports its constrained second byte");

    std::string encodedSurrogate{"\""};
    encodedSurrogate.push_back(static_cast<char>(0xEDU));
    encodedSurrogate.push_back(static_cast<char>(0xA0U));
    encodedSurrogate.push_back(static_cast<char>(0x80U));
    encodedSurrogate.push_back('"');
    expectError(expectations, encodedSurrogate, StrictJsonPreflightError::InvalidUtf8, 2,
                "a raw UTF-8 surrogate encoding is rejected");

    std::string beyondUnicode{"\""};
    beyondUnicode.push_back(static_cast<char>(0xF4U));
    beyondUnicode.push_back(static_cast<char>(0x90U));
    beyondUnicode.push_back(static_cast<char>(0x80U));
    beyondUnicode.push_back(static_cast<char>(0x80U));
    beyondUnicode.push_back('"');
    expectError(expectations, beyondUnicode, StrictJsonPreflightError::InvalidUtf8, 2,
                "a scalar above U+10FFFF reports its constrained second byte");

    std::string forbiddenLead{"\""};
    forbiddenLead.push_back(static_cast<char>(0xF5U));
    forbiddenLead.push_back(static_cast<char>(0x80U));
    forbiddenLead.push_back(static_cast<char>(0x80U));
    forbiddenLead.push_back(static_cast<char>(0x80U));
    forbiddenLead.push_back('"');
    expectError(expectations, forbiddenLead, StrictJsonPreflightError::InvalidUtf8, 1,
                "an out-of-range F5 UTF-8 lead byte is rejected immediately");

    std::string maximumScalar{"\""};
    maximumScalar.push_back(static_cast<char>(0xF4U));
    maximumScalar.push_back(static_cast<char>(0x8FU));
    maximumScalar.push_back(static_cast<char>(0xBFU));
    maximumScalar.push_back(static_cast<char>(0xBFU));
    maximumScalar.push_back('"');
    expectSuccess(expectations, maximumScalar, 1, 1,
                  "the maximum Unicode scalar U+10FFFF is accepted in shortest form");

    std::string invalidRoot(1, static_cast<char>(0xFFU));
    expectError(expectations, invalidRoot, StrictJsonPreflightError::InvalidUtf8, 0,
                "an invalid non-ASCII root byte is classified as malformed UTF-8");
}

void testAdversarialUtf8AndBomVariants(Expectations& expectations) {
    std::string truncatedTwoByte{"\""};
    truncatedTwoByte.push_back(static_cast<char>(0xC2U));
    expectError(expectations, truncatedTwoByte, StrictJsonPreflightError::InvalidUtf8,
                truncatedTwoByte.size(), "a truncated two-byte scalar at EOF reports EOF");

    std::string truncatedFourByte{"\""};
    truncatedFourByte.push_back(static_cast<char>(0xF0U));
    truncatedFourByte.push_back(static_cast<char>(0x9FU));
    truncatedFourByte.push_back(static_cast<char>(0x8CU));
    expectError(expectations, truncatedFourByte, StrictJsonPreflightError::InvalidUtf8,
                truncatedFourByte.size(), "a truncated four-byte scalar at EOF reports EOF");

    std::string forbiddenC1Lead{"\""};
    forbiddenC1Lead.push_back(static_cast<char>(0xC1U));
    forbiddenC1Lead.push_back(static_cast<char>(0x8FU));
    forbiddenC1Lead.push_back('"');
    expectError(expectations, forbiddenC1Lead, StrictJsonPreflightError::InvalidUtf8, 1,
                "a C1 lead byte outside the two-byte range is rejected immediately");

    std::string forbiddenFeLead{"\""};
    forbiddenFeLead.push_back(static_cast<char>(0xFEU));
    forbiddenFeLead.push_back('"');
    expectError(expectations, forbiddenFeLead, StrictJsonPreflightError::InvalidUtf8, 1,
                "an FE lead byte outside every sequence shape is rejected immediately");

    const std::string bomAfterWhitespace{" \xEF\xBB\xBF"};
    expectError(expectations, bomAfterWhitespace, StrictJsonPreflightError::InvalidSyntax, 1,
                "a BOM after leading whitespace is ordinary syntax garbage at the value position");

    std::string partialBom;
    partialBom.push_back(static_cast<char>(0xEFU));
    partialBom.push_back(static_cast<char>(0xBBU));
    expectError(expectations, partialBom, StrictJsonPreflightError::InvalidUtf8, partialBom.size(),
                "a truncated BOM prefix at the root reports malformed UTF-8 at EOF");

    std::string nearBom;
    nearBom.push_back(static_cast<char>(0xEFU));
    nearBom.push_back(static_cast<char>(0xBBU));
    nearBom.push_back(static_cast<char>(0xBEU));
    expectError(expectations, nearBom, StrictJsonPreflightError::InvalidSyntax, 0,
                "a BOM-adjacent scalar that is not a BOM is plain syntax garbage at the root");
}

void testEscapeSequenceAttacks(Expectations& expectations) {
    expectError(expectations, R"("a\U0041b")", StrictJsonPreflightError::InvalidEscape, 3,
                "an uppercase escape introducer reports its escape code byte");
    expectError(expectations, R"("ab\u12)", StrictJsonPreflightError::InvalidEscape, 7,
                "a unicode escape with fewer than four hex digits at EOF reports EOF");
    expectError(expectations, R"("\u123")", StrictJsonPreflightError::InvalidEscape, 6,
                "a closing quote consumed as a hex-digit slot reports its byte");
    expectError(expectations, R"("\uD83D\")", StrictJsonPreflightError::InvalidUnicodeScalar, 8,
                "a high surrogate followed by a non-u escape reports that byte");
    expectError(expectations, R"("\uD83D\uD83D")", StrictJsonPreflightError::InvalidUnicodeScalar,
                7, "a second high surrogate cannot complete the pair");
    expectSuccess(expectations, R"("\uD800\uDC00")", 1, 1,
                  "the lowest escaped surrogate pair decodes to U+10000");
    expectSuccess(expectations, R"("\uDBFF\uDFFF")", 1, 1,
                  "the highest escaped surrogate pair decodes to U+10FFFF");
    expectSuccess(expectations, R"("\ud83d\ude00")", 1, 1,
                  "lowercase hexadecimal escape digits are accepted");
}

void testResourceBoundaries(Expectations& expectations) {
    auto limits = strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues);
    limits.maximumDepth = 4;
    expectSuccess(expectations, "[[[[]]]]", 4, 4,
                  "an empty container exactly at the root-inclusive depth limit passes", limits);
    expectError(expectations, "[[[[[]]]]]", StrictJsonPreflightError::DepthLimitExceeded, 4,
                "a container one level beyond the depth limit is rejected", limits);
    expectError(expectations, "[[[[0]]]]", StrictJsonPreflightError::DepthLimitExceeded, 4,
                "a scalar below the deepest permitted container is rejected", limits);

    const std::string exactDepth(kStrictJsonMaximumDepth, '[');
    std::string exactDepthDocument = exactDepth;
    exactDepthDocument.append(kStrictJsonMaximumDepth, ']');
    expectSuccess(expectations, exactDepthDocument, kStrictJsonMaximumDepth,
                  kStrictJsonMaximumDepth, "the absolute depth boundary is accepted");
    std::string overDepth(kStrictJsonMaximumDepth + 1, '[');
    overDepth.append(kStrictJsonMaximumDepth + 1, ']');
    expectError(expectations, overDepth, StrictJsonPreflightError::DepthLimitExceeded,
                kStrictJsonMaximumDepth, "the absolute depth boundary plus one is rejected");

    limits = strictJsonDocumentPreflightLimits(3);
    expectSuccess(expectations, "[null,null]", 3, 2,
                  "the root and two children exactly consume a three-value budget", limits);
    limits.maximumValues = 2;
    expectError(expectations, "[null,null]", StrictJsonPreflightError::ValueLimitExceeded, 6,
                "the first value beyond the shared budget is rejected", limits);

    limits = strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues);
    limits.maximumContainerEntries = 1;
    expectSuccess(expectations, "[0]", 2, 2, "one array entry meets a one-entry limit", limits);
    expectError(expectations, "[0,1]", StrictJsonPreflightError::ContainerEntryLimitExceeded, 3,
                "the second array value exceeds a one-entry limit", limits);
    expectSuccess(expectations, R"({"a":0})", 2, 2, "one object member meets a one-entry limit",
                  limits);
    expectError(expectations, R"({"a":0,"b":1})",
                StrictJsonPreflightError::ContainerEntryLimitExceeded, 7,
                "the second object key exceeds a one-member limit", limits);
    limits.maximumContainerEntries = 0;
    expectSuccess(expectations, "{}", 1, 1, "an empty object is valid with a zero-entry limit",
                  limits);
    expectError(expectations, R"({"":0})", StrictJsonPreflightError::ContainerEntryLimitExceeded, 1,
                "the first member exceeds a zero-entry limit", limits);

    limits = strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues);
    limits.maximumDecodedStringBytes = 4;
    expectSuccess(expectations, "\"\xF0\x9F\x98\x80\"", 1, 1,
                  "a four-byte raw scalar exactly meets the decoded-string limit", limits);
    expectError(expectations, "\"\xF0\x9F\x98\x80x\"",
                StrictJsonPreflightError::DecodedStringLimitExceeded, 5,
                "the scalar that crosses the decoded-string limit is reported", limits);
    expectSuccess(expectations, R"("\uD83D\uDE00")", 1, 1,
                  "an escaped astral scalar also decodes to exactly four bytes", limits);
    limits.maximumDecodedStringBytes = 3;
    expectError(
        expectations, R"("\uD83D\uDE00")", StrictJsonPreflightError::DecodedStringLimitExceeded, 1,
        "the paired escape start is reported when its decoded scalar exceeds the limit", limits);
    limits.maximumDecodedStringBytes = 0;
    expectSuccess(expectations, R"({"":""})", 2, 2,
                  "empty member names and values need no decoded-string budget", limits);
    expectError(expectations, R"({"a":null})", StrictJsonPreflightError::DecodedStringLimitExceeded,
                2, "member names obey the same absolute decoded-string preflight", limits);
}

void testAlternatingDepthBomb(Expectations& expectations) {
    constexpr std::string_view arrayWrapper = R"({"a":[)";
    const auto buildAlternating = [](const std::string_view wrapper,
                                     const std::uint32_t wrapperCount) {
        std::string built{"["};
        for (std::uint32_t level = 0; level < wrapperCount; ++level) {
            built.append(wrapper);
        }
        built.push_back('0');
        for (std::uint32_t level = 0; level < wrapperCount; ++level) {
            built.append("]}");
        }
        built.push_back(']');
        return built;
    };

    expectSuccess(expectations, buildAlternating(arrayWrapper, kStrictJsonMaximumDepth / 2U - 1U),
                  kStrictJsonMaximumDepth, kStrictJsonMaximumDepth,
                  "deeply alternating array and object nesting meets the absolute depth boundary");
    expectError(expectations, buildAlternating(arrayWrapper, kStrictJsonMaximumDepth / 2U),
                StrictJsonPreflightError::DepthLimitExceeded,
                (kStrictJsonMaximumDepth / 2U) * arrayWrapper.size(),
                "one further alternating container breaches the absolute depth boundary");

    constexpr std::string_view memberWrapper = R"({"a":)";
    std::string objectNesting;
    for (std::uint32_t level = 0; level < kStrictJsonMaximumDepth - 1U; ++level) {
        objectNesting.append(memberWrapper);
    }
    objectNesting.append("{}");
    objectNesting.append(kStrictJsonMaximumDepth - 1U, '}');
    expectSuccess(expectations, objectNesting, kStrictJsonMaximumDepth, kStrictJsonMaximumDepth,
                  "object-only nesting also meets the absolute depth boundary");

    std::string overObjects;
    for (std::uint32_t level = 0; level < kStrictJsonMaximumDepth; ++level) {
        overObjects.append(memberWrapper);
    }
    overObjects.append("{}");
    overObjects.append(kStrictJsonMaximumDepth, '}');
    expectError(expectations, overObjects, StrictJsonPreflightError::DepthLimitExceeded,
                memberWrapper.size() * kStrictJsonMaximumDepth,
                "object-only nesting one level beyond the boundary reports the breaching brace");
}

void testInvalidLimitsBoundaryCombinations(Expectations& expectations) {
    const StrictJsonPreflightLimits hardMax{
        .maximumInputBytes = kStrictJsonDocumentMaximumInputBytes,
        .maximumValues = kStrictJsonMaximumValues,
        .maximumContainerEntries = kStrictJsonMaximumContainerEntries,
        .maximumDepth = kStrictJsonMaximumDepth,
        .maximumDecodedStringBytes = kStrictJsonMaximumDecodedStringBytes};
    expectSuccess(expectations, R"({"a":[0,-1]})", 4, 3,
                  "every limit simultaneously at its hard maximum remains valid", hardMax);

    auto zeroDepth = strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues);
    zeroDepth.maximumDepth = 0;
    expectError(expectations, "null", StrictJsonPreflightError::DepthLimitExceeded, 0,
                "a zero depth ceiling rejects the root scalar", zeroDepth);
    expectError(expectations, "{}", StrictJsonPreflightError::DepthLimitExceeded, 0,
                "a zero depth ceiling rejects the root container", zeroDepth);

    auto oneValue = strictJsonDocumentPreflightLimits(kStrictJsonMaximumContainerEntries);
    oneValue.maximumValues = 1;
    expectError(expectations, "[[]]", StrictJsonPreflightError::ValueLimitExceeded, 1,
                "the shared value budget wins before container-entry accounting", oneValue);

    auto zeroCeiling = strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues);
    zeroCeiling.maximumInputBytes = 0;
    expectError(expectations, "", StrictJsonPreflightError::EmptyInput, 0,
                "an empty entry meets a zero input ceiling exactly and reports emptiness",
                zeroCeiling);

    const std::string bomDocument{"\xEF\xBB\xBF{}"};
    auto bomSized = strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues);
    bomSized.maximumInputBytes = 2;
    expectError(expectations, bomDocument, StrictJsonPreflightError::InputTooLarge, 2,
                "the input-size ceiling is enforced before BOM classification", bomSized);
}

struct CheckpointLog final {
    std::array<std::size_t, 64> offsets{};
    std::size_t count = 0;
    std::size_t cancelAtOffset = std::numeric_limits<std::size_t>::max();
    bool cancelAtCompletion = false;
};

[[nodiscard]] bool recordCheckpoint(void* context, const std::size_t consumed,
                                    const std::size_t total) noexcept {
    auto& log = *static_cast<CheckpointLog*>(context);
    if (log.count < log.offsets.size()) {
        log.offsets[log.count] = consumed;
    }
    ++log.count;
    if (consumed == log.cancelAtOffset) {
        return false;
    }
    return !(log.cancelAtCompletion && consumed == total && consumed != 0);
}

void expectCheckpointCadence(Expectations& expectations, const std::string& input,
                             const std::string_view message) {
    CheckpointLog log;
    const auto result = scan(input, strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues),
                             StrictJsonCheckpoint{.context = &log, .function = recordCheckpoint});
    bool cadenceIsBounded = result && log.count >= 2 && log.count <= log.offsets.size() &&
                            log.offsets.front() == 0 && log.offsets[log.count - 1] == input.size();
    for (std::size_t index = 1; cadenceIsBounded && index < log.count; ++index) {
        cadenceIsBounded =
            log.offsets[index] >= log.offsets[index - 1] &&
            log.offsets[index] - log.offsets[index - 1] <= kStrictJsonCheckpointCadenceBytes;
    }
    expectations.expect(cadenceIsBounded, message);
}

void testCheckpointsAndCancellation(Expectations& expectations) {
    std::string whitespace(200'000, ' ');
    whitespace += "null";
    expectCheckpointCadence(expectations, whitespace,
                            "long whitespace cannot outrun the checkpoint cadence");

    std::string longString{"\""};
    longString.append(200'000, 'a');
    longString += '"';
    expectCheckpointCadence(expectations, longString,
                            "a long string cannot outrun the checkpoint cadence");

    std::string longNumber(200'000, '7');
    expectCheckpointCadence(expectations, longNumber,
                            "a long number cannot outrun the checkpoint cadence");

    CheckpointLog startCancellation;
    startCancellation.cancelAtOffset = 0;
    auto result =
        scan("null", strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues),
             StrictJsonCheckpoint{.context = &startCancellation, .function = recordCheckpoint});
    expectations.expect(!result && result.error == StrictJsonPreflightError::Cancelled &&
                            result.errorOffset == 0 && startCancellation.count == 1,
                        "cancellation may win at the mandatory offset-zero checkpoint");

    CheckpointLog middleCancellation;
    middleCancellation.cancelAtOffset = kStrictJsonCheckpointCadenceBytes;
    result =
        scan(longString, strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues),
             StrictJsonCheckpoint{.context = &middleCancellation, .function = recordCheckpoint});
    expectations.expect(!result && result.error == StrictJsonPreflightError::Cancelled &&
                            result.errorOffset == kStrictJsonCheckpointCadenceBytes,
                        "mid-token cancellation reports the exact consumed-byte checkpoint");

    CheckpointLog completionCancellation;
    completionCancellation.cancelAtCompletion = true;
    result = scan(
        "null", strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues),
        StrictJsonCheckpoint{.context = &completionCancellation, .function = recordCheckpoint});
    expectations.expect(!result && result.error == StrictJsonPreflightError::Cancelled &&
                            result.errorOffset == 4 && completionCancellation.count == 2 &&
                            completionCancellation.offsets[0] == 0 &&
                            completionCancellation.offsets[1] == 4,
                        "the forced completion checkpoint can cancel before success publication");

    std::string longContainer{"["};
    longContainer.reserve(140'002);
    for (std::size_t index = 0; index < 70'000; ++index) {
        if (index != 0) {
            longContainer.push_back(',');
        }
        longContainer.push_back('0');
    }
    longContainer.push_back(']');

    CheckpointLog containerCancellation;
    containerCancellation.cancelAtOffset = kStrictJsonCheckpointCadenceBytes;
    result =
        scan(longContainer, strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues),
             StrictJsonCheckpoint{.context = &containerCancellation, .function = recordCheckpoint});
    expectations.expect(!result && result.error == StrictJsonPreflightError::Cancelled &&
                            result.errorOffset == kStrictJsonCheckpointCadenceBytes,
                        "cancellation interrupts a long iterative container at the exact cadence");

    const auto allocationsBefore = allocationCount.load(std::memory_order_relaxed);
    result = scan(longContainer);
    const auto allocationsAfter = allocationCount.load(std::memory_order_relaxed);
    expectations.expect(result && allocationsAfter == allocationsBefore,
                        "a prepared input scan performs no ordinary heap allocation");
}

void testCheckpointMonotonicityUnderAdversarialInput(Expectations& expectations) {
    constexpr std::size_t lateOffset = 3 * kStrictJsonCheckpointCadenceBytes;
    std::string longString{"\""};
    longString.append(200'000, 'a');
    longString += '"';
    CheckpointLog lateCancellation;
    lateCancellation.cancelAtOffset = lateOffset;
    const auto cancelled =
        scan(longString, strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues),
             StrictJsonCheckpoint{.context = &lateCancellation, .function = recordCheckpoint});
    expectations.expect(
        !cancelled && cancelled.error == StrictJsonPreflightError::Cancelled &&
            cancelled.errorOffset == lateOffset && lateCancellation.count == 4 &&
            lateCancellation.offsets[0] == 0 &&
            lateCancellation.offsets[1] == kStrictJsonCheckpointCadenceBytes &&
            lateCancellation.offsets[2] == 2 * kStrictJsonCheckpointCadenceBytes &&
            lateCancellation.offsets[3] == 3 * kStrictJsonCheckpointCadenceBytes,
        "a late mid-token cancellation lands on the exact third cadence checkpoint");

    std::string whitespacePrefix(200'000, ' ');
    whitespacePrefix += "null";
    CheckpointLog whitespaceCancellation;
    whitespaceCancellation.cancelAtOffset = 2 * kStrictJsonCheckpointCadenceBytes;
    const auto whitespaceResult = scan(
        whitespacePrefix, strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues),
        StrictJsonCheckpoint{.context = &whitespaceCancellation, .function = recordCheckpoint});
    expectations.expect(!whitespaceResult &&
                            whitespaceResult.error == StrictJsonPreflightError::Cancelled &&
                            whitespaceResult.errorOffset == 2 * kStrictJsonCheckpointCadenceBytes &&
                            whitespaceCancellation.count == 3,
                        "pre-root cancellation interrupts long whitespace at the exact cadence");

    const auto monotonicUntilCancel = [](const CheckpointLog& log) {
        bool monotonic = log.count >= 2;
        for (std::size_t index = 1; monotonic && index < log.count; ++index) {
            monotonic = log.offsets[index] > log.offsets[index - 1];
        }
        return monotonic;
    };
    expectations.expect(
        monotonicUntilCancel(lateCancellation) && monotonicUntilCancel(whitespaceCancellation),
        "reported consumed bytes increase strictly monotonically up to cancellation");

    std::string straddlingString{"\""};
    straddlingString.append(kStrictJsonCheckpointCadenceBytes - 2U, 'a');
    straddlingString.push_back(static_cast<char>(0xF0U));
    straddlingString.push_back(static_cast<char>(0x9FU));
    straddlingString.push_back(static_cast<char>(0x8CU));
    straddlingString.push_back(static_cast<char>(0xB8U));
    straddlingString += '"';
    CheckpointLog straddleLog;
    const auto straddled =
        scan(straddlingString, strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues),
             StrictJsonCheckpoint{.context = &straddleLog, .function = recordCheckpoint});
    expectations.expect(straddled && straddled.valueCount == 1 &&
                            straddled.maximumObservedDepth == 1 && straddleLog.count >= 3 &&
                            straddleLog.offsets[straddleLog.count - 1] == straddlingString.size(),
                        "a four-byte scalar straddling a cadence boundary survives checkpointing");

    std::string invalidContinuation{"\""};
    invalidContinuation.append(70'000, 'a');
    invalidContinuation.push_back(static_cast<char>(0x80U));
    invalidContinuation += '"';
    CheckpointLog continuationLog;
    const auto continuation =
        scan(invalidContinuation, strictJsonDocumentPreflightLimits(kStrictJsonMaximumValues),
             StrictJsonCheckpoint{.context = &continuationLog, .function = recordCheckpoint});
    expectations.expect(
        !continuation && continuation.error == StrictJsonPreflightError::InvalidUtf8 &&
            continuation.errorOffset == 70'001 && continuationLog.count >= 2 &&
            continuationLog.offsets.front() == 0 &&
            continuationLog.offsets[continuationLog.count - 1] < invalidContinuation.size(),
        "an invalid continuation beyond several checkpoints keeps its exact classification");
}

void testDeterministicMutationCorpus(Expectations& expectations) {
    constexpr std::string_view seed =
        R"({"text":"A\uD83D\uDE00","array":[0,-0.5e+2,true,false,null,{}]})";
    constexpr std::array<std::uint8_t, 16> mutations{
        0x00U,
        0x1FU,
        0x7FU,
        0x80U,
        0xFFU,
        static_cast<std::uint8_t>('\"'),
        static_cast<std::uint8_t>('\\'),
        static_cast<std::uint8_t>(','),
        static_cast<std::uint8_t>(':'),
        static_cast<std::uint8_t>('['),
        static_cast<std::uint8_t>(']'),
        static_cast<std::uint8_t>('{'),
        static_cast<std::uint8_t>('}'),
        static_cast<std::uint8_t>('0'),
        static_cast<std::uint8_t>('e'),
        static_cast<std::uint8_t>(' '),
    };

    bool deterministic = true;
    for (std::size_t offset = 0; deterministic && offset < seed.size(); ++offset) {
        for (const auto mutation : mutations) {
            std::string candidate(seed);
            candidate[offset] = static_cast<char>(mutation);
            const auto first = scan(candidate);
            const auto second = scan(candidate);
            deterministic = first == second && first.errorOffset <= candidate.size() &&
                            (!first || first.errorOffset == candidate.size());
            if (!deterministic) {
                break;
            }
        }
    }
    for (std::size_t length = 0; deterministic && length <= seed.size(); ++length) {
        const auto candidate = seed.substr(0, length);
        const auto first = scan(candidate);
        const auto second = scan(candidate);
        deterministic = first == second && first.errorOffset <= candidate.size() &&
                        (!first || first.errorOffset == candidate.size());
    }
    expectations.expect(
        deterministic,
        "deterministic byte mutations and every truncation stay bounded with stable first errors");
}

} // namespace

int main() {
    Expectations expectations;
    testLimitFactoriesAndValidation(expectations);
    testGoldenGrammarAndBoundary(expectations);
    testRootDelimitingAttackSurfaces(expectations);
    testNumberGrammar(expectations);
    testExtendedNumberGrammarAttacks(expectations);
    testStringsAndUnicode(expectations);
    testAdversarialUtf8AndBomVariants(expectations);
    testEscapeSequenceAttacks(expectations);
    testResourceBoundaries(expectations);
    testAlternatingDepthBomb(expectations);
    testInvalidLimitsBoundaryCombinations(expectations);
    testCheckpointsAndCancellation(expectations);
    testCheckpointMonotonicityUnderAdversarialInput(expectations);
    testDeterministicMutationCorpus(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
