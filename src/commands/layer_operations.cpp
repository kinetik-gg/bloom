#include "node_operation_support.hpp"
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/core/frame_time_mapping.hpp>
#include <limits>

namespace bloom::commands {
namespace {
std::optional<core::RationalTime> snap(const core::RationalTime time,
                                       const document::Composition& composition) {
    const auto rate = composition.format().frameRate();
    const auto mapping = core::FrameTimeMapping::create(composition.duration(), rate.numerator(),
                                                        rate.denominator());
    if (!mapping)
        return std::nullopt;
    const auto index = mapping.value()->nearestFrameIndex(time);
    auto result = mapping.value()->timeForFrame(index);
    // The frame mapping clamps scrubs to the last frame. Range endpoints may also be duration.
    if (time == composition.duration())
        return time;
    if (!result)
        return std::nullopt;
    return *result.value();
}
OperationResult invalidRange() {
    return OperationResult::rejected(
        OperationIssueCode::InvalidValue,
        "Layer range must satisfy 0 <= in < out <= duration on composition frames");
}
} // namespace
std::string_view SetLayerParent::typeId() const noexcept { return "bloom.layer.set-parent"; }
OperationResult SetLayerParent::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Layer must exist in the target composition");
    if (layer->locked)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer is locked");
    auto ancestor = parent_;
    std::size_t remaining = composition->graph().layerOutputs().size();
    while (ancestor) {
        if (*ancestor == layer_ || remaining-- == 0)
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Layer parenting would create a cycle");
        const auto* parent = composition->graph().findLayer(*ancestor);
        if (!parent)
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Parent must be a layer in the same composition");
        ancestor = parent->parent;
    }
    if (layer->parent == parent_)
        return OperationResult::noChange();
    layer->parent = parent_;
    return OperationResult::applied();
}
std::string_view SetLayerRange::typeId() const noexcept { return "bloom.layer.set-range"; }
OperationResult SetLayerRange::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return detail::invalidTarget();
    if (layer->locked)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer is locked");
    if (in_ < core::RationalTime{} || in_ >= out_ || out_ > composition->duration())
        return invalidRange();
    const auto in = snap(in_, *composition), out = snap(out_, *composition);
    if (!in || !out || *in >= *out || *out > composition->duration())
        return invalidRange();
    if (layer->inPoint == *in && layer->endPoint(composition->duration()) == *out)
        return OperationResult::noChange();
    layer->inPoint = *in;
    layer->outPoint = *out;
    return OperationResult::applied();
}
std::string_view SplitLayerAtTime::typeId() const noexcept { return "bloom.layer.split-at-time"; }
OperationResult SplitLayerAtTime::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    const auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return detail::invalidTarget();
    if (layer->locked)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer is locked");
    const auto original = *layer;
    if (time_ <= original.inPoint || time_ >= original.endPoint(composition->duration()))
        return invalidRange();
    const auto time = snap(time_, *composition);
    if (!time || *time <= original.inPoint || *time >= original.endPoint(composition->duration()))
        return invalidRange();
    auto duplicated = DuplicateNodes(composition_, {original.nodeId}, {0, 0}).apply(draft);
    if (duplicated.status != OperationStatus::Applied)
        return duplicated;
    document::LayerId copyId;
    for (const auto& output : duplicated.outputs)
        if (output.name == "layer." + std::to_string(layer_.value()))
            copyId = std::get<document::LayerId>(output.id);
    auto* copy = composition->graph().findLayer(copyId);
    if (!copy)
        return detail::invalidTarget();
    composition->graph().findLayer(layer_)->outPoint = *time;
    copy->parent = original.parent;
    copy->inPoint = *time;
    copy->outPoint = original.outPoint;
    duplicated.outputs.push_back({"layer", copyId});
    return duplicated;
}
std::string_view SetLayerEnabled::typeId() const noexcept { return "bloom.layer.set-enabled"; }
OperationResult SetLayerEnabled::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return detail::invalidTarget();
    auto layout = detail::layoutFor(*composition, layer->nodeId);
    if (layer->enabled == value_ && layout.muted == !value_)
        return OperationResult::noChange();
    layer->enabled = value_;
    layout.muted = !value_;
    composition->nodeLayout()[layer->nodeId] = layout;
    return OperationResult::applied();
}
std::string_view SetLayerSolo::typeId() const noexcept { return "bloom.layer.set-solo"; }
OperationResult SetLayerSolo::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return detail::invalidTarget();
    if (layer->solo == value_)
        return OperationResult::noChange();
    layer->solo = value_;
    return OperationResult::applied();
}
std::string_view SetLayerLocked::typeId() const noexcept { return "bloom.layer.set-locked"; }
OperationResult SetLayerLocked::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return detail::invalidTarget();
    if (layer->locked == value_)
        return OperationResult::noChange();
    layer->locked = value_;
    return OperationResult::applied();
}
std::string_view SetLayerLabelColor::typeId() const noexcept {
    return "bloom.layer.set-label-color";
}
OperationResult SetLayerLabelColor::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return detail::invalidTarget();
    if (layer->labelColor == color_)
        return OperationResult::noChange();
    layer->labelColor = color_;
    return OperationResult::applied();
}
std::string_view SetWorkArea::typeId() const noexcept { return "bloom.composition.set-work-area"; }
OperationResult SetWorkArea::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return detail::invalidTarget();
    if (start_ < core::RationalTime{} || start_ >= end_ || end_ > composition->duration())
        return invalidRange();
    const auto start = snap(start_, *composition), end = snap(end_, *composition);
    if (!start || !end || *start >= *end)
        return invalidRange();
    const document::WorkArea area{*start, *end};
    if (composition->workArea() == area)
        return OperationResult::noChange();
    composition->setWorkArea(area);
    return OperationResult::applied();
}
std::string_view ClearWorkArea::typeId() const noexcept {
    return "bloom.composition.clear-work-area";
}
OperationResult ClearWorkArea::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return detail::invalidTarget();
    if (!composition->workArea())
        return OperationResult::noChange();
    composition->setWorkArea(std::nullopt);
    return OperationResult::applied();
}
} // namespace bloom::commands
