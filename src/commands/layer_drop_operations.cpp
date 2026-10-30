#include <bloom/commands/layer_drop_operations.hpp>

#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/layer_timing_gesture.hpp>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>

namespace bloom::commands {
namespace {
std::optional<document::LayerId> outputLayer(const OperationResult& result) {
    for (const auto& output : result.outputs)
        if (output.name == "layer")
            if (const auto* id = std::get_if<document::LayerId>(&output.id))
                return *id;
    return std::nullopt;
}

// The composition frame nearest `time`, clamped into the composition: the same snap the Timeline's
// frame math (docs/architecture/animation-and-time.md, "Session Time And Scrubbing") applies.
core::RationalTime frameStart(const document::Composition& composition,
                              const core::RationalTime time) {
    const auto rate = composition.format().frameRate();
    const auto created = core::FrameTimeMapping::create(composition.duration(), rate.numerator(),
                                                        rate.denominator());
    if (!created.hasValue())
        return core::RationalTime{};
    const auto mapped = created.value()->timeForFrame(created.value()->nearestFrameIndex(time));
    return mapped.hasValue() ? *mapped.value() : core::RationalTime{};
}
} // namespace

bool droppableAsLayer(const document::Project& project, const document::CompositionId target,
                      const LayerDropItem& item) {
    if (const auto* composition = std::get_if<document::CompositionId>(&item))
        return *composition != target && project.findComposition(*composition) != nullptr;
    const auto* asset = project.findAsset(std::get<document::AssetId>(item));
    return asset != nullptr &&
           (asset->kind == document::AssetKind::Image ||
            asset->kind == document::AssetKind::Sequence ||
            asset->kind == document::AssetKind::Video || asset->kind == document::AssetKind::Audio);
}

OperationResult AddDroppedLayers::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "The composition does not exist");
    if (items_.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Nothing to add");
    for (const auto& item : items_)
        if (!droppableAsLayer(draft.project(), composition_, item))
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "This item cannot be a layer here");
    // The new layers go directly above `anchor` (nullopt: below every layer), in item order.
    const auto entries = composition->graph().layerStack().entries();
    std::optional<document::LayerSlotId> anchor;
    switch (placement_.stack) {
    case LayerDropPlacement::Stack::Top:
        if (!entries.empty())
            anchor = entries.front().slotId;
        break;
    case LayerDropPlacement::Stack::Before: {
        auto before = placement_.before;
        if (!before.isValid() && placement_.beforeLayer.isValid()) {
            const auto slot = std::ranges::find(entries, placement_.beforeLayer,
                                                &document::LayerStackEntry::layerId);
            if (slot != entries.end())
                before = slot->slotId;
        }
        if (composition->graph().layerStack().find(before) == nullptr)
            return OperationResult::rejected(OperationIssueCode::MissingReference,
                                             "The layer to insert above no longer exists");
        anchor = before;
        break;
    }
    case LayerDropPlacement::Stack::Bottom:
        break;
    }
    auto start = frameStart(*composition, placement_.start);
    std::vector<OperationOutput> outputs;
    for (const auto& item : items_) {
        OperationResult added;
        if (const auto* nested = std::get_if<document::CompositionId>(&item))
            added = AddCompositionSource(composition_, *nested, {}, true).apply(draft);
        else if (const auto id = std::get<document::AssetId>(item);
                 draft.project().findAsset(id)->kind == document::AssetKind::Audio)
            added = AddAudioLayer(composition_, id).apply(draft);
        else
            added = AddImageLayer(composition_, id).apply(draft);
        if (added.status == OperationStatus::Rejected)
            return added;
        const auto layerId = outputLayer(added);
        composition = draft.project().findComposition(composition_);
        const auto* layer = layerId ? composition->graph().findLayer(*layerId) : nullptr;
        const auto stack = composition->graph().layerStack().entries();
        const auto slot = std::ranges::find(stack, *layerId, &document::LayerStackEntry::layerId);
        if (layer == nullptr || slot == stack.end())
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "The new layer is not in the layer stack");
        auto moved = MoveLayerBefore(composition_, slot->slotId, anchor).apply(draft);
        if (moved.status == OperationStatus::Rejected)
            return moved;
        // Move the layer to its start the way a Timeline body drag does: start, in and out
        // together, on frames, keeping at least one frame inside the composition.
        composition = draft.project().findComposition(composition_);
        layer = composition->graph().findLayer(*layerId);
        const auto duration = composition->duration();
        const auto proposal =
            proposeLayerTiming({.start = layer->timing.startTime,
                                .in = layer->inPoint,
                                .out = layer->endPoint(duration),
                                .speed = layer->timing.speed,
                                .rate = composition->format().frameRate(),
                                .duration = duration,
                                .extentStart = std::nullopt,
                                .extentEnd = std::nullopt,
                                .handle = 0,
                                .alt = false,
                                .deltaSeconds = start.toSeconds() - layer->inPoint.toSeconds()});
        if (!proposal)
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "The layer cannot start there");
        auto timed = SetLayerTiming(composition_, *layerId, proposal->start, proposal->in,
                                    proposal->out, proposal->speed)
                         .apply(draft);
        if (timed.status == OperationStatus::Rejected)
            return timed;
        if (placement_.sequence)
            start = proposal->out;
        outputs.push_back({"layer", *layerId});
    }
    return OperationResult::applied(std::move(outputs));
}

} // namespace bloom::commands
