#include <bloom/project/document_decode.hpp>

#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <numeric>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bloom::project::DecodedDocumentEnvelope;
using bloom::project::DocumentDecodeError;
using bloom::project::DocumentDecodeResult;
using bloom::project::JsonValue;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::StrictJsonDomResult;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

constexpr std::uint64_t kGenerousOperationBudget = 8ULL << 20U; // 8 MiB: ample for every fixture.

[[nodiscard]] ProjectIoOperationMemory makeOperation(const std::uint64_t limitBytes) {
    auto coordinator = bloom::project::ProjectIoMemoryCoordinator::create(limitBytes);
    if (!coordinator.has_value()) {
        std::abort();
    }
    auto operation = coordinator->createOperation(limitBytes, limitBytes);
    if (!operation.has_value()) {
        std::abort();
    }
    return std::move(*operation);
}

[[nodiscard]] std::span<const std::byte> asBytes(const std::string_view text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// ---------------------------------------------------------------------------------------------
// Minimal hand-assembled document.json fixtures. parseStrictJsonDom accepts any insignificant
// whitespace and member order (canonical layout is only a writer requirement), so these compact,
// non-canonically-ordered literals are valid input; document_decode.cpp's own order checks are
// exercised deliberately by the "wrong member order" fixtures below. Composition objects below
// carry only id/name/duration/format: the decoder matches composition members as a prefix (see
// document_decode.hpp's scope comment), so parameters/animationCurves/graph need not be present.
// ---------------------------------------------------------------------------------------------

constexpr std::string_view kSchemaVersion10 = R"({"major":1,"minor":0})";
constexpr std::string_view kValidDigest =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

[[nodiscard]] std::string colorSettingsJson(const std::string_view digest,
                                            const std::string_view portability,
                                            const std::string_view contextVariablesJson) {
    std::string result =
        R"({"schemaVersion":{"major":1,"minor":0},"processColorSpaceId":"lin_rec709_scene",)"
        R"("ocioConfig":{"schemaVersion":{"major":1,"minor":0},)"
        R"("locator":{"kind":"builtin","uri":"bloom://ocio/neutral-v1/config.ocio"},)"
        R"("expectedRevision":{"algorithm":"sha256","digest":")";
    result += digest;
    result += R"("},"portability":")";
    result += portability;
    result += R"(","contextVariables":[)";
    result += contextVariablesJson;
    result += "]}}";
    return result;
}

[[nodiscard]] std::string defaultColorSettingsJson() {
    return colorSettingsJson(kValidDigest, "builtin", "");
}

[[nodiscard]] std::string compositionJson(
    const std::string_view idJson, const std::string_view name,
    const std::string_view durationNumerator, const std::string_view durationDenominator,
    const std::string_view widthJson, const std::string_view heightJson,
    const std::string_view pixelAspectNumerator, const std::string_view pixelAspectDenominator,
    const std::string_view frameRateNumerator, const std::string_view frameRateDenominator) {
    std::string result = "{\"id\":";
    result += idJson;
    result += ",\"name\":\"";
    result += name;
    result += "\",\"duration\":{\"numerator\":\"";
    result += durationNumerator;
    result += "\",\"denominator\":\"";
    result += durationDenominator;
    result += "\"},\"format\":{\"width\":";
    result += widthJson;
    result += ",\"height\":";
    result += heightJson;
    result += ",\"pixelAspect\":{\"numerator\":\"";
    result += pixelAspectNumerator;
    result += "\",\"denominator\":\"";
    result += pixelAspectDenominator;
    result += "\"},\"frameRate\":{\"numerator\":\"";
    result += frameRateNumerator;
    result += "\",\"denominator\":\"";
    result += frameRateDenominator;
    result += "\"}}}";
    return result;
}

[[nodiscard]] std::string defaultCompositionJson(const std::string_view idJson = R"("1")") {
    return compositionJson(idJson, "Comp", "10", "1", "1920", "1080", "1", "1", "24", "1");
}

