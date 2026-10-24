#pragma once

// Composition Settings (#376): every authored composition setting -- name, size, pixel aspect,
// frame rate, duration, timecode labelling, background, working colour space and safe areas --
// created or edited as ONE operation, so the shared New Composition / Composition Settings dialog
// and scripting both make exactly one undoable change.
//
// Time stays layer-owned (ADR 0024). Every layer time, range and keyframe is an exact rational time
// in seconds; a frame-rate change rewrites none of them, so a layer keeps playing the same content
// at the same instant. Keys that sat on a frame of the old rate keep their exact times and may fall
// between frames of the new rate (the playhead still snaps to the new frames). A duration change
// moves nothing either; a work area that would end after the new duration is clamped to it (and
// cleared if nothing of it remains). A size change keeps existing content pinned to a resize
// anchor: every top-level layer's position (its constant and every key) shifts by the size change
// times the anchor's fraction, so the content keeps its place relative to that edge or the centre.

#include <bloom/commands/operation.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::document {
class Composition;
struct AssetRecord;
} // namespace bloom::document

namespace bloom::commands {

struct CompositionSettings final {
    std::string name;
    document::CompositionFormat format;
    core::RationalTime duration;
    document::CompositionTimecode timecode{};
    core::Color4d background{0.0, 0.0, 0.0, 1.0};
    // nullopt follows the project working colour space.
    std::optional<std::string> workingColorSpaceId{};
    document::SafeAreaSettings safeAreas{};

    friend bool operator==(const CompositionSettings&, const CompositionSettings&) = default;
};

// The settings a composition currently holds.
[[nodiscard]] CompositionSettings compositionSettingsOf(const document::Composition& composition);

// A plain reason when the settings cannot be authored, nullopt when they can.
[[nodiscard]] std::optional<std::string>
compositionSettingsIssue(const CompositionSettings& settings);

// Where existing content stays when the size changes: a 3 x 3 grid, column and row 0 (left / top),
// 1 (centre) or 2 (right / bottom).
struct ResizeAnchor final {
    std::uint8_t column = 1;
    std::uint8_t row = 1;

    [[nodiscard]] bool isValid() const noexcept { return column <= 2 && row <= 2; }
    friend bool operator==(const ResizeAnchor&, const ResizeAnchor&) = default;
};

// Adds a composition with every setting at once. Output: kAddCompositionOutput.
class AddCompositionWithSettings final : public Operation {
  public:
    explicit AddCompositionWithSettings(CompositionSettings settings)
        : settings_(std::move(settings)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    CompositionSettings settings_;
};

// Replaces every setting of one composition. A whole-render change (the default footprint): size,
// rate and duration reach every frame and every composition that nests this one.
class SetCompositionSettings final : public Operation {
  public:
    SetCompositionSettings(document::CompositionId composition, CompositionSettings settings,
                           ResizeAnchor anchor = {})
        : composition_(composition), settings_(std::move(settings)), anchor_(anchor) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    CompositionSettings settings_;
    ResizeAnchor anchor_;
};

// Changes only the named settings and keeps the rest (the scripting form, which names what it
// edits without reading the document first). Everything else is SetCompositionSettings.
struct CompositionSettingsChange final {
    std::optional<std::string> name;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::optional<core::PixelAspectRatio> pixelAspect;
    std::optional<document::FrameRate> frameRate;
    std::optional<core::RationalTime> duration;
    std::optional<core::RationalTime> timecodeStart;
    std::optional<bool> dropFrame;
    std::optional<core::Color4d> background;
    // An empty string follows the project working colour space.
    std::optional<std::string> workingColorSpaceId;
    std::optional<double> actionSafe;
    std::optional<double> titleSafe;
};

class UpdateCompositionSettings final : public Operation {
  public:
    UpdateCompositionSettings(document::CompositionId composition, CompositionSettingsChange change,
                              ResizeAnchor anchor = {})
        : composition_(composition), change_(std::move(change)), anchor_(anchor) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    CompositionSettingsChange change_;
    ResizeAnchor anchor_;
};

// New Composition from Selection (#378), as in After Effects: the composition takes the footage's
// name and size, a video's frame rate and duration, a sequence's frame count at that rate and an
// audio file's duration. Whatever the footage does not state comes from `base`.
[[nodiscard]] CompositionSettings compositionSettingsForAsset(const document::AssetRecord& asset,
                                                              CompositionSettings base);

// Adds one composition with `settings`, files it in `folder` (nullopt is the Assets root) and adds
// a layer for each image, sequence, video or audio asset, the first asset on top. One undoable
// step. Output: kAddCompositionOutput, then each layer's outputs.
class AddCompositionFromAssets final : public Operation {
  public:
    AddCompositionFromAssets(CompositionSettings settings, std::vector<document::AssetId> assets,
                             std::optional<document::AssetFolderId> folder = {})
        : settings_(std::move(settings)), assets_(std::move(assets)), folder_(folder) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    CompositionSettings settings_;
    std::vector<document::AssetId> assets_;
    std::optional<document::AssetFolderId> folder_;
};

} // namespace bloom::commands
