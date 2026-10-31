#pragma once
// Project Settings > Colour as Qt-free choices: which OCIO configuration a project uses and its
// working colour space. The Project Settings dialog, Python's Host.set_color_settings and the MCP
// tool project.set-color-settings all resolve a choice here, so they accept the same values and
// build the same settings.
#include <bloom/document/color_settings.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace bloom::host {

enum class ProjectColorConfig : std::uint8_t {
    // Keep the project's own configuration (an external one can only be kept).
    Current,
    BloomNeutral,
    AcesCg,
};

// "neutral", "aces" or "current"; nullopt for anything else.
[[nodiscard]] std::optional<ProjectColorConfig> parseProjectColorConfig(std::string_view name);
[[nodiscard]] std::string_view projectColorConfigName(ProjectColorConfig config) noexcept;

// The built-in configuration `settings` uses, or Current for an external one.
[[nodiscard]] ProjectColorConfig projectColorConfigOf(const document::ColorSettings& settings);

// The working colour spaces the dialog offers for a built-in configuration, first the default;
// empty for Current, whose working space is the project's own.
[[nodiscard]] std::span<const std::string_view>
projectWorkingColorSpaces(ProjectColorConfig config);

// The name an artist reads for a working color space id. Bloom Neutral's scene-linear Rec.709
// space is stored by its interchange identifier lin_rec709_scene and reads "Linear Rec.709
// (sRGB)", the ACES config's own name for the same space; every other id is already a name and
// comes back unchanged. Display only: the stored id, the config and its revision never change.
// The dialogs, the Viewer probe, MCP and Python read-back all use this one mapping.
[[nodiscard]] std::string_view workingColorSpaceLabel(std::string_view id) noexcept;

struct ProjectColorResolution final {
    std::optional<document::ColorSettings> settings;
    // Why `settings` is absent, for people.
    std::string reason;
};

// The settings a choice describes. Choosing the configuration the project already uses (or
// Current) keeps it, with its revision and context, and changes only the working space; choosing
// another built-in configuration starts from its qualified revision. An omitted working space keeps
// the project's when the configuration stays and takes the configuration's default otherwise.
[[nodiscard]] ProjectColorResolution
resolveProjectColorSettings(const document::ColorSettings& original, ProjectColorConfig config,
                            std::optional<std::string> workingColorSpaceId);

} // namespace bloom::host