[[nodiscard]] std::string documentJson(const std::string_view schemaVersionJson,
                                       const std::string_view projectIdJson,
                                       const std::string_view projectNameJson,
                                       const std::string_view colorSettingsJsonText,
                                       const std::string_view compositionsArrayBody) {
    std::string result = "{\"schemaVersion\":";
    result += schemaVersionJson;
    result += ",\"project\":{\"id\":";
    result += projectIdJson;
    result += ",\"name\":";
    result += projectNameJson;
    result += ",\"colorSettings\":";
    result += colorSettingsJsonText;
    result += ",\"compositions\":[";
    result += compositionsArrayBody;
    result += "]},\"idAllocation\":{},\"extensions\":[]}";
    return result;
}

[[nodiscard]] std::string baselineDocument() {
    return documentJson(kSchemaVersion10, R"("1")", R"("Untitled")", defaultColorSettingsJson(),
                        defaultCompositionJson());
}

[[nodiscard]] std::string documentWithComposition(const std::string_view compositionJsonText) {
    return documentJson(kSchemaVersion10, R"("1")", R"("Untitled")", defaultColorSettingsJson(),
                        compositionJsonText);
}

[[nodiscard]] std::string documentWithColorSettings(const std::string_view colorSettingsJsonText) {
    return documentJson(kSchemaVersion10, R"("1")", R"("Untitled")", colorSettingsJsonText,
                        defaultCompositionJson());
}

// ---------------------------------------------------------------------------------------------
// Decode helpers
// ---------------------------------------------------------------------------------------------

[[nodiscard]] DocumentDecodeResult decodeText(const std::string& text) {
    auto parsed = bloom::project::parseStrictJsonDom(asBytes(text), {},
                                                     makeOperation(kGenerousOperationBudget));
    if (!parsed) {
        // Every fixture below is constructed to be syntactically valid strict JSON; a parse
        // failure means the fixture itself is broken, not the decoder under test. Fail loudly
        // rather than reporting a misleading decode mismatch.
        std::cerr << "fixture failed to parse as strict JSON (error="
                  << static_cast<int>(parsed.error()) << ")\n";
        std::abort();
    }
    return bloom::project::decodeDocumentEnvelope(parsed.document()->root());
}

void expectDecodeFailure(Expectations& expectations, const std::string& text,
                         const DocumentDecodeError expectedError,
                         const std::string_view expectedPath, const std::string_view message) {
    const auto decoded = decodeText(text);
    expectations.expect(
        !decoded && decoded.error() == expectedError && decoded.path() == expectedPath, message);
}

// ---------------------------------------------------------------------------------------------
// Success path sanity and full round trip
// ---------------------------------------------------------------------------------------------

void testBaselineDecodesSuccessfully(Expectations& expectations) {
    const auto decoded = decodeText(baselineDocument());
    expectations.expect(static_cast<bool>(decoded) && decoded.value() != nullptr,
                        "the hand-assembled baseline fixture decodes without error");
    if (decoded.value() != nullptr) {
        const auto& envelope = *decoded.value();
        expectations.expect(envelope.projectId.value() == 1,
                            "the baseline project id decodes to 1");
        expectations.expect(envelope.projectName == "Untitled",
                            "the baseline project name decodes exactly");
        expectations.expect(envelope.compositions.size() == 1,
                            "the baseline fixture decodes exactly one composition");
    }
}

