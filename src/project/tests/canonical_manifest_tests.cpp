#include <bloom/project/canonical_manifest.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bloom::document::SchemaVersion;
using bloom::project::CanonicalManifestError;
using bloom::project::CanonicalManifestLimits;
using bloom::project::CanonicalManifestV1;
using bloom::project::ManifestRequirement;

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

[[nodiscard]] ManifestRequirement requirement(std::string providerId, std::string capabilityId,
                                              std::vector<std::string> nodeTypeIds = {},
                                              const SchemaVersion version = {1, 0}) {
    return {.providerId = std::move(providerId),
            .capabilityId = std::move(capabilityId),
            .schemaVersion = version,
            .providedNodeTypeIds = std::move(nodeTypeIds)};
}

[[nodiscard]] std::string_view writtenView(const std::span<const char> output,
                                           const std::size_t size) {
    return {output.data(), size};
}

void testEmptyGoldenBytes(Expectations& expectations) {
    constexpr std::string_view expected = "{\n"
                                          "  \"format\": \"org.kinetik.bloom.project\",\n"
                                          "  \"containerVersion\": {\n"
                                          "    \"major\": 1,\n"
                                          "    \"minor\": 0\n"
                                          "  },\n"
                                          "  \"document\": {\n"
                                          "    \"path\": \"document.json\",\n"
                                          "    \"schemaVersion\": {\n"
                                          "      \"major\": 1,\n"
                                          "      \"minor\": 0\n"
                                          "    }\n"
                                          "  },\n"
                                          "  \"requirements\": []\n"
                                          "}\n";
    const CanonicalManifestV1 manifest;
    const auto size = bloom::project::canonicalManifestSize(manifest);
    expectations.expect(size && *size.value() == expected.size(),
                        "empty-manifest preflight returns its exact golden byte count");

    std::array<char, 512> output{};
    output.fill('?');
    const auto result = bloom::project::encodeCanonicalManifest(manifest, output);
    expectations.expect(result && result.requiredSize() == expected.size() &&
                            result.bytesWritten() == expected.size() &&
                            writtenView(output, result.bytesWritten()) == expected &&
                            output[expected.size()] == '?',
                        "the empty manifest emits fixed member order and one final LF only");
}

void testNonEmptyGoldenBytes(Expectations& expectations) {
    const std::vector requirements{
        requirement("provider.a", "provider.a.data"),
        requirement("provider.b", "provider.b.nodes", {"provider.b.blur", "provider.b.text"},
                    {2, 3}),
    };
    const CanonicalManifestV1 manifest{.requirements = requirements};
    constexpr std::string_view expected = "{\n"
                                          "  \"format\": \"org.kinetik.bloom.project\",\n"
                                          "  \"containerVersion\": {\n"
                                          "    \"major\": 1,\n"
                                          "    \"minor\": 0\n"
                                          "  },\n"
                                          "  \"document\": {\n"
                                          "    \"path\": \"document.json\",\n"
                                          "    \"schemaVersion\": {\n"
                                          "      \"major\": 1,\n"
                                          "      \"minor\": 0\n"
                                          "    }\n"
                                          "  },\n"
                                          "  \"requirements\": [\n"
                                          "    {\n"
                                          "      \"providerId\": \"provider.a\",\n"
                                          "      \"capabilityId\": \"provider.a.data\",\n"
                                          "      \"schemaVersion\": {\n"
                                          "        \"major\": 1,\n"
                                          "        \"minor\": 0\n"
                                          "      },\n"
                                          "      \"providedNodeTypeIds\": []\n"
                                          "    },\n"
                                          "    {\n"
                                          "      \"providerId\": \"provider.b\",\n"
                                          "      \"capabilityId\": \"provider.b.nodes\",\n"
                                          "      \"schemaVersion\": {\n"
                                          "        \"major\": 2,\n"
                                          "        \"minor\": 3\n"
                                          "      },\n"
                                          "      \"providedNodeTypeIds\": [\n"
                                          "        \"provider.b.blur\",\n"
                                          "        \"provider.b.text\"\n"
                                          "      ]\n"
                                          "    }\n"
                                          "  ]\n"
                                          "}\n";

    const auto size = bloom::project::canonicalManifestSize(manifest);
    std::vector<char> output(expected.size() + 7, '?');
    const auto result = bloom::project::encodeCanonicalManifest(manifest, output);
    expectations.expect(
        size && *size.value() == expected.size() && result &&
            result.bytesWritten() == *size.value() &&
            writtenView(output, result.bytesWritten()) == expected &&
            std::ranges::all_of(output.begin() + static_cast<std::ptrdiff_t>(expected.size()),
                                output.end(), [](const char value) { return value == '?'; }),
        "non-empty requirement records use their exact fixed shape and call order");
}

