#include <bloom/document/color_settings.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bloom::document::BuiltInOcioConfigLocator;
using bloom::document::ColorSettings;
using bloom::document::ExternalOcioConfigLocator;
using bloom::document::ExternalOciozLocator;
using bloom::document::OcioConfigLocator;
using bloom::document::OcioConfigPortability;
using bloom::document::OcioConfigReference;
using bloom::document::OcioContextVariable;
using bloom::document::OcioRevisionAlgorithm;
using bloom::document::ProjectRelativeOciozLocator;
using bloom::document::SchemaVersion;
using bloom::document::ValidationCode;
using bloom::document::ValidationResult;

static_assert(std::is_copy_constructible_v<ColorSettings>);
static_assert(std::is_nothrow_move_constructible_v<ColorSettings>);
static_assert(std::is_same_v<decltype(std::declval<OcioConfigReference>().expectedRevision.digest),
                             bloom::core::Sha256Digest>);

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

[[nodiscard]] constexpr bloom::core::Sha256Digest testDigest() noexcept {
    return bloom::core::Sha256Digest::fromBytes({
        0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA, 0x41, 0x41, 0x40,
        0xDE, 0x5D, 0xAE, 0x22, 0x23, 0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17,
        0x7A, 0x9C, 0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD,
    });
}

[[nodiscard]] OcioConfigReference
makeReference(OcioConfigLocator locator, const OcioConfigPortability portability,
              std::vector<OcioContextVariable> contextVariables = {}) {
    return {
        .schemaVersion = bloom::document::kOcioConfigReferenceSchemaVersionV1,
        .locator = std::move(locator),
        .expectedRevision =
            {
                .algorithm = OcioRevisionAlgorithm::Sha256,
                .digest = testDigest(),
            },
        .portability = portability,
        .contextVariables = std::move(contextVariables),
    };
}

[[nodiscard]] bool hasIssueAt(const ValidationResult& result, const ValidationCode code,
                              const std::string_view path) {
    return std::ranges::any_of(result.issues(), [code, path](const auto& issue) {
        return issue.code == code && issue.path == path;
    });
}

void testBuildSuppliedBloomNeutralValue(Expectations& expectations) {
    const auto digest = testDigest();
    const auto settings = bloom::document::makeBloomNeutralColorSettingsV1(digest);
    const auto* locator = std::get_if<BuiltInOcioConfigLocator>(&settings.ocioConfig.locator);
    expectations.expect(
        settings.validate().ok() &&
            settings.schemaVersion == bloom::document::kColorSettingsSchemaVersionV1 &&
            settings.processColorSpaceId == bloom::document::kProcessColorSpaceIdV1 &&
            locator != nullptr && locator->uri == bloom::document::kBloomNeutralConfigUriV1 &&
            settings.ocioConfig.schemaVersion ==
                bloom::document::kOcioConfigReferenceSchemaVersionV1 &&
            settings.ocioConfig.expectedRevision.algorithm == OcioRevisionAlgorithm::Sha256 &&
            settings.ocioConfig.expectedRevision.digest == digest &&
            settings.ocioConfig.portability == OcioConfigPortability::BuiltIn &&
            settings.ocioConfig.contextVariables.empty(),
        "the Bloom Neutral factory fixes v1 semantics around the caller-supplied revision");

    auto copied = settings;
    const auto moved = std::move(copied);
    expectations.expect(moved == settings,
                        "color settings retain complete value semantics across copy and move");

    ColorSettings uninitialized;
    expectations.expect(!uninitialized.validate().ok(),
                        "default construction is not a fake valid Bloom Neutral reference");
}

void testKnownLocatorFamilies(Expectations& expectations) {
    const std::array references{
        makeReference(
            BuiltInOcioConfigLocator{std::string(bloom::document::kBloomNeutralConfigUriV1)},
            OcioConfigPortability::BuiltIn),
        makeReference(ProjectRelativeOciozLocator{"color/config.ocioz"},
                      OcioConfigPortability::ProjectRelative),
        makeReference(ExternalOciozLocator{"file:///show/config.ocioz"},
                      OcioConfigPortability::External),
        makeReference(ExternalOcioConfigLocator{"file:///show/config.ocio"},
                      OcioConfigPortability::External),
    };
    for (const auto& reference : references) {
        expectations.expect(reference.validate().ok(),
                            "each closed v1 OCIO locator family validates");
    }

    expectations.expect(
        makeReference(ExternalOciozLocator{"file:/show/config.ocioz"},
                      OcioConfigPortability::External)
                .validate()
                .ok() &&
            makeReference(ExternalOciozLocator{"file:///C:/show/config.ocioz"},
                          OcioConfigPortability::External)
                .validate()
                .ok() &&
            makeReference(ExternalOciozLocator{"FILE://server/show/config.ocioz"},
                          OcioConfigPortability::External)
                .validate()
                .ok() &&
            makeReference(ExternalOcioConfigLocator{"file:///show/config%2Eocio"},
                          OcioConfigPortability::External)
                .validate()
                .ok(),
        "the safe v1 subset accepts POSIX, drive, UNC, scheme case, and percent spelling forms");
}