void testFullRoundTrip(Expectations& expectations) {
    using bloom::core::RationalTime;
    using bloom::document::Document;

    const auto duration = RationalTime::create(240, 24);
    if (!duration.has_value()) {
        std::abort();
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    std::array<std::uint8_t, 32> digestBytes{};
    std::iota(digestBytes.begin(), digestBytes.end(), std::uint8_t{0});
    const auto settings = bloom::document::makeBloomNeutralColorSettingsV1(
        bloom::core::Sha256Digest::fromBytes(digestBytes));

    std::array<char, 256> payloadScratch{};
    std::array<std::size_t, 256> sortScratch{};
    const bloom::project::CanonicalDocumentV1 request{
        .snapshot = &snapshot,
        .colorSettings = &settings,
        .payloadScratch = payloadScratch,
        .sortScratch = sortScratch,
    };
    const auto size = bloom::project::canonicalDocumentSize(request);
    if (!size.hasValue()) {
        expectations.expect(false, "the round-trip snapshot sizes successfully");
        return;
    }
    std::vector<char> output(*size.value());
    const auto written = bloom::project::encodeCanonicalDocument(request, output);
    if (!written) {
        expectations.expect(false, "the round-trip snapshot encodes successfully");
        return;
    }

    const std::string_view encodedText(output.data(), output.size());
    auto parsed = bloom::project::parseStrictJsonDom(asBytes(encodedText), {},
                                                     makeOperation(kGenerousOperationBudget));
    expectations.expect(static_cast<bool>(parsed),
                        "the canonical writer's own bytes parse as strict JSON");
    if (!parsed) {
        return;
    }

    const auto decoded = bloom::project::decodeDocumentEnvelope(parsed.document()->root());
    expectations.expect(static_cast<bool>(decoded) && decoded.value() != nullptr,
                        "the canonical writer's own bytes decode without error");
    if (decoded.value() == nullptr) {
        return;
    }

    const auto& envelope = *decoded.value();
    const auto& sourceProject = snapshot.project();
    expectations.expect(envelope.projectId == sourceProject.id(),
                        "round trip: decoded project id equals the source project id");
    expectations.expect(envelope.projectName == sourceProject.name(),
                        "round trip: decoded project name equals the source project name");
    expectations.expect(envelope.colorSettings == settings,
                        "round trip: decoded color settings equal the source color settings");
    expectations.expect(
        envelope.compositions.size() == sourceProject.compositions().size(),
        "round trip: decoded composition count equals the source composition count");
    if (envelope.compositions.size() == sourceProject.compositions().size()) {
        for (std::size_t index = 0; index < envelope.compositions.size(); ++index) {
            const auto& decodedComposition = envelope.compositions[index];
            const auto& sourceComposition = sourceProject.compositions()[index];
            expectations.expect(
                decodedComposition.id == sourceComposition.id(),
                "round trip: decoded composition id equals the source composition id");
            expectations.expect(
                decodedComposition.name == sourceComposition.name(),
                "round trip: decoded composition name equals the source composition name");
            expectations.expect(
                decodedComposition.duration == sourceComposition.duration(),
                "round trip: decoded composition duration equals the source duration");
            expectations.expect(decodedComposition.format == sourceComposition.format(),
                                "round trip: decoded composition format equals the source format");
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Structural rejection: exact member order and unknown members
// ---------------------------------------------------------------------------------------------

void testRejectsRootMemberOrder(Expectations& expectations) {
    const std::string project =
        std::string("{\"id\":\"1\",\"name\":\"Untitled\",\"colorSettings\":") +
        defaultColorSettingsJson() + ",\"compositions\":[" + defaultCompositionJson() + "]}";
    const std::string json = std::string("{\"schemaVersion\":") + std::string(kSchemaVersion10) +
                             ",\"idAllocation\":{},\"project\":" + project + ",\"extensions\":[]}";
    expectDecodeFailure(
        expectations, json, DocumentDecodeError::MemberOutOfOrder, "/idAllocation",
        "a root object with idAllocation before project reports member-out-of-order "
        "at the exact offending key");
}

void testRejectsRootUnknownMember(Expectations& expectations) {
    const std::string json =
        baselineDocument().substr(0, baselineDocument().size() - 1) + ",\"bogus\":null}";
    expectDecodeFailure(expectations, json, DocumentDecodeError::UnknownMember, "/bogus",
                        "an unrecognized trailing root member is rejected with its exact path");
}

void testRejectsProjectMemberOrder(Expectations& expectations) {
    const std::string project =
        std::string("{\"name\":\"Untitled\",\"id\":\"1\",\"colorSettings\":") +
        defaultColorSettingsJson() + ",\"compositions\":[" + defaultCompositionJson() + "]}";
    const std::string json = std::string("{\"schemaVersion\":") + std::string(kSchemaVersion10) +
                             ",\"project\":" + project + ",\"idAllocation\":{},\"extensions\":[]}";
    expectDecodeFailure(expectations, json, DocumentDecodeError::MemberOutOfOrder, "/project/name",
                        "a project object with name before id reports member-out-of-order at the "
                        "exact offending key");
}

void testRejectsCompositionMemberOrder(Expectations& expectations) {
    const std::string composition =
        R"({"name":"Comp","id":"1","duration":{"numerator":"10","denominator":"1"},)"
        R"("format":{"width":1920,"height":1080,"pixelAspect":{"numerator":"1","denominator":"1"},)"
        R"("frameRate":{"numerator":"24","denominator":"1"}}})";
    expectDecodeFailure(
        expectations, documentWithComposition(composition), DocumentDecodeError::MemberOutOfOrder,
        "/project/compositions/0/name",
        "a composition object with name before id reports member-out-of-order at the "
        "exact offending key");
}

void testRejectsFormatMemberOrder(Expectations& expectations) {
    const std::string composition =
        R"({"id":"1","name":"Comp","duration":{"numerator":"10","denominator":"1"},)"
        R"("format":{"height":1080,"width":1920,"pixelAspect":{"numerator":"1","denominator":"1"},)"
        R"("frameRate":{"numerator":"24","denominator":"1"}}})";
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::MemberOutOfOrder,
                        "/project/compositions/0/format/height",
                        "a format object with height before width reports member-out-of-order at "
                        "the exact offending key");
}

void testRejectsCompositionMissingFormat(Expectations& expectations) {
    const std::string composition =
        R"({"id":"1","name":"Comp","duration":{"numerator":"10","denominator":"1"}})";
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::MissingMember, "/project/compositions/0/format",
                        "a composition missing its format member reports the exact missing path");
}

void testRejectsProjectIdWrongKind(Expectations& expectations) {
    const std::string json = documentJson(kSchemaVersion10, "1", R"("Untitled")",
                                          defaultColorSettingsJson(), defaultCompositionJson());
    expectDecodeFailure(expectations, json, DocumentDecodeError::WrongValueKind, "/project/id",
                        "a project id encoded as a JSON number instead of a string is rejected");
}

// ---------------------------------------------------------------------------------------------
// Schema version
// ---------------------------------------------------------------------------------------------

void testRejectsSchemaVersionMinor11(Expectations& expectations) {
    const std::string json = documentJson(R"({"major":1,"minor":1})", R"("1")", R"("Untitled")",
                                          defaultColorSettingsJson(), defaultCompositionJson());
    expectDecodeFailure(expectations, json, DocumentDecodeError::DomainViolation, "/schemaVersion",
                        "document schemaVersion 1.1 is rejected as not exactly 1.0");
}

void testRejectsSchemaVersionMajor2(Expectations& expectations) {
    const std::string json = documentJson(R"({"major":2,"minor":0})", R"("1")", R"("Untitled")",
                                          defaultColorSettingsJson(), defaultCompositionJson());
    expectDecodeFailure(expectations, json, DocumentDecodeError::DomainViolation, "/schemaVersion",
                        "document schemaVersion 2.0 is rejected as not exactly 1.0");
}

// ---------------------------------------------------------------------------------------------
// Object id canonical spelling
// ---------------------------------------------------------------------------------------------

void testRejectsProjectIdSpellings(Expectations& expectations) {
    for (const auto& [spelling, label] :
         {std::pair<std::string_view, std::string_view>{"0", "zero"},
          std::pair<std::string_view, std::string_view>{"01", "leading-zero"},
          std::pair<std::string_view, std::string_view>{"+1", "leading-plus"}}) {
        const std::string idJson = std::string("\"") + std::string(spelling) + "\"";
        const std::string json = documentJson(kSchemaVersion10, idJson, R"("Untitled")",
                                              defaultColorSettingsJson(), defaultCompositionJson());
        expectDecodeFailure(expectations, json, DocumentDecodeError::InvalidId, "/project/id",
                            std::string("a project id spelled '") + std::string(spelling) + "' (" +
                                std::string(label) + ") is rejected as non-canonical");
    }
}

// ---------------------------------------------------------------------------------------------
// Duration
// ---------------------------------------------------------------------------------------------

void testRejectsUnreducedDuration(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "20", "2", "1920", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::UnreducedRational, "/project/compositions/0/duration",
                        "an unreduced duration 20/2 is rejected");
}

void testRejectsZeroDuration(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "0", "1", "1920", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::NonPositiveDuration,
                        "/project/compositions/0/duration",
                        "a zero duration is rejected as non-positive");
}

