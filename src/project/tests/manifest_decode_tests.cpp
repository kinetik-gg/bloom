#include <bloom/project/manifest_decode.hpp>

#include <bloom/document/persisted_text.hpp>
#include <bloom/document/schema_version.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/manifest_requirements.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::document::SchemaVersion;
using bloom::project::DecodedManifest;
using bloom::project::JsonValue;
using bloom::project::ManifestDecodeError;
using bloom::project::ManifestDecodeOutcome;
using bloom::project::ManifestDecodeResult;
using bloom::project::ManifestRequirement;
using bloom::project::ProjectIoOperationMemory;

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
// Minimal hand-assembled manifest.json fixtures. parseStrictJsonDom accepts any insignificant
// whitespace and member order (canonical layout is only a writer requirement), so these compact,
// non-canonically-ordered literals are valid input; manifest_decode.cpp's own order checks are
// exercised deliberately by the "wrong member order" fixtures below.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] std::string versionJson(const std::string_view majorToken,
                                      const std::string_view minorToken) {
    std::string result = "{\"major\":";
    result += majorToken;
    result += ",\"minor\":";
    result += minorToken;
    result += "}";
    return result;
}

[[nodiscard]] std::string documentSectionJson(const std::string_view pathJson,
                                              const std::string_view schemaVersionJson) {
    std::string result = "{\"path\":";
    result += pathJson;
    result += ",\"schemaVersion\":";
    result += schemaVersionJson;
    result += "}";
    return result;
}

[[nodiscard]] std::string requirementJson(const std::string_view providerIdJson,
                                          const std::string_view capabilityIdJson,
                                          const std::string_view schemaVersionJson,
                                          const std::string_view nodeTypesJson) {
    std::string result = "{\"providerId\":";
    result += providerIdJson;
    result += ",\"capabilityId\":";
    result += capabilityIdJson;
    result += ",\"schemaVersion\":";
    result += schemaVersionJson;
    result += ",\"providedNodeTypeIds\":";
    result += nodeTypesJson;
    result += "}";
    return result;
}

[[nodiscard]] std::string manifestJson(const std::string_view formatJson,
                                       const std::string_view containerVersionJson,
                                       const std::string_view documentJson,
                                       const std::string_view requirementsJson) {
    std::string result = "{\"format\":";
    result += formatJson;
    result += ",\"containerVersion\":";
    result += containerVersionJson;
    result += ",\"document\":";
    result += documentJson;
    result += ",\"requirements\":";
    result += requirementsJson;
    result += "}";
    return result;
}

constexpr std::string_view kValidFormatJson = R"("org.kinetik.bloom.project")";

[[nodiscard]] std::string validVersion10Json() { return versionJson("1", "0"); }

[[nodiscard]] std::string validDocumentJson() {
    return documentSectionJson(R"("document.json")", validVersion10Json());
}

[[nodiscard]] std::string defaultRequirementJson() {
    return requirementJson(R"("provider.a")", R"("provider.a.cap")", validVersion10Json(), "[]");
}

[[nodiscard]] std::string baselineManifestJson() {
    return manifestJson(kValidFormatJson, validVersion10Json(), validDocumentJson(), "[]");
}

[[nodiscard]] std::string manifestWithFormat(const std::string_view formatJson) {
    return manifestJson(formatJson, validVersion10Json(), validDocumentJson(), "[]");
}

[[nodiscard]] std::string
manifestWithFormatAndContainerVersion(const std::string_view formatJson,
                                      const std::string_view containerVersionJson) {
    return manifestJson(formatJson, containerVersionJson, validDocumentJson(), "[]");
}

[[nodiscard]] std::string
manifestWithContainerVersion(const std::string_view containerVersionJson) {
    return manifestJson(kValidFormatJson, containerVersionJson, validDocumentJson(), "[]");
}

[[nodiscard]] std::string manifestWithDocument(const std::string_view documentJson) {
    return manifestJson(kValidFormatJson, validVersion10Json(), documentJson, "[]");
}

[[nodiscard]] std::string manifestWithRequirements(const std::string_view requirementsJson) {
    return manifestJson(kValidFormatJson, validVersion10Json(), validDocumentJson(),
                        requirementsJson);
}