void testSchemaProcessAndRevisionContract(Expectations& expectations) {
    auto reference = makeReference(
        BuiltInOcioConfigLocator{std::string(bloom::document::kBloomNeutralConfigUriV1)},
        OcioConfigPortability::BuiltIn);
    reference.schemaVersion = SchemaVersion{1, 1};
    expectations.expect(
        hasIssueAt(reference.validate(), ValidationCode::InvalidValue, "schemaVersion"),
        "OCIO reference schema minor versions outside exact 1.0 are rejected");

    reference = makeReference(
        BuiltInOcioConfigLocator{std::string(bloom::document::kBloomNeutralConfigUriV1)},
        OcioConfigPortability::BuiltIn);
    reference.expectedRevision.algorithm = OcioRevisionAlgorithm::Unknown;
    expectations.expect(hasIssueAt(reference.validate(), ValidationCode::InvalidValue,
                                   "expectedRevision.algorithm"),
                        "an absent revision algorithm is rejected");
    reference = makeReference(
        BuiltInOcioConfigLocator{std::string(bloom::document::kBloomNeutralConfigUriV1)},
        OcioConfigPortability::BuiltIn);
    reference.expectedRevision.digest = bloom::core::Sha256Digest{};
    expectations.expect(reference.validate().ok(),
                        "all digest bit patterns remain structurally representable document data");

    auto settings = bloom::document::makeBloomNeutralColorSettingsV1(testDigest());
    settings.schemaVersion = SchemaVersion{2, 0};
    expectations.expect(
        hasIssueAt(settings.validate(), ValidationCode::InvalidValue, "schemaVersion"),
        "color settings require exact schema 1.0");
    settings = bloom::document::makeBloomNeutralColorSettingsV1(testDigest());
    settings.processColorSpaceId = "role_scene_linear";
    expectations.expect(
        hasIssueAt(settings.validate(), ValidationCode::InvalidValue, "processColorSpaceId"),
        "a role or alias cannot replace lin_rec709_scene");

    constexpr std::string_view uppercase =
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";
    constexpr std::string_view lowercase =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const auto parsed = bloom::core::Sha256Digest::fromLowercaseHex(lowercase);
    expectations.expect(parsed.has_value() && *parsed == testDigest() &&
                            !bloom::core::Sha256Digest::fromLowercaseHex(uppercase).has_value(),
                        "digest text enters the model only through the strict lowercase core type");
}

void testBuiltInAndPortabilityRejection(Expectations& expectations) {
    for (const auto invalidUri : std::array<std::string_view, 6>{
             "",
             "bloom://ocio/default/config.ocio",
             "bloom://ocio/latest/config.ocio",
             "bloom://ocio/neutral-v1/config.ocio?latest=1",
             "bloom://ocio/neutral-v1/config.ocio#display",
             "BLOOM://ocio/neutral-v1/config.ocio",
         }) {
        const auto reference = makeReference(BuiltInOcioConfigLocator{std::string(invalidUri)},
                                             OcioConfigPortability::BuiltIn);
        expectations.expect(
            hasIssueAt(reference.validate(), ValidationCode::InvalidValue, "locator.uri"),
            "only the exact immutable Bloom Neutral URI is accepted");
    }

    const std::array mismatches{
        makeReference(
            BuiltInOcioConfigLocator{std::string(bloom::document::kBloomNeutralConfigUriV1)},
            OcioConfigPortability::External),
        makeReference(ProjectRelativeOciozLocator{"config.ocioz"}, OcioConfigPortability::BuiltIn),
        makeReference(ExternalOciozLocator{"file:///show/config.ocioz"},
                      OcioConfigPortability::ProjectRelative),
        makeReference(ExternalOcioConfigLocator{"file:///show/config.ocio"},
                      OcioConfigPortability::Unknown),
    };
    for (const auto& reference : mismatches) {
        expectations.expect(
            hasIssueAt(reference.validate(), ValidationCode::TypeMismatch, "portability"),
            "locator family and portability cannot disagree");
    }
}