void testFixedConstantsAndSchemaVersions(Expectations& expectations) {
    auto expectError = [&](const CanonicalManifestV1& manifest, const CanonicalManifestError error,
                           const std::string_view message) {
        const auto result = bloom::project::canonicalManifestSize(manifest);
        expectations.expect(!result && result.error() == error, message);
    };

    auto manifest = CanonicalManifestV1{};
    manifest.format = "org.kinetik.bloom.project\n";
    expectError(manifest, CanonicalManifestError::InvalidFormat,
                "the format constant is exact and never escaped into output");
    manifest = {};
    manifest.containerVersion = {0, 0};
    expectError(manifest, CanonicalManifestError::InvalidContainerVersion,
                "container schema zero is rejected");
    manifest = {};
    manifest.containerVersion = {1, 1};
    expectError(manifest, CanonicalManifestError::InvalidContainerVersion,
                "the v1 container minor is fixed");
    manifest = {};
    manifest.documentPath = "./document.json";
    expectError(manifest, CanonicalManifestError::InvalidDocumentPath,
                "the document path constant is exact");
    manifest = {};
    manifest.documentSchemaVersion = {0, 0};
    expectError(manifest, CanonicalManifestError::InvalidDocumentSchemaVersion,
                "document schema zero is rejected");
    manifest = {};
    manifest.documentSchemaVersion = {2, 0};
    expectError(manifest, CanonicalManifestError::InvalidDocumentSchemaVersion,
                "the manifest v1 document schema is fixed");
}

void testIdentifiersAndOrdering(Expectations& expectations) {
    auto checkRequirementError =
        [&](const std::vector<ManifestRequirement>& requirements,
            const CanonicalManifestError error, const std::size_t requirementIndex,
            const std::size_t nodeTypeIndex, const std::string_view message) {
            const auto result = bloom::project::canonicalManifestSize(
                CanonicalManifestV1{.requirements = requirements});
            expectations.expect(!result && result.error() == error &&
                                    result.requirementIndex() == requirementIndex &&
                                    result.nodeTypeIndex() == nodeTypeIndex,
                                message);
        };

    checkRequirementError({requirement("Provider", "provider.data")},
                          CanonicalManifestError::InvalidProviderId, 0,
                          bloom::project::kCanonicalManifestNoIndex,
                          "provider identifiers use exact namespaced syntax");
    checkRequirementError({requirement("provider", "provider.\"data")},
                          CanonicalManifestError::InvalidCapabilityId, 0,
                          bloom::project::kCanonicalManifestNoIndex,
                          "escape-capable capability text is rejected rather than encoded");
    checkRequirementError({requirement("provider", "provider.data", {}, {0, 1})},
                          CanonicalManifestError::InvalidRequirementSchemaVersion, 0,
                          bloom::project::kCanonicalManifestNoIndex,
                          "requirement schema major zero is rejected");
    checkRequirementError({requirement("provider.z", "provider.z.data"),
                           requirement("provider.a", "provider.a.data")},
                          CanonicalManifestError::InvalidRequirementOrder, 1,
                          bloom::project::kCanonicalManifestNoIndex,
                          "requirement records must already be ordered by unsigned UTF-8 bytes");
    checkRequirementError(
        {requirement("provider", "provider.data", {}, {1, 0}),
         requirement("provider", "provider.data", {}, {2, 0})},
        CanonicalManifestError::DuplicateRequirementIdentity, 1,
        bloom::project::kCanonicalManifestNoIndex,
        "provider and capability identity is unique regardless of schema version");
    checkRequirementError(
        {requirement("provider", "provider.data", {"provider.valid", "provider/invalid"})},
        CanonicalManifestError::InvalidProvidedNodeTypeId, 0, 1,
        "provided node types use exact namespaced syntax");
    checkRequirementError({requirement("provider", "provider.data", {"provider.z", "provider.a"})},
                          CanonicalManifestError::InvalidProvidedNodeTypeOrder, 0, 1,
                          "provided node types must already be sorted by unsigned UTF-8 bytes");
    checkRequirementError({requirement("provider", "provider.data", {"provider.a", "provider.a"})},
                          CanonicalManifestError::DuplicateProvidedNodeTypeId, 0, 1,
                          "provided node types are duplicate-free");
}

