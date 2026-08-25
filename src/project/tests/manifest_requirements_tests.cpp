#include <bloom/project/manifest_requirements.hpp>

#include <bloom/core/rational_time.hpp>
#include <bloom/document/extension_records.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bloom::document::CompositionId;
using bloom::document::ExtensionRecord;
using bloom::document::ExtensionRecordId;
using bloom::document::NodeId;
using bloom::document::NoExtensionReferences;
using bloom::document::Project;
using bloom::document::SchemaVersion;
using bloom::document::ValidationCode;
using bloom::document::ValidationResult;
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

template <typename Id> [[nodiscard]] constexpr Id id(const std::uint64_t value) noexcept {
    return Id::fromRaw(value);
}

[[nodiscard]] bool hasIssue(const ValidationResult& validation, const ValidationCode code,
                            const std::string_view path = {}) {
    return std::ranges::any_of(validation.issues(), [&](const auto& issue) {
        return issue.code == code && (path.empty() || issue.path == path);
    });
}

[[nodiscard]] Project makeProject() {
    return bloom::document::makeNewProject("Project", "Composition",
                                           bloom::core::RationalTime::fromInteger(1))
        .project;
}

void addCustomNode(Project& project, const std::string_view typeId) {
    auto* composition = project.findComposition(id<CompositionId>(1));
    if (composition == nullptr ||
        !composition->graph().addNode({id<NodeId>(100), std::string(typeId), {}, 1})) {
        throw std::logic_error("Could not build requirement node fixture");
    }
}

[[nodiscard]] ManifestRequirement requirement(std::string provider, std::string capability,
                                              std::vector<std::string> nodeTypes = {},
                                              const SchemaVersion version = {1, 0}) {
    return {.providerId = std::move(provider),
            .capabilityId = std::move(capability),
            .schemaVersion = version,
            .providedNodeTypeIds = std::move(nodeTypes)};
}

void testFoundationAndExactNodeCoverage(Expectations& expectations) {
    auto foundationOnly = makeProject();
    expectations.expect(bloom::project::validateManifestRequirements(foundationOnly, {}).ok(),
                        "foundation-only project truth requires no provider manifest entries");

    auto custom = makeProject();
    addCustomNode(custom, "vendor.nodes.blur");
    const auto missing = bloom::project::validateManifestRequirements(custom, {});
    expectations.expect(hasIssue(missing, ValidationCode::MissingReference, "requirements"),
                        "a non-foundation node type requires exact manifest coverage");

    const std::vector valid{
        requirement("vendor.package", "vendor.package.nodes", {"vendor.nodes.blur"})};
    expectations.expect(bloom::project::validateManifestRequirements(custom, valid).ok(),
                        "one canonical requirement exactly covers one custom node type");

    const std::vector duplicateCoverage{
        requirement("provider.a", "provider.a.nodes", {"vendor.nodes.blur"}),
        requirement("provider.b", "provider.b.nodes", {"vendor.nodes.blur"}),
    };
    expectations.expect(
        hasIssue(bloom::project::validateManifestRequirements(custom, duplicateCoverage),
                 ValidationCode::SharedReference, "requirements[1].providedNodeTypeIds[0]"),
        "two providers cannot claim the same node type");
}

void testForbiddenAndOrphanNodeClaims(Expectations& expectations) {
    const auto project = makeProject();
    const std::vector claims{requirement("vendor.package", "vendor.package.nodes",
                                         {"bloom.layer-stack", "vendor.nodes.unused"})};
    const auto validation = bloom::project::validateManifestRequirements(project, claims);
    expectations.expect(hasIssue(validation, ValidationCode::InvalidValue,
                                 "requirements[0].providedNodeTypeIds[0]") &&
                            hasIssue(validation, ValidationCode::OrphanObject,
                                     "requirements[0].providedNodeTypeIds[1]"),
                        "foundation types and unused provider claims are both rejected");
}

void testExtensionOwnerCoverage(Expectations& expectations) {
    auto project = makeProject();
    ExtensionRecord record{
        .id = id<ExtensionRecordId>(1),
        .ownerId = "vendor.extension",
        .typeId = "vendor.extension.record",
        .schemaVersion = {1, 0},
        .subject = std::nullopt,
        .mediaType = "application/octet-stream",
        .referencePolicy = NoExtensionReferences{},
        .payload = {},
    };
    if (!project.addExtensionRecord(std::move(record))) {
        throw std::logic_error("Could not build extension requirement fixture");
    }

    expectations.expect(hasIssue(bloom::project::validateManifestRequirements(project, {}),
                                 ValidationCode::MissingReference, "requirements"),
                        "opaque extension truth requires a manifest entry for its owner");
    const std::vector covered{requirement("vendor.extension", "vendor.extension.project-data")};
    expectations.expect(bloom::project::validateManifestRequirements(project, covered).ok(),
                        "an extension-only capability may provide no node types");
}

void testRequirementSyntaxOrderingAndUniqueness(Expectations& expectations) {
    const auto project = makeProject();
    const std::vector malformed{
        requirement("vendor.z", "vendor.z.data"),
        requirement("Vendor.Invalid", "vendor.invalid/data"),
    };
    const auto malformedValidation =
        bloom::project::validateManifestRequirements(project, malformed);
    expectations.expect(
        hasIssue(malformedValidation, ValidationCode::InvalidOrder, "requirements[1]") &&
            hasIssue(malformedValidation, ValidationCode::InvalidValue,
                     "requirements[1].providerId") &&
            hasIssue(malformedValidation, ValidationCode::InvalidValue,
                     "requirements[1].capabilityId"),
        "requirement identifiers and canonical record ordering are enforced together");

    const std::vector duplicatePair{
        requirement("vendor.package", "vendor.package.data", {}, {1, 0}),
        requirement("vendor.package", "vendor.package.data", {}, {2, 0}),
    };
    expectations.expect(
        hasIssue(bloom::project::validateManifestRequirements(project, duplicatePair),
                 ValidationCode::DuplicateId, "requirements[1]"),
        "provider and capability identity is unique regardless of schema version");

    const std::vector invalidVersion{
        requirement("vendor.package", "vendor.package.data", {}, {0, 1})};
    expectations.expect(
        hasIssue(bloom::project::validateManifestRequirements(project, invalidVersion),
                 ValidationCode::InvalidValue, "requirements[0].schemaVersion"),
        "a requirement schema version has a nonzero major component");

    auto custom = makeProject();
    addCustomNode(custom, "vendor.nodes.blur");
    const std::vector unsortedTypes{requirement("vendor.package", "vendor.package.nodes",
                                                {"vendor.nodes.blur", "vendor.nodes.blur"})};
    expectations.expect(
        hasIssue(bloom::project::validateManifestRequirements(custom, unsortedTypes),
                 ValidationCode::InvalidOrder, "requirements[0].providedNodeTypeIds[1]"),
        "a requirement's node-type list is strictly sorted and duplicate-free");
}

} // namespace

int main() {
    try {
        Expectations expectations;
        testFoundationAndExactNodeCoverage(expectations);
        testForbiddenAndOrphanNodeClaims(expectations);
        testExtensionOwnerCoverage(expectations);
        testRequirementSyntaxOrderingAndUniqueness(expectations);
        return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
