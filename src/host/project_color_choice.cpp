#include <bloom/host/project_color_choice.hpp>

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>

#include <algorithm>
#include <array>
#include <variant>

namespace bloom::host {
namespace {

constexpr std::array<std::string_view, 1> kNeutralSpaces{document::kProcessColorSpaceIdV1};
constexpr std::array<std::string_view, 5> kAcesSpaces{
    "ACEScg", "ACES2065-1", "Linear Rec.709 (sRGB)", "Linear P3-D65", "Linear Rec.2020"};

} // namespace

std::optional<ProjectColorConfig> parseProjectColorConfig(const std::string_view name) {
    if (name == "neutral")
        return ProjectColorConfig::BloomNeutral;
    if (name == "aces")
        return ProjectColorConfig::AcesCg;
    if (name == "current")
        return ProjectColorConfig::Current;
    return std::nullopt;
}

std::string_view projectColorConfigName(const ProjectColorConfig config) noexcept {
    switch (config) {
    case ProjectColorConfig::BloomNeutral:
        return "neutral";
    case ProjectColorConfig::AcesCg:
        return "aces";
    case ProjectColorConfig::Current:
        break;
    }
    return "current";
}

ProjectColorConfig projectColorConfigOf(const document::ColorSettings& settings) {
    const auto* builtIn =
        std::get_if<document::BuiltInOcioConfigLocator>(&settings.ocioConfig.locator);
    if (builtIn != nullptr && builtIn->uri == color::kAcesCgV1ConfigUri)
        return ProjectColorConfig::AcesCg;
    if (builtIn != nullptr && builtIn->uri == color::kBloomNeutralV1ConfigUri)
        return ProjectColorConfig::BloomNeutral;
    return ProjectColorConfig::Current;
}

std::string_view workingColorSpaceLabel(const std::string_view id) noexcept {
    if (id == document::kProcessColorSpaceIdV1)
        return "Linear Rec.709 (sRGB)";
    return id;
}

std::span<const std::string_view> projectWorkingColorSpaces(const ProjectColorConfig config) {
    switch (config) {
    case ProjectColorConfig::BloomNeutral:
        return kNeutralSpaces;
    case ProjectColorConfig::AcesCg:
        return kAcesSpaces;
    case ProjectColorConfig::Current:
        break;
    }
    return {};
}

ProjectColorResolution resolveProjectColorSettings(const document::ColorSettings& original,
                                                   const ProjectColorConfig config,
                                                   std::optional<std::string> workingColorSpaceId) {
    const auto current = projectColorConfigOf(original);
    const bool keep = config == ProjectColorConfig::Current || config == current;
    const auto offered = projectWorkingColorSpaces(keep ? current : config);
    std::string working = workingColorSpaceId.value_or(
        keep ? original.processColorSpaceId
             : std::string(offered.empty() ? std::string_view{} : offered.front()));
    // The project's own working space stays valid while its configuration stays, even when the
    // dialog would not offer it (a project authored elsewhere).
    const bool own = keep && working == original.processColorSpaceId;
    if (!own && !offered.empty() && std::ranges::find(offered, working) == offered.end())
        return {std::nullopt, "The working color space \"" + working +
                                  "\" is not offered by that configuration"};
    if (keep) {
        auto settings = original;
        settings.processColorSpaceId = std::move(working);
        return {std::move(settings), {}};
    }
    if (config == ProjectColorConfig::AcesCg) {
        const auto revision = color::ocioBuiltInContentRevision(
            color::OcioConfigLocatorKind::BloomBuiltIn, color::kAcesCgV1ConfigUri);
        if (!revision.has_value())
            return {std::nullopt, "The built-in ACES configuration is unavailable in this build, "
                                  "so it cannot be selected."};
        return {document::ColorSettings{
                    .schemaVersion = document::kColorSettingsSchemaVersionV1,
                    .processColorSpaceId = std::move(working),
                    .ocioConfig = {.schemaVersion = document::kOcioConfigReferenceSchemaVersionV1,
                                   .locator = document::BuiltInOcioConfigLocator{std::string(
                                       color::kAcesCgV1ConfigUri)},
                                   .expectedRevision = {.algorithm =
                                                            document::OcioRevisionAlgorithm::Sha256,
                                                        .digest = *revision},
                                   .portability = document::OcioConfigPortability::BuiltIn,
                                   .contextVariables = {}}},
                {}};
    }
    auto settings = document::makeBloomNeutralColorSettingsV1(color::kBloomNeutralV1ConfigDigest);
    settings.processColorSpaceId = std::move(working);
    return {std::move(settings), {}};
}

} // namespace bloom::host