// ---------------------------------------------------------------------------------------------
// Decode helpers
// ---------------------------------------------------------------------------------------------

[[nodiscard]] ManifestDecodeResult decodeText(const std::string& text) {
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
    return bloom::project::decodeManifestEnvelope(parsed.document()->root());
}

void expectDecodeFailure(Expectations& expectations, const std::string& text,
                         const ManifestDecodeError expectedError,
                         const std::string_view expectedPath, const std::string_view message) {
    const auto decoded = decodeText(text);
    expectations.expect(!decoded && decoded.outcome() == ManifestDecodeOutcome::Failed &&
                            decoded.error() == expectedError && decoded.path() == expectedPath,
                        message);
}

void expectPreservationRequired(Expectations& expectations, const std::string& text,
                                const std::string_view expectedPath,
                                const std::string_view message) {
    const auto decoded = decodeText(text);
    expectations.expect(!decoded &&
                            decoded.outcome() == ManifestDecodeOutcome::PreservationRequired &&
                            decoded.error() == ManifestDecodeError::None &&
                            decoded.path() == expectedPath && decoded.value() == nullptr,
                        message);
}

// ---------------------------------------------------------------------------------------------
// Round trip: bytes produced by encodeCanonicalManifest (canonical_manifest.hpp's writer, the
// module this decoder round-trips against per the task) parse and decode back to exactly the
// input values.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] ManifestRequirement makeRequirement(std::string providerId, std::string capabilityId,
                                                  std::vector<std::string> nodeTypeIds = {},
                                                  const SchemaVersion version = {1, 0}) {
    return {.providerId = std::move(providerId),
            .capabilityId = std::move(capabilityId),
            .schemaVersion = version,
            .providedNodeTypeIds = std::move(nodeTypeIds)};
}

void expectRoundTrip(Expectations& expectations,
                     const std::vector<ManifestRequirement>& requirements,
                     const std::string_view message) {
    const bloom::project::CanonicalManifestV1 manifest{.requirements = requirements};
    const auto size = bloom::project::canonicalManifestSize(manifest);
    if (!size) {
        expectations.expect(false, "fixture manifest sizes successfully");
        return;
    }
    std::vector<char> output(*size.value());
    const auto written = bloom::project::encodeCanonicalManifest(manifest, output);
    if (!written) {
        expectations.expect(false, "fixture manifest encodes successfully");
        return;
    }

    const std::string_view encodedText(output.data(), output.size());
    auto parsed = bloom::project::parseStrictJsonDom(asBytes(encodedText), {},
                                                     makeOperation(kGenerousOperationBudget));
    if (!parsed) {
        expectations.expect(false, "the canonical writer's own bytes parse as strict JSON");
        return;
    }

    const auto decoded = bloom::project::decodeManifestEnvelope(parsed.document()->root());
    const bool decodedOk = static_cast<bool>(decoded) && decoded.value() != nullptr;
    expectations.expect(decodedOk, message);
    if (!decodedOk) {
        return;
    }

    const auto& envelope = *decoded.value();
    expectations.expect(envelope.containerVersion ==
                            bloom::project::kCanonicalManifestContainerVersionV1,
                        "round trip: decoded containerVersion equals the canonical v1 constant");
    expectations.expect(envelope.documentPath == bloom::project::kCanonicalManifestDocumentPath,
                        "round trip: decoded document path equals the canonical constant");
    expectations.expect(envelope.documentSchemaVersion ==
                            bloom::project::kCanonicalManifestDocumentSchemaVersionV1,
                        "round trip: decoded document schemaVersion equals the canonical constant");
    expectations.expect(envelope.requirements == requirements,
                        "round trip: decoded requirements equal the source requirements exactly");
}

void testRoundTripEmptyRequirements(Expectations& expectations) {
    expectRoundTrip(expectations, {}, "an empty-requirements manifest round-trips through decode");
}

void testRoundTripPopulatedRequirements(Expectations& expectations) {
    const std::vector<ManifestRequirement> requirements{
        makeRequirement("provider.a", "provider.a.data"),
        makeRequirement("provider.b", "provider.b.nodes", {"provider.b.blur", "provider.b.text"},
                        {2, 3}),
        makeRequirement("provider.c", "provider.c.data", {"provider.c.only"}),
    };
    expectRoundTrip(expectations, requirements,
                    "a populated multi-requirement, multi-node-type manifest round-trips exactly");
}