void testRejectsNegativeDuration(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "-5", "1", "1920", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::NonPositiveDuration,
                        "/project/compositions/0/duration",
                        "a negative duration is rejected as non-positive");
}

// ---------------------------------------------------------------------------------------------
// Pixel aspect
// ---------------------------------------------------------------------------------------------

void testRejectsUnreducedPixelAspect(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "1920", "1080", "2", "2", "24", "1");
    expectDecodeFailure(
        expectations, documentWithComposition(composition), DocumentDecodeError::UnreducedRational,
        "/project/compositions/0/format/pixelAspect", "an unreduced pixel aspect 2/2 is rejected");
}

// ---------------------------------------------------------------------------------------------
// Composition format domain: width/height/pixel product
// ---------------------------------------------------------------------------------------------

void testRejectsZeroWidth(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "0", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::DomainViolation,
                        "/project/compositions/0/format/width", "a zero width is rejected");
}

void testRejectsWidthOverCeiling(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "1048577", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::DomainViolation,
                        "/project/compositions/0/format/width",
                        "a width above the frozen 1048576 ceiling is rejected");
}

void testRejectsZeroHeight(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "1920", "0", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::DomainViolation,
                        "/project/compositions/0/format/height", "a zero height is rejected");
}

void testRejectsHeightOverCeiling(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "1920", "1048577", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::DomainViolation,
                        "/project/compositions/0/format/height",
                        "a height above the frozen 1048576 ceiling is rejected");
}