void testProjectRelativePathProfile(Expectations& expectations) {
    const std::array invalidPaths{
        std::string{},
        std::string{"/color/config.ocioz"},
        std::string{"C:/color/config.ocioz"},
        std::string{"C:\\color\\config.ocioz"},
        std::string{"color//config.ocioz"},
        std::string{"./config.ocioz"},
        std::string{"../config.ocioz"},
        std::string{"color/./config.ocioz"},
        std::string{"color/../config.ocioz"},
        std::string{"color/config.OCIOZ"},
        std::string{"color/config.ocio"},
        std::string{"color/\0config.ocioz", 19},
        std::string{"color/\xC3\x28.ocioz", 14},
    };
    for (const auto& path : invalidPaths) {
        const auto reference = makeReference(ProjectRelativeOciozLocator{path},
                                             OcioConfigPortability::ProjectRelative);
        expectations.expect(
            hasIssueAt(reference.validate(), ValidationCode::InvalidValue, "locator.path"),
            "hostile or non-normalized project-relative paths are rejected");
    }

    constexpr std::string_view suffix = ".ocioz";
    std::string maximumPath(bloom::document::kMaxOcioProjectRelativePathBytes - suffix.size(), 'a');
    maximumPath.append(suffix);
    expectations.expect(makeReference(ProjectRelativeOciozLocator{maximumPath},
                                      OcioConfigPortability::ProjectRelative)
                            .validate()
                            .ok(),
                        "a normalized project-relative path at the exact byte ceiling is accepted");
    maximumPath.insert(maximumPath.begin(), 'a');
    expectations.expect(hasIssueAt(makeReference(ProjectRelativeOciozLocator{maximumPath},
                                                 OcioConfigPortability::ProjectRelative)
                                       .validate(),
                                   ValidationCode::InvalidValue, "locator.path"),
                        "a project-relative path above 4096 bytes is rejected");
}

void testExternalFileUriProfile(Expectations& expectations) {
    const std::array invalidArchiveUris{
        std::string{""},
        std::string{"https://example/config.ocioz"},
        std::string{"file:relative/config.ocioz"},
        std::string{"file://user@host/show/config.ocioz"},
        std::string{"file://host:80/show/config.ocioz"},
        std::string{"file://[]/show/config.ocioz"},
        std::string{"file://[::1]/show/config.ocioz"},
        std::string{"file:///show/config.ocioz?revision=1"},
        std::string{"file:///show/config.ocioz#fragment"},
        std::string{"file:///show/config ocioz"},
        std::string{"file:///show/config%ZZ.ocioz"},
        std::string{"file:///show/config%00.ocioz"},
        std::string{"file:///show%2Fconfig.ocioz"},
        std::string{"file:///show/config.ocio"},
        std::string{"file:///show/\0config.ocioz", 26},
        std::string{"file:///show/\xC3\xA9.ocioz", 21},
    };
    for (const auto& uri : invalidArchiveUris) {
        const auto reference =
            makeReference(ExternalOciozLocator{uri}, OcioConfigPortability::External);
        expectations.expect(
            hasIssueAt(reference.validate(), ValidationCode::InvalidValue, "locator.uri"),
            "invalid, ambiguous, or non-ASCII archive file URIs are rejected");
    }

    for (const auto invalidConfigUri : std::array<std::string_view, 4>{
             "file:///show/not-config.ocio",
             "file:///show/config.ocioz",
             "file:///show/CONFIG.ocio",
             "file:///show/config.ocio/",
         }) {
        const auto reference =
            makeReference(ExternalOcioConfigLocator{std::string(invalidConfigUri)},
                          OcioConfigPortability::External);
        expectations.expect(
            hasIssueAt(reference.validate(), ValidationCode::InvalidValue, "locator.uri"),
            "an external loose locator must name exact config.ocio");
    }

    constexpr std::string_view prefix = "file:///";
    constexpr std::string_view suffix = ".ocioz";
    std::string maximumUri(prefix);
    maximumUri.append(bloom::document::kMaxOcioExternalUriBytes - prefix.size() - suffix.size(),
                      'a');
    maximumUri.append(suffix);
    expectations.expect(
        makeReference(ExternalOciozLocator{maximumUri}, OcioConfigPortability::External)
            .validate()
            .ok(),
        "an external file URI at the exact ASCII byte ceiling is accepted");
    maximumUri.insert(maximumUri.end() - static_cast<std::ptrdiff_t>(suffix.size()), 'a');
    expectations.expect(
        hasIssueAt(makeReference(ExternalOciozLocator{maximumUri}, OcioConfigPortability::External)
                       .validate(),
                   ValidationCode::InvalidValue, "locator.uri"),
        "an external file URI above 16384 bytes is rejected");
}