// ---------------------------------------------------------------------------------------------
// Success path sanity
// ---------------------------------------------------------------------------------------------

void testBaselineDecodesSuccessfully(Expectations& expectations) {
    const auto decoded = decodeText(baselineManifestJson());
    expectations.expect(static_cast<bool>(decoded) && decoded.value() != nullptr,
                        "the hand-assembled baseline fixture decodes without error");
    if (decoded.value() == nullptr) {
        return;
    }
    const auto& manifest = *decoded.value();
    expectations.expect(manifest.containerVersion == SchemaVersion{1, 0},
                        "the baseline containerVersion decodes to exactly {1,0}");
    expectations.expect(manifest.documentPath == "document.json",
                        "the baseline document path decodes exactly");
    expectations.expect(manifest.documentSchemaVersion == SchemaVersion{1, 0},
                        "the baseline document schemaVersion decodes to exactly {1,0}");
    expectations.expect(manifest.requirements.empty(),
                        "the baseline fixture decodes zero requirements");
}

// ---------------------------------------------------------------------------------------------
// Exact values: format, document.path
// ---------------------------------------------------------------------------------------------

void testRejectsWrongFormat(Expectations& expectations) {
    expectDecodeFailure(expectations, manifestWithFormat(R"("org.kinetik.bloom.other")"),
                        ManifestDecodeError::InvalidFormat, "/format",
                        "a wrong format string is a typed InvalidFormat error");
}

void testRejectsWrongDocumentPath(Expectations& expectations) {
    expectDecodeFailure(
        expectations,
        manifestWithDocument(documentSectionJson(R"("other.json")", validVersion10Json())),
        ManifestDecodeError::InvalidDocumentPath, "/document/path",
        "a wrong document.path string is a typed InvalidDocumentPath error");
}

// ---------------------------------------------------------------------------------------------
// Missing member (one per object; the trailing expected key omitted so the mismatch is a genuine
// absence rather than a reordering)
// ---------------------------------------------------------------------------------------------

void testRejectsRootMissingRequirements(Expectations& expectations) {
    std::string root = "{\"format\":";
    root += kValidFormatJson;
    root += ",\"containerVersion\":";
    root += validVersion10Json();
    root += ",\"document\":";
    root += validDocumentJson();
    root += "}";
    expectDecodeFailure(expectations, root, ManifestDecodeError::MissingMember, "/requirements",
                        "a root missing requirements is a typed MissingMember error");
}

void testRejectsContainerVersionMissingMinor(Expectations& expectations) {
    expectDecodeFailure(expectations, manifestWithContainerVersion(R"({"major":1})"),
                        ManifestDecodeError::MissingMember, "/containerVersion/minor",
                        "a containerVersion missing minor is a typed MissingMember error");
}

void testRejectsDocumentMissingSchemaVersion(Expectations& expectations) {
    expectDecodeFailure(expectations, manifestWithDocument(R"({"path":"document.json"})"),
                        ManifestDecodeError::MissingMember, "/document/schemaVersion",
                        "a document missing schemaVersion is a typed MissingMember error");
}

void testRejectsRequirementMissingProvidedNodeTypeIds(Expectations& expectations) {
    std::string truncated = "{\"providerId\":\"provider.a\",\"capabilityId\":\"provider.a.cap\","
                            "\"schemaVersion\":";
    truncated += validVersion10Json();
    truncated += "}";
    expectDecodeFailure(expectations, manifestWithRequirements("[" + truncated + "]"),
                        ManifestDecodeError::MissingMember, "/requirements/0/providedNodeTypeIds",
                        "a requirement missing providedNodeTypeIds is a typed MissingMember error");
}

// ---------------------------------------------------------------------------------------------
// Extra unknown member at {1,0} (root, version object, document object, requirement)
// ---------------------------------------------------------------------------------------------

