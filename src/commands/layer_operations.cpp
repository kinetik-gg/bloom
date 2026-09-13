#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include "node_operation_support.hpp"
#include <cmath>
#include <limits>

namespace bloom::commands {
namespace {
std::optional<core::RationalTime> snap(const core::RationalTime time, const document::Composition& composition) {
    const auto rate = composition.format().frameRate();
    const long double frame = std::round(static_cast<long double>(time.numerator()) / time.denominator() * rate.numerator() / rate.denominator());
    if (frame < 0 || frame > static_cast<long double>(std::numeric_limits<std::int64_t>::max()) / static_cast<long double>(rate.denominator()))
        return std::nullopt;
    return core::RationalTime::create(static_cast<std::int64_t>(frame) * rate.denominator(), rate.numerator());
}
OperationResult invalidRange() {
    return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer range must satisfy 0 <= in < out <= duration on composition frames");
}
}
std::string_view SetLayerRange::typeId() const noexcept { return "bloom.layer.set-range"; }
OperationResult SetLayerRange::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer) return detail::invalidTarget();
    if (layer->locked) return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer is locked");
    if (in_ < core::RationalTime{} || in_ >= out_ || out_ > composition->duration()) return invalidRange();
    const auto in = snap(in_, *composition), out = snap(out_, *composition);
    if (!in || !out || *in >= *out || *out > composition->duration()) return invalidRange();
    if (layer->inPoint == *in && layer->endPoint(composition->duration()) == *out) return OperationResult::noChange();
    layer->inPoint = *in;
    layer->outPoint = *out;
    return OperationResult::applied();
}
std::string_view SplitLayerAtTime::typeId() const noexcept { return "bloom.layer.split-at-time"; }
OperationResult SplitLayerAtTime::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    const auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer) return detail::invalidTarget();
    if (layer->locked) return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer is locked");
    const auto original = *layer;
    const auto time = snap(time_, *composition);
    if (!time || *time <= original.inPoint || *time >= original.endPoint(composition->duration())) return invalidRange();
    auto duplicated = DuplicateNodes(composition_, {original.nodeId}, {0, 0}).apply(draft);
    if (duplicated.status != OperationStatus::Applied) return duplicated;
    document::LayerId copyId;
    for (const auto& output : duplicated.outputs)
        if (output.name == "layer." + std::to_string(layer_.value()))
            copyId = std::get<document::LayerId>(output.id);
    auto* copy = composition->graph().findLayer(copyId);
    if (!copy) return detail::invalidTarget();
    // Duplicate the boundary and its own parameters, retaining shared upstream graph inputs.
    const auto edges = std::vector(composition->graph().edges().begin(), composition->graph().edges().end());
    for (auto edge : edges) {
        auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
        if (!input || input->nodeId != original.nodeId) continue;
        const auto id = draft.ids().allocateEdge();
        if (!id) return detail::exhaustedIds();
        edge.id = *id;
        input->nodeId = copy->nodeId;
        if (!composition->graph().addEdge(std::move(edge))) return detail::invalidTarget();
    }
    composition->graph().findLayer(layer_)->outPoint = *time;
    copy->inPoint = *time;
    copy->outPoint = original.outPoint;
    duplicated.outputs.push_back({"layer", copyId});
    return duplicated;
}
std::string_view SetLayerEnabled::typeId() const noexcept { return "bloom.layer.set-enabled"; }
OperationResult SetLayerEnabled::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer) return detail::invalidTarget();
    if (layer->enabled == value_) return OperationResult::noChange();
    layer->enabled = value_;
    auto& layout = composition->nodeLayout()[layer->nodeId];
    layout.muted = !value_;
    return OperationResult::applied();
}
std::string_view SetLayerSolo::typeId() const noexcept { return "bloom.layer.set-solo"; }
OperationResult SetLayerSolo::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer) return detail::invalidTarget();
    if (layer->solo == value_) return OperationResult::noChange();
    layer->solo = value_;
    return OperationResult::applied();
}
std::string_view SetLayerLocked::typeId() const noexcept { return "bloom.layer.set-locked"; }
OperationResult SetLayerLocked::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer) return detail::invalidTarget();
    if (layer->locked == value_) return OperationResult::noChange();
    layer->locked = value_;
    return OperationResult::applied();
}
} // namespace bloom::commands
