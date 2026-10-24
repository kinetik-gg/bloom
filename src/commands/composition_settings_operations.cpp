#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/commands/operations.hpp>

#include <bloom/document/animation.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/project.hpp>

#include <limits>
#include <utility>
#include <vector>

namespace bloom::commands {
namespace {

// Moves one position parameter by `delta`: its constant, or every key and the fallback value of
// its component curves. A driven position follows its driver and is left alone.
[[nodiscard]] bool shiftPosition(document::Composition& composition, const document::ParameterId id,
                                 const document::Vec2d delta) {
    const auto* record = composition.parameters().find(id);
    if (record == nullptr)
        return true;
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&record->source)) {
        const auto* value = std::get_if<document::Vec2d>(&constant->value);
        if (value == nullptr)
            return true;
        return composition.parameters().setSource(id, document::ConstantValueSource{document::Vec2d{
                                                          value->x + delta.x, value->y + delta.y}});
    }
    const auto* animated = std::get_if<document::AnimationCurveSource>(&record->source);
    if (animated == nullptr)
        return true;
    auto source = *animated;
    if (source.defaultValue)
        if (const auto* fallback = std::get_if<document::Vec2d>(&*source.defaultValue))
            source.defaultValue = document::Vec2d{fallback->x + delta.x, fallback->y + delta.y};
    const auto* curve = composition.animationCurves().findVec2(source.curveId);
    if (curve == nullptr)
        return composition.parameters().setSource(id, std::move(source));
    const auto copy = *curve;
    for (const auto component :
         {document::AnimationComponent::X, document::AnimationComponent::Y}) {
        const auto* keys = copy.component(component);
        if (keys == nullptr)
            continue;
        const double offset = component == document::AnimationComponent::X ? delta.x : delta.y;
        for (auto key : keys->keyframes) {
            key.value += offset;
            if (!composition.animationCurves().updateKeyframe(copy.id, component, key))
                return false;
        }
    }
    return composition.parameters().setSource(id, std::move(source));
}

} // namespace

CompositionSettings compositionSettingsOf(const document::Composition& composition) {
    return CompositionSettings{
        composition.name(),     composition.format(),          composition.duration(),
        composition.timecode(), composition.backgroundColor(), composition.workingColorSpaceId(),
        composition.safeAreas()};
}

std::optional<std::string> compositionSettingsIssue(const CompositionSettings& settings) {
    if (!document::isValidHumanFacingName(settings.name))
        return "The name must be 1 to 128 UTF-8 bytes";
    if (settings.duration <= core::RationalTime{})
        return "The duration must be greater than zero";
    if (!settings.timecode.isValid())
        return "The start timecode must not be negative";
    if (!settings.background.isValid())
        return "The background colour is invalid";
    if (settings.workingColorSpaceId &&
        !document::validateWorkingColorSpaceId(*settings.workingColorSpaceId).ok())
        return "The working colour space is invalid";
    if (!settings.safeAreas.isValid())
        return "Safe areas must be 1 to 100 percent, with title safe inside action safe";
    return std::nullopt;
}

std::string_view AddCompositionWithSettings::typeId() const noexcept {
    return "bloom.composition.add-with-settings";
}

OperationResult AddCompositionWithSettings::apply(document::Draft& draft) const {
    if (const auto issue = compositionSettingsIssue(settings_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue, *issue);
    const AddComposition add(settings_.name, settings_.format, settings_.format.frameRate(),
                             settings_.duration, settings_.background);
    auto result = add.apply(draft);
    if (result.status != OperationStatus::Applied)
        return result;
    for (const auto& output : result.outputs) {
        const auto* id = std::get_if<document::CompositionId>(&output.id);
        auto* composition = id ? draft.project().findComposition(*id) : nullptr;
        if (composition == nullptr)
            continue;
        composition->setTimecode(settings_.timecode);
        composition->setWorkingColorSpaceId(settings_.workingColorSpaceId);
        composition->setSafeAreas(settings_.safeAreas);
    }
    return result;
}

std::string_view SetCompositionSettings::typeId() const noexcept {
    return "bloom.composition.set-settings";
}

OperationResult SetCompositionSettings::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Composition does not exist");
    if (const auto issue = compositionSettingsIssue(settings_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue, *issue);
    if (!anchor_.isValid())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The resize anchor is not on the 3 x 3 grid");
    if (compositionSettingsOf(*composition) == settings_)
        return OperationResult::noChange();

    // Keep existing content pinned to the anchor: shift every unlocked top-level layer.
    const auto before = composition->format();
    const document::Vec2d delta{
        (static_cast<double>(settings_.format.width()) - static_cast<double>(before.width())) *
            static_cast<double>(anchor_.column) / 2.0,
        (static_cast<double>(settings_.format.height()) - static_cast<double>(before.height())) *
            static_cast<double>(anchor_.row) / 2.0};
    if (delta.x != 0.0 || delta.y != 0.0) {
        std::vector<document::ParameterId> positions;
        for (const auto& boundary : composition->graph().layerOutputs()) {
            if (boundary.parent || boundary.locked)
                continue;
            const auto* node = composition->graph().findNode(boundary.nodeId);
            if (node == nullptr)
                continue;
            for (const auto& binding : node->parameters)
                if (binding.role == document::kPositionParameterRole)
                    positions.push_back(binding.parameterId);
        }
        for (const auto id : positions)
            if (!shiftPosition(*composition, id, delta))
                return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                 "A layer position could not be moved");
    }

    composition->setName(settings_.name);
    composition->setFormat(settings_.format);
    if (!composition->setDuration(settings_.duration))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The duration must be greater than zero");
    if (const auto area = composition->workArea(); area && area->end > settings_.duration) {
        auto clamped = *area;
        clamped.end = settings_.duration;
        composition->setWorkArea(clamped.start < clamped.end ? std::optional(clamped)
                                                             : std::nullopt);
    }
    composition->setTimecode(settings_.timecode);
    composition->setBackgroundColor(settings_.background);
    composition->setWorkingColorSpaceId(settings_.workingColorSpaceId);
    composition->setSafeAreas(settings_.safeAreas);
    return OperationResult::applied();
}