void testRejectsRootUnknownMember(Expectations& expectations) {
    std::string root = baselineManifestJson();
    root.pop_back(); // drop closing '}'
    root += R"(,"extra":true})";
    expectDecodeFailure(expectations, root, ManifestDecodeError::UnknownMember, "/extra",
                        "an extra root member at {1,0} is a typed UnknownMember error");
}

void testRejectsContainerVersionUnknownMember(Expectations& expectations) {
    expectDecodeFailure(expectations,
                        manifestWithContainerVersion(R"({"major":1,"minor":0,"extra":true})"),
                        ManifestDecodeError::UnknownMember, "/containerVersion/extra",
                        "an extra containerVersion member at {1,0} is a typed UnknownMember error");
}

void testRejectsDocumentUnknownMember(Expectations& expectations) {
    std::string document = validDocumentJson();
    document.pop_back();
    document += R"(,"extra":true})";
    expectDecodeFailure(expectations, manifestWithDocument(document),
                        ManifestDecodeError::UnknownMember, "/document/extra",
                        "an extra document member at {1,0} is a typed UnknownMember error");
}

void testRejectsRequirementUnknownMember(Expectations& expectations) {
    std::string requirement = defaultRequirementJson();
    requirement.pop_back();
    requirement += R"(,"extra":true})";
    expectDecodeFailure(expectations, manifestWithRequirements("[" + requirement + "]"),
                        ManifestDecodeError::UnknownMember, "/requirements/0/extra",
                        "an extra requirement member at {1,0} is a typed UnknownMember error");
}

// ---------------------------------------------------------------------------------------------
// Members out of order (each object)
// ---------------------------------------------------------------------------------------------

void testRejectsRootMemberOrder(Expectations& expectations) {
    std::string root = "{\"containerVersion\":";
    root += validVersion10Json();
    root += ",\"format\":";
    root += kValidFormatJson;
    root += ",\"document\":";
    root += validDocumentJson();
    root += ",\"requirements\":[]}";
    expectDecodeFailure(expectations, root, ManifestDecodeError::MemberOutOfOrder,
                        "/containerVersion",
                        "a reordered root member is a typed MemberOutOfOrder error");
}

void testRejectsContainerVersionMemberOrder(Expectations& expectations) {
    expectDecodeFailure(expectations, manifestWithContainerVersion(R"({"minor":0,"major":1})"),
                        ManifestDecodeError::MemberOutOfOrder, "/containerVersion/minor",
                        "a reordered containerVersion member is a typed MemberOutOfOrder error");
}

void testRejectsDocumentMemberOrder(Expectations& expectations) {
    std::string document = "{\"schemaVersion\":";
    document += validVersion10Json();
    document += R"(,"path":"document.json"})";
    expectDecodeFailure(expectations, manifestWithDocument(document),
                        ManifestDecodeError::MemberOutOfOrder, "/document/schemaVersion",
                        "a reordered document member is a typed MemberOutOfOrder error");
}

void testRejectsRequirementMemberOrder(Expectations& expectations) {
    // A requirement's key ORDER is swapped here (capabilityId before providerId), unlike
    // requirementJson()'s argument order which only controls key VALUES.
    std::string reordered = "{\"capabilityId\":\"provider.a.cap\",\"providerId\":\"provider.a\","
                            "\"schemaVersion\":";
    reordered += validVersion10Json();
    reordered += R"(,"providedNodeTypeIds":[]})";
    expectDecodeFailure(expectations, manifestWithRequirements("[" + reordered + "]"),
                        ManifestDecodeError::MemberOutOfOrder, "/requirements/0/capabilityId",
                        "a reordered requirement member is a typed MemberOutOfOrder error");
}

// ---------------------------------------------------------------------------------------------
// Non-canonical version numbers (negative, fractional, exponent, string-typed, > uint32)
// ---------------------------------------------------------------------------------------------

void testRejectsNegativeVersionNumber(Expectations& expectations) {
    expectDecodeFailure(expectations, manifestWithContainerVersion(versionJson("-1", "0")),
                        ManifestDecodeError::InvalidJsonUInt32, "/containerVersion/major",
                        "a negative version number is a typed InvalidJsonUInt32 error");
}