void testRejectsPixelProductOverCeiling(Expectations& expectations) {
    // Both terms individually satisfy 1..1048576, but 65536 * 65537 = 4295032832 > 2^32.
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "65536", "65537", "1", "1", "24", "1");
    expectDecodeFailure(
        expectations, documentWithComposition(composition), DocumentDecodeError::DomainViolation,
        "/project/compositions/0/format",
        "a checked pixel product over 2^32 is rejected even though width and height "
        "each satisfy the per-dimension ceiling");
}

// ---------------------------------------------------------------------------------------------
// JSON-number uint32 spelling
// ---------------------------------------------------------------------------------------------

void testRejectsWidthFractionSpelling(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "1920.0", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::InvalidJsonUInt32,
                        "/project/compositions/0/format/width",
                        "a width spelled with a fraction (1920.0) is rejected as non-canonical");
}

void testRejectsWidthExponentSpelling(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "192e1", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::InvalidJsonUInt32,
                        "/project/compositions/0/format/width",
                        "a width spelled with an exponent (192e1) is rejected as non-canonical");
}

void testRejectsWidthNegativeZeroSpelling(Expectations& expectations) {
    // "-0" is valid RFC 8259 JSON number syntax (unlike a genuine multi-digit leading zero such as
    // "01920", which the mandatory strict JSON preflight layer rejects before typed decode ever
    // sees it -- see strict_json_preflight_tests.cpp's InvalidNumber{"01", ...} case). Negative
    // zero is the reachable member of the same non-canonical-sign spelling family the contract
    // calls out explicitly: "JSON negative zero is invalid for an integer field."
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "-0", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::InvalidJsonUInt32,
                        "/project/compositions/0/format/width",
                        "a width spelled as negative zero (-0) is rejected as non-canonical");
}

// ---------------------------------------------------------------------------------------------
// OCIO digest spelling
// ---------------------------------------------------------------------------------------------

void testRejectsDigestWrongLength(Expectations& expectations) {
    const std::string shortDigest(kValidDigest.substr(0, kValidDigest.size() - 1));
    const std::string colorSettings = colorSettingsJson(shortDigest, "builtin", "");
    expectDecodeFailure(expectations, documentWithColorSettings(colorSettings),
                        DocumentDecodeError::InvalidDigestSpelling,
                        "/project/colorSettings/ocioConfig/expectedRevision/digest",
                        "a 63-character digest is rejected for its wrong length");
}

void testRejectsDigestUppercaseHex(Expectations& expectations) {
    std::string upperDigest(kValidDigest.substr(0, kValidDigest.size() - 2));
    upperDigest += "1F";
    const std::string colorSettings = colorSettingsJson(upperDigest, "builtin", "");
    expectDecodeFailure(expectations, documentWithColorSettings(colorSettings),
                        DocumentDecodeError::InvalidDigestSpelling,
                        "/project/colorSettings/ocioConfig/expectedRevision/digest",
                        "a digest containing an uppercase hex character is rejected");
}

// ---------------------------------------------------------------------------------------------
// OCIO portability/locator agreement
// ---------------------------------------------------------------------------------------------