std::string_view UpdateCompositionSettings::typeId() const noexcept {
    return "bloom.composition.update-settings";
}

OperationResult UpdateCompositionSettings::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Composition does not exist");
    auto settings = compositionSettingsOf(*composition);
    const auto current = composition->format();
    const auto format = document::CompositionFormat::create(
        change_.width.value_or(current.width()), change_.height.value_or(current.height()),
        change_.pixelAspect.value_or(current.pixelAspect()),
        change_.frameRate.value_or(current.frameRate()));
    if (!format)
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "Width and height must be 1 to 1048576 pixels and at most 4294967296 pixels in all");
    settings.format = *format;
    if (change_.name)
        settings.name = *change_.name;
    if (change_.duration)
        settings.duration = *change_.duration;
    if (change_.timecodeStart)
        settings.timecode.start = *change_.timecodeStart;
    if (change_.dropFrame)
        settings.timecode.dropFrame = *change_.dropFrame;
    if (change_.background)
        settings.background = *change_.background;
    if (change_.workingColorSpaceId)
        settings.workingColorSpaceId = change_.workingColorSpaceId->empty()
                                           ? std::nullopt
                                           : std::optional(*change_.workingColorSpaceId);
    if (change_.actionSafe)
        settings.safeAreas.action = *change_.actionSafe;
    if (change_.titleSafe)
        settings.safeAreas.title = *change_.titleSafe;
    return SetCompositionSettings(composition_, std::move(settings), anchor_).apply(draft);
}

CompositionSettings compositionSettingsForAsset(const document::AssetRecord& asset,
                                                CompositionSettings base) {
    base.name = asset.name.empty() ? base.name : asset.name;
    auto rate = base.format.frameRate();
    if (asset.kind == document::AssetKind::Video)
        for (const auto& stream : asset.videoStreams)
            if (stream.kind == 1 && stream.framePeriod.numerator() > 0 &&
                stream.framePeriod.denominator() > 0 &&
                stream.framePeriod.denominator() <= std::numeric_limits<std::uint32_t>::max() &&
                stream.framePeriod.numerator() <= std::numeric_limits<std::uint32_t>::max()) {
                // A frame period of n/d seconds is a rate of d/n frames per second.
                if (const auto streamRate = document::FrameRate::create(
                        static_cast<std::uint32_t>(stream.framePeriod.denominator()),
                        static_cast<std::uint32_t>(stream.framePeriod.numerator())))
                    rate = *streamRate;
                break;
            }
    const bool visual = asset.kind == document::AssetKind::Image ||
                        asset.kind == document::AssetKind::Sequence ||
                        asset.kind == document::AssetKind::Video;
    const auto width = visual && asset.width > 0 ? asset.width : base.format.width();
    const auto height = visual && asset.height > 0 ? asset.height : base.format.height();
    if (const auto format = document::CompositionFormat::create(
            width, height, visual ? core::PixelAspectRatio::square() : base.format.pixelAspect(),
            rate))
        base.format = *format;
    else if (const auto rated = document::CompositionFormat::create(
                 base.format.width(), base.format.height(), base.format.pixelAspect(), rate))
        base.format = *rated;
    if ((asset.kind == document::AssetKind::Video || asset.kind == document::AssetKind::Audio) &&
        asset.duration > core::RationalTime{})
        base.duration = asset.duration;
    else if (asset.kind == document::AssetKind::Sequence && !asset.manifest.members.empty())
        if (const auto frames = core::RationalTime::create(
                static_cast<std::int64_t>(asset.manifest.members.size()) *
                    static_cast<std::int64_t>(base.format.frameRate().denominator()),
                static_cast<std::int64_t>(base.format.frameRate().numerator())))
            base.duration = *frames;
    return base;
}

std::string_view AddCompositionFromAssets::typeId() const noexcept {
    return "bloom.composition.add-from-assets";
}

OperationResult AddCompositionFromAssets::apply(document::Draft& draft) const {
    if (folder_ && draft.project().findAssetFolder(*folder_) == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "The destination folder does not exist");
    auto result = AddCompositionWithSettings(settings_).apply(draft);
    if (result.status != OperationStatus::Applied)
        return result;
    document::CompositionId composition;
    for (const auto& output : result.outputs)
        if (const auto* id = std::get_if<document::CompositionId>(&output.id))
            composition = *id;
    if (auto* created = draft.project().findComposition(composition))
        created->setFolder(folder_);
    // Each layer lands on top, so adding in reverse leaves the first asset uppermost.
    for (auto it = assets_.rbegin(); it != assets_.rend(); ++it) {
        const auto* asset = draft.project().findAsset(*it);
        if (asset == nullptr)
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "Asset does not exist");
        if (asset->kind == document::AssetKind::Lut || asset->kind == document::AssetKind::Font)
            continue;
        auto layer = asset->kind == document::AssetKind::Audio
                         ? AddAudioLayer(composition, *it).apply(draft)
                         : AddImageLayer(composition, *it).apply(draft);
        if (layer.status != OperationStatus::Applied)
            return layer;
        result.outputs.insert(result.outputs.end(), layer.outputs.begin(), layer.outputs.end());
    }
    return result;
}

} // namespace bloom::commands