void testRejectsFractionalVersionNumber(Expectations& expectations) {
    expectDecodeFailure(expectations, manifestWithContainerVersion(versionJson("1.0", "0")),
                        ManifestDecodeError::InvalidJsonUInt32, "/containerVersion/major",
                        "a fractional version number is a typed InvalidJsonUInt32 error");
}

void testRejectsExponentVersionNumber(Expectations& expectations) {
    expectDecodeFailure(expectations, manifestWithContainerVersion(versionJson("1e0", "0")),
                        ManifestDecodeError::InvalidJsonUInt32, "/containerVersion/major",
                        "an exponent version number is a typed InvalidJsonUInt32 error");
}

void testRejectsStringTypedVersionNumber(Expectations& expectations) {
    expectDecodeFailure(expectations, manifestWithContainerVersion(versionJson(R"("1")", "0")),
                        ManifestDecodeError::WrongValueKind, "/containerVersion/major",
                        "a string-typed version number is a typed WrongValueKind error");
}

void testRejectsOverflowVersionNumber(Expectations& expectations) {
    expectDecodeFailure(expectations, manifestWithContainerVersion(versionJson("4294967296", "0")),
                        ManifestDecodeError::InvalidJsonUInt32, "/containerVersion/major",
                        "a > uint32 version number is a typed InvalidJsonUInt32 error");
}

// ---------------------------------------------------------------------------------------------
// Unsupported majors (0, 2), on both containerVersion.major and document.schemaVersion.major
// ---------------------------------------------------------------------------------------------

void testRejectsContainerVersionMajorZero(Expectations& expectations) {
    expectDecodeFailure(expectations, manifestWithContainerVersion(versionJson("0", "0")),
                        ManifestDecodeError::UnsupportedMajorVersion, "/containerVersion/major",
                        "containerVersion major 0 is a typed UnsupportedMajorVersion error");
}

void testRejectsContainerVersionMajorTwo(Expectations& expectations) {
    expectDecodeFailure(expectations, manifestWithContainerVersion(versionJson("2", "0")),
                        ManifestDecodeError::UnsupportedMajorVersion, "/containerVersion/major",
                        "containerVersion major 2 is a typed UnsupportedMajorVersion error");
}

void testRejectsDocumentSchemaVersionMajorZero(Expectations& expectations) {
    expectDecodeFailure(
        expectations,
        manifestWithDocument(documentSectionJson(R"("document.json")", versionJson("0", "0"))),
        ManifestDecodeError::UnsupportedMajorVersion, "/document/schemaVersion/major",
        "document schemaVersion major 0 is a typed UnsupportedMajorVersion error");
}

void testRejectsDocumentSchemaVersionMajorTwo(Expectations& expectations) {
    expectDecodeFailure(
        expectations,
        manifestWithDocument(documentSectionJson(R"("document.json")", versionJson("2", "0"))),
        ManifestDecodeError::UnsupportedMajorVersion, "/document/schemaVersion/major",
        "document schemaVersion major 2 is a typed UnsupportedMajorVersion error");
}

// ---------------------------------------------------------------------------------------------
// Unsorted requirements (each independently reachable sort key -- see the report: providerId and
// capabilityId are independently testable, but a schemaVersion-only ordering violation cannot
// occur without also being a DuplicateRequirementIdentity, since identity is (providerId,
// capabilityId) alone; the writer's own tests carry the same limitation)
// ---------------------------------------------------------------------------------------------

void testRejectsUnsortedRequirementsByProviderId(Expectations& expectations) {
    const std::string requirements =
        "[" +
        requirementJson(R"("provider.b")", R"("provider.b.cap")", validVersion10Json(), "[]") +
        "," +
        requirementJson(R"("provider.a")", R"("provider.a.cap")", validVersion10Json(), "[]") + "]";
    expectDecodeFailure(
        expectations, manifestWithRequirements(requirements),
        ManifestDecodeError::InvalidRequirementOrder, "/requirements/1",
        "requirements out of order by providerId are a typed InvalidRequirementOrder error");
}