void testLimitsAndSizeAccounting(Expectations& expectations) {
    const std::vector requirements{
        requirement("provider.a", "provider.a.data", {"provider.a.one", "provider.a.two"}),
        requirement("provider.b", "provider.b.data"),
    };
    const CanonicalManifestV1 manifest{.requirements = requirements};

    auto limits = CanonicalManifestLimits{};
    limits.maximumRequirements = 1;
    expectations.expect(bloom::project::canonicalManifestSize(manifest, limits).error() ==
                            CanonicalManifestError::RequirementCountExceeded,
                        "a lower requirement-count budget rejects before sizing");

    limits = {};
    limits.maximumProvidedNodeTypes = 1;
    const auto nodeCount = bloom::project::canonicalManifestSize(manifest, limits);
    expectations.expect(
        !nodeCount && nodeCount.error() == CanonicalManifestError::ProvidedNodeTypeCountExceeded &&
            nodeCount.requirementIndex() == 0,
        "a lower per-list count budget reports the owning requirement");

    limits = {};
    limits.maximumValues = 10;
    const auto tenValueSize = bloom::project::canonicalManifestSize(CanonicalManifestV1{}, limits);
    std::array<char, 512> tenValueOutput{};
    tenValueOutput.fill('?');
    const auto tenValueWrite =
        bloom::project::encodeCanonicalManifest(CanonicalManifestV1{}, tenValueOutput, limits);
    expectations.expect(
        !tenValueSize && tenValueSize.error() == CanonicalManifestError::ValueCountExceeded &&
            !tenValueWrite && tenValueWrite.error() == CanonicalManifestError::ValueCountExceeded &&
            tenValueWrite.bytesWritten() == 0 &&
            std::ranges::all_of(tenValueOutput, [](const char value) { return value == '?'; }),
        "the fixed empty manifest accounts for all eleven JSON values before writing");
    limits.maximumValues = 11;
    std::array<char, 512> elevenValueOutput{};
    const auto elevenValueSize =
        bloom::project::canonicalManifestSize(CanonicalManifestV1{}, limits);
    const auto elevenValueWrite =
        bloom::project::encodeCanonicalManifest(CanonicalManifestV1{}, elevenValueOutput, limits);
    expectations.expect(elevenValueSize && elevenValueWrite &&
                            elevenValueWrite.bytesWritten() == *elevenValueSize.value(),
                        "an exact eleven-value budget sizes and writes the empty manifest");

    const auto exact = bloom::project::canonicalManifestSize(manifest);
    expectations.expect(static_cast<bool>(exact),
                        "the size-limit fixture has a valid default preflight");
    if (!exact) {
        return;
    }
    limits = {};
    limits.maximumOutputBytes = *exact.value() - 1;
    expectations.expect(bloom::project::canonicalManifestSize(manifest, limits).error() ==
                            CanonicalManifestError::ManifestSizeExceeded,
                        "the exact canonical byte count is checked against the manifest limit");
    limits.maximumOutputBytes = *exact.value();
    expectations.expect(static_cast<bool>(bloom::project::canonicalManifestSize(manifest, limits)),
                        "an output limit equal to the exact canonical size succeeds");

    limits = {};
    limits.maximumRequirements = bloom::project::kMaxManifestRequirementCount + 1;
    expectations.expect(bloom::project::canonicalManifestSize(manifest, limits).error() ==
                            CanonicalManifestError::InvalidLimits,
                        "caller budgets cannot raise the accepted v1 ceilings");
}

void testTransactionalOutput(Expectations& expectations) {
    const std::vector requirements{requirement(
        "provider", "provider.data", {"provider.node"},
        {std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max()})};
    const CanonicalManifestV1 manifest{.requirements = requirements};
    const auto size = bloom::project::canonicalManifestSize(manifest);
    if (!size) {
        expectations.expect(false, "the transactional fixture preflights");
        return;
    }

    std::vector<char> shortOutput(*size.value() - 1, '?');
    const auto shortResult = bloom::project::encodeCanonicalManifest(manifest, shortOutput);
    expectations.expect(
        !shortResult && shortResult.error() == CanonicalManifestError::OutputCapacityExceeded &&
            shortResult.requiredSize() == *size.value() && shortResult.bytesWritten() == 0 &&
            std::ranges::all_of(shortOutput, [](const char value) { return value == '?'; }),
        "capacity shortage reports exact bytes and leaves the destination untouched");

    auto invalid = requirements;
    invalid.front().providerId = "invalid\nprovider";
    std::vector<char> invalidOutput(*size.value(), '?');
    const auto invalidResult = bloom::project::encodeCanonicalManifest(
        CanonicalManifestV1{.requirements = invalid}, invalidOutput);
    expectations.expect(
        !invalidResult && invalidResult.error() == CanonicalManifestError::InvalidProviderId &&
            invalidResult.bytesWritten() == 0 &&
            std::ranges::all_of(invalidOutput, [](const char value) { return value == '?'; }),
        "lexical failure completes before any destination byte is touched");

    std::vector<char> exactOutput(*size.value(), '?');
    const auto exactResult = bloom::project::encodeCanonicalManifest(manifest, exactOutput);
    expectations.expect(
        exactResult && exactResult.bytesWritten() == *size.value() &&
            writtenView(exactOutput, exactResult.bytesWritten()).find("4294967295") !=
                std::string_view::npos &&
            exactOutput.back() == '\n',
        "exact capacity emits canonical uint32 extrema and one final LF");
}

} // namespace

int main() {
    try {
        Expectations expectations;
        testEmptyGoldenBytes(expectations);
        testNonEmptyGoldenBytes(expectations);
        testFixedConstantsAndSchemaVersions(expectations);
        testIdentifiersAndOrdering(expectations);
        testLimitsAndSizeAccounting(expectations);
        testTransactionalOutput(expectations);
        return expectations.failures() == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected test exception: " << error.what() << '\n';
        return 1;
    }
}
