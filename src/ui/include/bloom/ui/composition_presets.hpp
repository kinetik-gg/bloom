#pragma once

// Composition presets for the shared New Composition / Composition Settings dialog (#376).
//
// Built-in presets name a frame size and pixel aspect only, so choosing one keeps the frame rate,
// duration and every other setting; they match whenever the size and pixel aspect equal theirs.
// User presets are saved from the dialog and hold every portable setting -- size, pixel aspect,
// frame rate, duration, start timecode, drop-frame and background -- and match only when all of
// them are equal. The name and the working colour space stay with the composition.
//
// User presets are a per-user application setting (QSettings group `compositionPresets`), never
// project truth: a project opens the same on a machine that has none of them.

#include <bloom/commands/composition_settings_operations.hpp>

#include <QString>

#include <cstddef>
#include <optional>
#include <vector>

class QSettings;

namespace bloom::ui {

struct CompositionPreset final {
    QString name;
    bool builtIn = false;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    // User presets only.
    std::optional<document::FrameRate> frameRate;
    std::optional<core::RationalTime> duration;
    std::optional<document::CompositionTimecode> timecode;
    std::optional<core::Color4d> background;

    friend bool operator==(const CompositionPreset&, const CompositionPreset&) = default;
};

// HD 1080p, HD 720p, UHD 4K, DCI 2K, DCI 4K, Vertical 1080 x 1920 and Square 1080, in that order.
[[nodiscard]] std::vector<CompositionPreset> builtInCompositionPresets();

[[nodiscard]] std::vector<CompositionPreset> loadUserCompositionPresets(QSettings& settings);
void saveUserCompositionPresets(QSettings& settings, const std::vector<CompositionPreset>& presets);

// A user preset holding every portable setting of `settings`.
[[nodiscard]] CompositionPreset userPresetFrom(const QString& name,
                                               const commands::CompositionSettings& settings);

// `settings` with the preset's values applied (a built-in changes only size and pixel aspect).
[[nodiscard]] commands::CompositionSettings
applyCompositionPreset(const CompositionPreset& preset, commands::CompositionSettings settings);

[[nodiscard]] bool compositionPresetMatches(const CompositionPreset& preset,
                                            const commands::CompositionSettings& settings);

// The index of the first preset in `presets` that matches, or nullopt for Custom. Pass user
// presets before built-ins so the more specific match wins.
[[nodiscard]] std::optional<std::size_t>
matchingCompositionPreset(const std::vector<CompositionPreset>& presets,
                          const commands::CompositionSettings& settings);

// A plain reason a user preset name cannot be used (empty, too long, or already taken by another
// preset than `ignoring`), or nullopt.
[[nodiscard]] std::optional<QString>
userPresetNameIssue(const QString& name, const std::vector<CompositionPreset>& presets,
                    std::optional<std::size_t> ignoring = std::nullopt);

} // namespace bloom::ui