void testRejectsUnsortedRequirementsByCapabilityId(Expectations& expectations) {
    const std::string requirements =
        "[" + requirementJson(R"("provider.a")", R"("provider.a.z")", validVersion10Json(), "[]") +
        "," + requirementJson(R"("provider.a")", R"("provider.a.a")", validVersion10Json(), "[]") +
        "]";
    expectDecodeFailure(expectations, manifestWithRequirements(requirements),
                        ManifestDecodeError::InvalidRequirementOrder, "/requirements/1",
                        "requirements out of order by capabilityId (same providerId) are a typed "
                        "InvalidRequirementOrder error");
}

void testRejectsDuplicateRequirementIdentity(Expectations& expectations) {
    const std::string requirements =
        "[" +
        requirementJson(R"("provider.a")", R"("provider.a.cap")", versionJson("1", "0"), "[]") +
        "," +
        requirementJson(R"("provider.a")", R"("provider.a.cap")", versionJson("2", "0"), "[]") +
        "]";
    expectDecodeFailure(
        expectations, manifestWithRequirements(requirements),
        ManifestDecodeError::DuplicateRequirementIdentity, "/requirements/1",
        "duplicate provider/capability pairs are a typed DuplicateRequirementIdentity error "
        "regardless of schemaVersion");
}

// ---------------------------------------------------------------------------------------------
// Unsorted / duplicate providedNodeTypeIds
// ---------------------------------------------------------------------------------------------

void testRejectsUnsortedProvidedNodeTypeIds(Expectations& expectations) {
    const std::string requirement =
        requirementJson(R"("provider.a")", R"("provider.a.cap")", validVersion10Json(),
                        R"(["provider.a.z","provider.a.a"])");
    expectDecodeFailure(
        expectations, manifestWithRequirements("[" + requirement + "]"),
        ManifestDecodeError::InvalidProvidedNodeTypeOrder, "/requirements/0/providedNodeTypeIds/1",
        "unsorted providedNodeTypeIds are a typed InvalidProvidedNodeTypeOrder error");
}

void testRejectsDuplicateProvidedNodeTypeIds(Expectations& expectations) {
    const std::string requirement =
        requirementJson(R"("provider.a")", R"("provider.a.cap")", validVersion10Json(),
                        R"(["provider.a.x","provider.a.x"])");
    expectDecodeFailure(
        expectations, manifestWithRequirements("[" + requirement + "]"),
        ManifestDecodeError::DuplicateProvidedNodeTypeId, "/requirements/0/providedNodeTypeIds/1",
        "duplicate providedNodeTypeIds are a typed DuplicateProvidedNodeTypeId error");
}

// ---------------------------------------------------------------------------------------------
// Invalid ID lexical domains (uppercase, leading dot, over-length, empty)
// ---------------------------------------------------------------------------------------------

void testRejectsProviderIdUppercase(Expectations& expectations) {
    const std::string requirement =
        requirementJson(R"("Provider")", R"("provider.cap")", validVersion10Json(), "[]");
    expectDecodeFailure(expectations, manifestWithRequirements("[" + requirement + "]"),
                        ManifestDecodeError::InvalidProviderId, "/requirements/0/providerId",
                        "an uppercase providerId is a typed InvalidProviderId error");
}

void testRejectsProviderIdLeadingDot(Expectations& expectations) {
    const std::string requirement =
        requirementJson(R"(".provider")", R"("provider.cap")", validVersion10Json(), "[]");
    expectDecodeFailure(expectations, manifestWithRequirements("[" + requirement + "]"),
                        ManifestDecodeError::InvalidProviderId, "/requirements/0/providerId",
                        "a leading-dot providerId is a typed InvalidProviderId error");
}

void testRejectsProviderIdOverLength(Expectations& expectations) {
    const std::string overLong(bloom::document::kMaxNamespacedIdentifierBytes + 1, 'a');
    const std::string requirement =
        requirementJson("\"" + overLong + "\"", R"("provider.cap")", validVersion10Json(), "[]");
    expectDecodeFailure(expectations, manifestWithRequirements("[" + requirement + "]"),
                        ManifestDecodeError::InvalidProviderId, "/requirements/0/providerId",
                        "a providerId over 128 bytes is a typed InvalidProviderId error");
}