void testContextVariables(Expectations& expectations) {
    auto reference = makeReference(
        ProjectRelativeOciozLocator{"color/config.ocioz"}, OcioConfigPortability::ProjectRelative,
        {{"A", ""}, {"A0_", "show"}, {"_ROOT", "plates/é"}, {"a", "value"}});
    expectations.expect(reference.validate().ok(),
                        "sorted explicit context variables permit empty and UTF-8 values");

    for (const auto& variable : std::array{
             OcioContextVariable{"", "value"},
             OcioContextVariable{"0NAME", "value"},
             OcioContextVariable{"BAD-NAME", "value"},
             OcioContextVariable{"NÉ", "value"},
             OcioContextVariable{std::string(129, 'A'), "value"},
         }) {
        reference.contextVariables = {variable};
        expectations.expect(hasIssueAt(reference.validate(),
                                       variable.name.empty() ? ValidationCode::EmptyKey
                                                             : ValidationCode::InvalidValue,
                                       "contextVariables[0].name"),
                            "context variable names enforce the closed ASCII identifier grammar");
    }

    reference.contextVariables = {{"A", std::string("bad\0value", 9)}};
    expectations.expect(
        hasIssueAt(reference.validate(), ValidationCode::InvalidValue, "contextVariables[0].value"),
        "context values reject embedded NUL");
    reference.contextVariables = {{"A", std::string("\xC3\x28", 2)}};
    expectations.expect(
        hasIssueAt(reference.validate(), ValidationCode::InvalidValue, "contextVariables[0].value"),
        "context values reject malformed UTF-8");
    reference.contextVariables = {{"A", std::string(4'097, 'v')}};
    expectations.expect(
        hasIssueAt(reference.validate(), ValidationCode::InvalidValue, "contextVariables[0].value"),
        "context values reject more than 4096 UTF-8 bytes");
    reference.contextVariables = {{"A", std::string(4'096, 'v')}};
    expectations.expect(reference.validate().ok(),
                        "context values accept the exact 4096-byte ceiling");

    reference.contextVariables = {{"B", "one"}, {"A", "two"}};
    expectations.expect(
        hasIssueAt(reference.validate(), ValidationCode::InvalidOrder, "contextVariables[1].name"),
        "context variables reject non-byte-sorted order");
    reference.contextVariables = {{"A", "one"}, {"A", "two"}};
    expectations.expect(
        hasIssueAt(reference.validate(), ValidationCode::InvalidOrder, "contextVariables[1].name"),
        "context variables reject duplicate names");

    reference.contextVariables.clear();
    reference.contextVariables.reserve(257);
    for (std::size_t index = 0; index < 257; ++index) {
        auto name = std::to_string(index);
        name.insert(name.begin(), 3 - name.size(), '0');
        reference.contextVariables.push_back({"V" + name, "value"});
    }
    expectations.expect(
        hasIssueAt(reference.validate(), ValidationCode::InvalidValue, "contextVariables"),
        "more than 256 explicit context variables are rejected");
    reference.contextVariables.back().name.clear();
    const auto boundedValidation = reference.validate();
    expectations.expect(
        boundedValidation.issues().size() == 1 &&
            hasIssueAt(boundedValidation, ValidationCode::InvalidValue, "contextVariables"),
        "validation does not inspect records beyond the closed 256-entry ceiling");
}

} // namespace

int main() try {
    Expectations expectations;
    testBuildSuppliedBloomNeutralValue(expectations);
    testKnownLocatorFamilies(expectations);
    testSchemaProcessAndRevisionContract(expectations);
    testBuiltInAndPortabilityRejection(expectations);
    testProjectRelativePathProfile(expectations);
    testExternalFileUriProfile(expectations);
    testContextVariables(expectations);
    return expectations.failures() == 0 ? 0 : 1;
} catch (const std::exception& exception) {
    std::cerr << "FAILED: unexpected exception: " << exception.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "FAILED: unexpected non-standard exception\n";
    return 1;
}