void testRejectsPortabilityLocatorMismatch(Expectations& expectations) {
    const std::string colorSettings = colorSettingsJson(kValidDigest, "external", "");
    expectDecodeFailure(expectations, documentWithColorSettings(colorSettings),
                        DocumentDecodeError::DomainViolation,
                        "/project/colorSettings/ocioConfig/portability",
                        "portability 'external' disagreeing with a builtin locator is rejected");
}

// ---------------------------------------------------------------------------------------------
// OCIO context variables
// ---------------------------------------------------------------------------------------------

void testRejectsUnsortedContextVariables(Expectations& expectations) {
    const std::string colorSettings = colorSettingsJson(
        kValidDigest, "builtin", R"({"name":"b","value":"1"},{"name":"a","value":"2"})");
    expectDecodeFailure(expectations, documentWithColorSettings(colorSettings),
                        DocumentDecodeError::DomainViolation,
                        "/project/colorSettings/ocioConfig/contextVariables/1/name",
                        "context variables out of UTF-8 name order are rejected");
}

void testRejectsDuplicateContextVariables(Expectations& expectations) {
    const std::string colorSettings = colorSettingsJson(
        kValidDigest, "builtin", R"({"name":"a","value":"1"},{"name":"a","value":"2"})");
    expectDecodeFailure(expectations, documentWithColorSettings(colorSettings),
                        DocumentDecodeError::DomainViolation,
                        "/project/colorSettings/ocioConfig/contextVariables/1/name",
                        "a duplicate context variable name is rejected");
}

// ---------------------------------------------------------------------------------------------
// Compositions collection ordering
// ---------------------------------------------------------------------------------------------

void testRejectsUnsortedCompositions(Expectations& expectations) {
    const std::string compositions =
        defaultCompositionJson(R"("2")") + "," + defaultCompositionJson(R"("1")");
    expectDecodeFailure(expectations, documentWithComposition(compositions),
                        DocumentDecodeError::UnsortedCompositions, "/project/compositions/1/id",
                        "compositions out of ascending numeric id order are rejected");
}

void testRejectsDuplicateCompositions(Expectations& expectations) {
    const std::string compositions =
        defaultCompositionJson(R"("1")") + "," + defaultCompositionJson(R"("1")");
    expectDecodeFailure(expectations, documentWithComposition(compositions),
                        DocumentDecodeError::DuplicateComposition, "/project/compositions/1/id",
                        "two compositions declaring the same numeric id are rejected");
}

} // namespace

int main() try {
    Expectations expectations;

    testBaselineDecodesSuccessfully(expectations);
    testFullRoundTrip(expectations);

    testRejectsRootMemberOrder(expectations);
    testRejectsRootUnknownMember(expectations);
    testRejectsProjectMemberOrder(expectations);
    testRejectsCompositionMemberOrder(expectations);
    testRejectsFormatMemberOrder(expectations);
    testRejectsCompositionMissingFormat(expectations);
    testRejectsProjectIdWrongKind(expectations);

    testRejectsSchemaVersionMinor11(expectations);
    testRejectsSchemaVersionMajor2(expectations);

    testRejectsProjectIdSpellings(expectations);

    testRejectsUnreducedDuration(expectations);
    testRejectsZeroDuration(expectations);
    testRejectsNegativeDuration(expectations);

    testRejectsUnreducedPixelAspect(expectations);

    testRejectsZeroWidth(expectations);
    testRejectsWidthOverCeiling(expectations);
    testRejectsZeroHeight(expectations);
    testRejectsHeightOverCeiling(expectations);
    testRejectsPixelProductOverCeiling(expectations);

    testRejectsWidthFractionSpelling(expectations);
    testRejectsWidthExponentSpelling(expectations);
    testRejectsWidthNegativeZeroSpelling(expectations);

    testRejectsDigestWrongLength(expectations);
    testRejectsDigestUppercaseHex(expectations);

    testRejectsPortabilityLocatorMismatch(expectations);

    testRejectsUnsortedContextVariables(expectations);
    testRejectsDuplicateContextVariables(expectations);

    testRejectsUnsortedCompositions(expectations);
    testRejectsDuplicateCompositions(expectations);

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " expectation(s) failed\n";
        return 1;
    }
    return 0;
} catch (const std::exception& exception) {
    std::cerr << "Unexpected test exception: " << exception.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "Unexpected non-standard test exception\n";
    return 1;
}