void testRejectsProviderIdEmpty(Expectations& expectations) {
    const std::string requirement =
        requirementJson(R"("")", R"("provider.cap")", validVersion10Json(), "[]");
    expectDecodeFailure(expectations, manifestWithRequirements("[" + requirement + "]"),
                        ManifestDecodeError::InvalidProviderId, "/requirements/0/providerId",
                        "an empty providerId is a typed InvalidProviderId error");
}

void testRejectsCapabilityIdUppercase(Expectations& expectations) {
    const std::string requirement =
        requirementJson(R"("provider")", R"("Provider.Cap")", validVersion10Json(), "[]");
    expectDecodeFailure(expectations, manifestWithRequirements("[" + requirement + "]"),
                        ManifestDecodeError::InvalidCapabilityId, "/requirements/0/capabilityId",
                        "an uppercase capabilityId is a typed InvalidCapabilityId error");
}

void testRejectsProvidedNodeTypeIdUppercase(Expectations& expectations) {
    const std::string requirement = requirementJson(R"("provider.a")", R"("provider.a.cap")",
                                                    validVersion10Json(), R"(["Provider.Type"])");
    expectDecodeFailure(
        expectations, manifestWithRequirements("[" + requirement + "]"),
        ManifestDecodeError::InvalidProvidedNodeTypeId, "/requirements/0/providedNodeTypeIds/0",
        "an uppercase providedNodeTypeIds entry is a typed InvalidProvidedNodeTypeId error");
}

// ---------------------------------------------------------------------------------------------
// `requirements` not an array
// ---------------------------------------------------------------------------------------------

void testRejectsRequirementsNotArray(Expectations& expectations) {
    expectDecodeFailure(expectations, manifestWithRequirements("{}"),
                        ManifestDecodeError::WrongValueKind, "/requirements",
                        "a non-array requirements value is a typed WrongValueKind error");
}

// ---------------------------------------------------------------------------------------------
// Identity before version: a wrong or non-string `format` must fail outright even when
// containerVersion would otherwise classify as newer-minor PreservationRequired -- a manifest that
// does not declare itself as this format is never a "newer Bloom manifest."
// ---------------------------------------------------------------------------------------------

void testRejectsWrongFormatEvenWithNewerMinorContainerVersion(Expectations& expectations) {
    expectDecodeFailure(
        expectations,
        manifestWithFormatAndContainerVersion(R"("org.other.thing")", versionJson("1", "1")),
        ManifestDecodeError::InvalidFormat, "/format",
        "a wrong format string fails outright even under a newer-minor containerVersion, rather "
        "than classifying as PreservationRequired");
}

void testRejectsNonStringFormatEvenWithNewerMinorContainerVersion(Expectations& expectations) {
    expectDecodeFailure(
        expectations, manifestWithFormatAndContainerVersion("1", versionJson("1", "1")),
        ManifestDecodeError::WrongValueKind, "/format",
        "a non-string format fails outright even under a newer-minor containerVersion, rather "
        "than classifying as PreservationRequired");
}

// ---------------------------------------------------------------------------------------------
// Newer-minor classification: containerVersion {1,1}, with and without unknown members, both
// PreservationRequired -- pinned distinctly from every error above.
// ---------------------------------------------------------------------------------------------

void testAcceptsContainerVersionMinor1WithoutUnknownMembers(Expectations& expectations) {
    expectPreservationRequired(expectations, manifestWithContainerVersion(versionJson("1", "1")),
                               "/containerVersion/minor",
                               "containerVersion {1,1} without unknown members classifies as "
                               "PreservationRequired, not Decoded");
}

void testAcceptsContainerVersionMinor1WithUnknownMembersEverywhere(Expectations& expectations) {
    // Unknown members at the root, inside containerVersion itself, inside document, and inside a
    // requirement -- none of them inspected, since decodeManifestEnvelope short-circuits to
    // PreservationRequired immediately once containerVersion.minor > 0 is known (see
    // manifest_decode.hpp's file comment).
    std::string document = validDocumentJson();
    document.pop_back();
    document += R"(,"docExtra":true})";
    std::string requirement = defaultRequirementJson();
    requirement.pop_back();
    requirement += R"(,"reqExtra":true})";
    std::string root = "{\"format\":";
    root += kValidFormatJson;
    root += ",\"containerVersion\":{\"major\":1,\"minor\":1,\"cvExtra\":true},\"document\":";
    root += document;
    root += ",\"requirements\":[";
    root += requirement;
    root += "],\"rootExtra\":true}";
    expectPreservationRequired(expectations, root, "/containerVersion/minor",
                               "containerVersion {1,1} with unknown members everywhere still "
                               "classifies as PreservationRequired, not an UnknownMember error");
}

