#include "node_operation_support.hpp"

#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/persisted_text.hpp>

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace bloom::commands {
namespace {

using core::RationalTime;

struct Target final {
    document::Composition* composition = nullptr;
    document::LayerOutputBoundary* layer = nullptr;
};

[[nodiscard]] std::optional<OperationResult> findTarget(document::Draft& draft,
                                                        const document::CompositionId compositionId,
                                                        const document::LayerId layerId,
                                                        Target& target) {
    target.composition = draft.project().findComposition(compositionId);
    target.layer = target.composition ? target.composition->graph().findLayer(layerId) : nullptr;
    if (target.layer == nullptr)
        return detail::invalidTarget();
    if (target.layer->locked)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer is locked");
    return std::nullopt;
}

[[nodiscard]] document::LayerMask* findMask(document::LayerOutputBoundary& layer,
                                            const document::LayerMaskId id) {
    const auto found = std::ranges::find(layer.masks, id, &document::LayerMask::id);
    return found == layer.masks.end() ? nullptr : &*found;
}

// The layer's active span clipped to the composition, as a normalized footprint range.
void addSpan(std::vector<AffectedTimeRange>& ranges, const document::LayerOutputBoundary& layer,
             const RationalTime duration) {
    const auto start = std::max(layer.inPoint, RationalTime{});
    const auto end = std::min(layer.endPoint(duration), duration);
    if (start < end)
        ranges.push_back({start, end});
}

[[nodiscard]] OperationResult spanResult(const document::CompositionId composition,
                                         const document::LayerOutputBoundary& layer,
                                         const RationalTime duration,
                                         OperationResult result = OperationResult::applied()) {
    std::vector<AffectedTimeRange> ranges;
    addSpan(ranges, layer, duration);
    result.affectedTimes = normalizeAffectedTimeFootprint({composition, std::move(ranges)});
    return result;
}

[[nodiscard]] OperationResult invalid(std::string message) {
    return OperationResult::rejected(OperationIssueCode::InvalidValue, std::move(message));
}

// Erases a mask's parameters and the curves they own.
void eraseMaskParameters(document::Composition& composition, const document::LayerMask& mask) {
    for (const auto& [id, schemaKey] : document::layerMaskParameters(mask)) {
        (void)schemaKey;
        const auto* record = composition.parameters().find(id);
        std::optional<document::AnimationCurveId> curve;
        if (record != nullptr)
            if (const auto* source = std::get_if<document::AnimationCurveSource>(&record->source))
                curve = source->curveId;
        (void)composition.parameters().erase(id);
        if (curve)
            (void)composition.animationCurves().erase(*curve);
    }
}

} // namespace

std::string_view AddLayerMask::typeId() const noexcept { return "bloom.layer.add-mask"; }
OperationResult AddLayerMask::apply(document::Draft& draft) const {
    Target target;
    if (auto failure = findTarget(draft, composition_, layer_, target))
        return std::move(*failure);
    auto& composition = *target.composition;
    auto& layer = *target.layer;
    if (!path_.isValid())
        return invalid("Mask path is invalid");
    if (layer.masks.size() >= document::kMaximumLayerMasks)
        return invalid("A layer may hold at most 64 masks");
    auto name = name_.empty() ? "Mask " + std::to_string(layer.masks.size() + 1) : name_;
    if (!document::isValidHumanFacingName(name))
        return invalid("Mask name must be valid nonempty UTF-8");
    const auto maskId = draft.ids().allocateLayerMask();
    const auto pathId = draft.ids().allocateParameter();
    const auto featherId = draft.ids().allocateParameter();
    const auto expansionId = draft.ids().allocateParameter();
    const auto opacityId = draft.ids().allocateParameter();
    if (!maskId || !pathId || !featherId || !expansionId || !opacityId)
        return detail::exhaustedIds();
    auto& parameters = composition.parameters();
    const bool inserted =
        parameters.insert({*pathId, std::string(document::kLayerMaskPathSchemaKey),
                           document::ConstantValueSource{path_}}) &&
        parameters.insert({*featherId, std::string(document::kLayerMaskFeatherSchemaKey),
                           document::ConstantValueSource{0.0}}) &&
        parameters.insert({*expansionId, std::string(document::kLayerMaskExpansionSchemaKey),
                           document::ConstantValueSource{0.0}}) &&
        parameters.insert({*opacityId, std::string(document::kLayerMaskOpacitySchemaKey),
                           document::ConstantValueSource{1.0}});
    if (!inserted)
        return invalid("Mask parameters could not be created");
    layer.masks.push_back(
        {*maskId, std::move(name), mode_, false, *pathId, *featherId, *expansionId, *opacityId});
    return spanResult(composition_, layer, composition.duration(),
                      OperationResult::applied({{"mask", *maskId},
                                                {"path", *pathId},
                                                {"feather", *featherId},
                                                {"expansion", *expansionId},
                                                {"opacity", *opacityId}}));
}

std::string_view RemoveLayerMask::typeId() const noexcept { return "bloom.layer.remove-mask"; }
OperationResult RemoveLayerMask::apply(document::Draft& draft) const {
    Target target;
    if (auto failure = findTarget(draft, composition_, layer_, target))
        return std::move(*failure);
    auto& layer = *target.layer;
    const auto* mask = findMask(layer, mask_);
    if (mask == nullptr)
        return detail::invalidTarget();
    const auto removed = *mask;
    std::erase_if(layer.masks, [&](const auto& m) { return m.id == mask_; });
    eraseMaskParameters(*target.composition, removed);
    return spanResult(composition_, layer, target.composition->duration());
}

std::string_view MoveLayerMask::typeId() const noexcept { return "bloom.layer.move-mask"; }
OperationResult MoveLayerMask::apply(document::Draft& draft) const {
    Target target;
    if (auto failure = findTarget(draft, composition_, layer_, target))
        return std::move(*failure);
    auto& layer = *target.layer;
    if (findMask(layer, mask_) == nullptr || newIndex_ >= layer.masks.size())
        return detail::invalidTarget();
    const auto found = std::ranges::find(layer.masks, mask_, &document::LayerMask::id);
    const auto oldIndex = static_cast<std::size_t>(std::distance(layer.masks.begin(), found));
    if (oldIndex == newIndex_)
        return OperationResult::noChange();
    auto moved = std::move(*found);
    layer.masks.erase(found);
    layer.masks.insert(layer.masks.begin() + static_cast<std::ptrdiff_t>(newIndex_),
                       std::move(moved));
    return spanResult(composition_, layer, target.composition->duration());
}

std::string_view SetLayerMaskMode::typeId() const noexcept { return "bloom.layer.set-mask-mode"; }
OperationResult SetLayerMaskMode::apply(document::Draft& draft) const {
    Target target;
    if (auto failure = findTarget(draft, composition_, layer_, target))
        return std::move(*failure);
    auto* mask = findMask(*target.layer, mask_);
    if (mask == nullptr)
        return detail::invalidTarget();
    using Mode = document::LayerMaskMode;
    if (mode_ != Mode::None && mode_ != Mode::Add && mode_ != Mode::Subtract &&
        mode_ != Mode::Intersect && mode_ != Mode::Difference)
        return invalid("Mask mode is not a known value");
    if (mask->mode == mode_)
        return OperationResult::noChange();
    mask->mode = mode_;
    return spanResult(composition_, *target.layer, target.composition->duration());
}

std::string_view SetLayerMaskInverted::typeId() const noexcept {
    return "bloom.layer.set-mask-inverted";
}
OperationResult SetLayerMaskInverted::apply(document::Draft& draft) const {
    Target target;
    if (auto failure = findTarget(draft, composition_, layer_, target))
        return std::move(*failure);
    auto* mask = findMask(*target.layer, mask_);
    if (mask == nullptr)
        return detail::invalidTarget();
    if (mask->inverted == inverted_)
        return OperationResult::noChange();
    mask->inverted = inverted_;
    return spanResult(composition_, *target.layer, target.composition->duration());
}

std::string_view RenameLayerMask::typeId() const noexcept { return "bloom.layer.rename-mask"; }
OperationResult RenameLayerMask::apply(document::Draft& draft) const {
    Target target;
    if (auto failure = findTarget(draft, composition_, layer_, target))
        return std::move(*failure);
    auto* mask = findMask(*target.layer, mask_);
    if (mask == nullptr)
        return detail::invalidTarget();
    if (!document::isValidHumanFacingName(name_))
        return invalid("Mask name must be valid nonempty UTF-8");
    if (mask->name == name_)
        return OperationResult::noChange();
    mask->name = name_;
    // A name does not change pixels.
    auto result = OperationResult::applied();
    result.affectedTimes = normalizeAffectedTimeFootprint({composition_, {}});
    return result;
}

std::string_view SetLayerMaskPath::typeId() const noexcept { return "bloom.layer.set-mask-path"; }
OperationResult SetLayerMaskPath::apply(document::Draft& draft) const {
    Target target;
    if (auto failure = findTarget(draft, composition_, layer_, target))
        return std::move(*failure);
    auto& composition = *target.composition;
    const auto* mask = findMask(*target.layer, mask_);
    if (mask == nullptr)
        return detail::invalidTarget();
    if (!path_.isValid())
        return invalid("Mask path is invalid");
    const auto* record = composition.parameters().find(mask->path);
    if (record == nullptr)
        return detail::invalidTarget();
    if (std::holds_alternative<document::ConstantValueSource>(record->source)) {
        const auto* current = std::get_if<document::PathValue>(
            &std::get<document::ConstantValueSource>(record->source).value);
        if (current != nullptr && *current == path_)
            return OperationResult::noChange();
        if (!composition.parameters().setSource(mask->path, document::ConstantValueSource{path_}))
            return invalid("Mask path could not be set");
        return spanResult(composition_, *target.layer, composition.duration());
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&record->source);
    const auto* curve = source ? composition.animationCurves().findPath(source->curveId) : nullptr;
    if (curve == nullptr)
        return invalid("Mask path is not a constant or a Path curve");
    const auto curveId = source->curveId;
    const auto existing = std::ranges::find(curve->keyframes, time_, &document::PathKeyframe::time);
    if (existing != curve->keyframes.end()) {
        if (existing->value == path_)
            return OperationResult::noChange();
        auto key = *existing;
        key.value = path_;
        if (!composition.animationCurves().updateKeyframe(curveId, std::move(key)))
            return invalid("Mask path key could not be updated");
        return spanResult(composition_, *target.layer, composition.duration());
    }
    const auto keyId = draft.ids().allocateKeyframe();
    if (!keyId)
        return detail::exhaustedIds();
    document::PathKeyframe key;
    key.id = *keyId;
    key.time = time_;
    key.value = path_;
    key.outgoingInterpolation = document::KeyframeInterpolation::Linear;
    if (!composition.animationCurves().insertKeyframe(curveId, std::move(key)))
        return invalid("Mask path key could not be inserted");
    return spanResult(composition_, *target.layer, composition.duration(),
                      OperationResult::applied({{"keyframe", *keyId}}));
}

std::string_view AnimateLayerMaskPath::typeId() const noexcept {
    return "bloom.layer.animate-mask-path";
}
OperationResult AnimateLayerMaskPath::apply(document::Draft& draft) const {
    Target target;
    if (auto failure = findTarget(draft, composition_, layer_, target))
        return std::move(*failure);
    auto& composition = *target.composition;
    const auto* mask = findMask(*target.layer, mask_);
    if (mask == nullptr)
        return detail::invalidTarget();
    const auto* record = composition.parameters().find(mask->path);
    if (record == nullptr)
        return detail::invalidTarget();
    const auto* constant = std::get_if<document::ConstantValueSource>(&record->source);
    if (constant == nullptr)
        return OperationResult::noChange();
    const auto* value = std::get_if<document::PathValue>(&constant->value);
    if (value == nullptr)
        return invalid("Mask path parameter does not hold a path");
    const auto curveId = draft.ids().allocateAnimationCurve();
    const auto keyId = draft.ids().allocateKeyframe();
    if (!curveId || !keyId)
        return detail::exhaustedIds();
    document::PathKeyframe key;
    key.id = *keyId;
    key.time = time_;
    key.value = *value;
    key.outgoingInterpolation = document::KeyframeInterpolation::Linear;
    document::PathAnimationCurve curve;
    curve.id = *curveId;
    curve.keyframes.push_back(std::move(key));
    const auto defaultValue = constant->value;
    if (!composition.animationCurves().insert(std::move(curve)) ||
        !composition.parameters().setSource(mask->path,
                                            document::AnimationCurveSource{*curveId, defaultValue}))
        return invalid("Mask path could not be animated");
    return spanResult(composition_, *target.layer, composition.duration(),
                      OperationResult::applied({{"curve", *curveId}, {"keyframe", *keyId}}));
}

std::string_view StopAnimatingLayerMaskPath::typeId() const noexcept {
    return "bloom.layer.stop-animating-mask-path";
}
OperationResult StopAnimatingLayerMaskPath::apply(document::Draft& draft) const {
    Target target;
    if (auto failure = findTarget(draft, composition_, layer_, target))
        return std::move(*failure);
    auto& composition = *target.composition;
    const auto* mask = findMask(*target.layer, mask_);
    if (mask == nullptr)
        return detail::invalidTarget();
    if (!value_.isValid())
        return invalid("Mask path is invalid");
    const auto* record = composition.parameters().find(mask->path);
    if (record == nullptr)
        return detail::invalidTarget();
    const auto* source = std::get_if<document::AnimationCurveSource>(&record->source);
    if (source == nullptr)
        return OperationResult::noChange();
    const auto curveId = source->curveId;
    if (!composition.parameters().setSource(mask->path, document::ConstantValueSource{value_}) ||
        !composition.animationCurves().erase(curveId))
        return invalid("Mask path could not be made constant");
    return spanResult(composition_, *target.layer, composition.duration());
}

std::string_view SetTrackMatte::typeId() const noexcept { return "bloom.layer.set-track-matte"; }
OperationResult SetTrackMatte::apply(document::Draft& draft) const {
    Target target;
    if (auto failure = findTarget(draft, composition_, target_, target))
        return std::move(*failure);
    auto& composition = *target.composition;
    auto& graph = composition.graph();
    auto& layer = *target.layer;
    if (layer.trackMatte == matte_)
        return OperationResult::noChange();
    if (matte_) {
        const auto& matte = *matte_;
        if (matte.channel != document::TrackMatteChannel::Alpha &&
            matte.channel != document::TrackMatteChannel::Luma)
            return invalid("Track matte channel must be Alpha or Luma");
        if (matte.source == target_)
            return invalid("A layer cannot be its own track matte");
        if (graph.findLayer(matte.source) == nullptr)
            return OperationResult::rejected(
                OperationIssueCode::MissingReference,
                "Track matte source must be a layer in the composition");
        const auto mergeOf = [&](const document::LayerId id) -> const document::LayerStack* {
            for (const auto& stack : graph.merges())
                for (const auto& entry : stack.entries())
                    if (entry.layerId == id)
                        return &stack;
            return nullptr;
        };
        const auto* targetMerge = mergeOf(target_);
        if (targetMerge == nullptr || targetMerge != mergeOf(matte.source))
            return invalid("Track matte source must be a layer in the same Merge");
        std::unordered_set<document::LayerId> chain{target_};
        for (auto next = std::optional<document::LayerId>{matte.source}; next;) {
            if (!chain.insert(*next).second)
                return OperationResult::rejected(OperationIssueCode::GraphCycle,
                                                 "Track mattes must not form a cycle");
            const auto* found = graph.findLayer(*next);
            next = found && found->trackMatte ? std::optional{found->trackMatte->source}
                                              : std::nullopt;
        }
    }
    const auto duration = composition.duration();
    std::vector<AffectedTimeRange> ranges;
    addSpan(ranges, layer, duration);
    for (const auto& candidate : {layer.trackMatte, matte_})
        if (candidate)
            if (const auto* source = graph.findLayer(candidate->source))
                addSpan(ranges, *source, duration);
    layer.trackMatte = matte_;
    auto result = OperationResult::applied();
    result.affectedTimes = normalizeAffectedTimeFootprint({composition_, std::move(ranges)});
    return result;
}

} // namespace bloom::commands