// ---------------------------------------------------------------------------------------------
// Document schemaVersion {1,1} inside an otherwise-{1,0} manifest: decodes (exposed value
// checked), NOT refused. Manifest/document agreement is a later chain-layer concern.
// ---------------------------------------------------------------------------------------------

void testAcceptsDocumentSchemaVersionMinor1(Expectations& expectations) {
    const auto decoded = decodeText(
        manifestWithDocument(documentSectionJson(R"("document.json")", versionJson("1", "1"))));
    expectations.expect(
        static_cast<bool>(decoded) && decoded.value() != nullptr,
        "a document schemaVersion of {1,1} inside a {1,0} manifest decodes successfully");
    if (decoded.value() == nullptr) {
        return;
    }
    expectations.expect(decoded.value()->documentSchemaVersion == SchemaVersion{1, 1},
                        "the newer document schemaVersion minor is exposed exactly, not refused");
    expectations.expect(decoded.value()->containerVersion == SchemaVersion{1, 0},
                        "the manifest's own containerVersion stays {1,0} in this fixture");
}

} // namespace

int main() try {
    Expectations expectations;

    testRoundTripEmptyRequirements(expectations);
    testRoundTripPopulatedRequirements(expectations);

    testBaselineDecodesSuccessfully(expectations);

    testRejectsWrongFormat(expectations);
    testRejectsWrongDocumentPath(expectations);

    testRejectsRootMissingRequirements(expectations);
    testRejectsContainerVersionMissingMinor(expectations);
    testRejectsDocumentMissingSchemaVersion(expectations);
    testRejectsRequirementMissingProvidedNodeTypeIds(expectations);

    testRejectsRootUnknownMember(expectations);
    testRejectsContainerVersionUnknownMember(expectations);
    testRejectsDocumentUnknownMember(expectations);
    testRejectsRequirementUnknownMember(expectations);

    testRejectsRootMemberOrder(expectations);
    testRejectsContainerVersionMemberOrder(expectations);
    testRejectsDocumentMemberOrder(expectations);
    testRejectsRequirementMemberOrder(expectations);

    testRejectsNegativeVersionNumber(expectations);
    testRejectsFractionalVersionNumber(expectations);
    testRejectsExponentVersionNumber(expectations);
    testRejectsStringTypedVersionNumber(expectations);
    testRejectsOverflowVersionNumber(expectations);

    testRejectsContainerVersionMajorZero(expectations);
    testRejectsContainerVersionMajorTwo(expectations);
    testRejectsDocumentSchemaVersionMajorZero(expectations);
    testRejectsDocumentSchemaVersionMajorTwo(expectations);

    testRejectsUnsortedRequirementsByProviderId(expectations);
    testRejectsUnsortedRequirementsByCapabilityId(expectations);
    testRejectsDuplicateRequirementIdentity(expectations);

    testRejectsUnsortedProvidedNodeTypeIds(expectations);
    testRejectsDuplicateProvidedNodeTypeIds(expectations);

    testRejectsProviderIdUppercase(expectations);
    testRejectsProviderIdLeadingDot(expectations);
    testRejectsProviderIdOverLength(expectations);
    testRejectsProviderIdEmpty(expectations);
    testRejectsCapabilityIdUppercase(expectations);
    testRejectsProvidedNodeTypeIdUppercase(expectations);

    testRejectsRequirementsNotArray(expectations);

    testRejectsWrongFormatEvenWithNewerMinorContainerVersion(expectations);
    testRejectsNonStringFormatEvenWithNewerMinorContainerVersion(expectations);

    testAcceptsContainerVersionMinor1WithoutUnknownMembers(expectations);
    testAcceptsContainerVersionMinor1WithUnknownMembersEverywhere(expectations);

    testAcceptsDocumentSchemaVersionMinor1(expectations);

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
