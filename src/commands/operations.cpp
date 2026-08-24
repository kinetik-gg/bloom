#include <bloom/commands/operations.hpp>

#include <bloom/document/layer_stack.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>

namespace bloom::commands {
namespace {

OperationResult invalidComposition(const document::CompositionId compositionId) {
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Composition " + std::to_string(compositionId.value()) +
                                         " does not exist");
}

OperationResult exhaustedIds() {
    return OperationResult::rejected(OperationIssueCode::Unsupported,
                                     "Document ID space is exhausted");
}

} // namespace

std::string_view AddTextLayer::typeId() const noexcept { return "bloom.layer.add-text"; }

OperationResult AddTextLayer::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (name_.empty()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Text layer name must not be empty");
    }
    if (!std::isfinite(position_.x) || !std::isfinite(position_.y)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Text layer position must be finite");
    }
    if (!std::isfinite(opacity_) || opacity_ < 0.0 || opacity_ > 1.0) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Text layer opacity must be between zero and one");
    }

    auto& ids = draft.ids();
    const auto textNodeId = ids.allocateNode();
    const auto layerOutputNodeId = ids.allocateNode();
    const auto textToLayerEdgeId = ids.allocateEdge();
    const auto layerToStackEdgeId = ids.allocateEdge();
    const auto layerId = ids.allocateLayer();
    const auto slotId = ids.allocateLayerSlot();
    const auto textParameterId = ids.allocateParameter();
    const auto positionParameterId = ids.allocateParameter();
    const auto opacityParameterId = ids.allocateParameter();
    if (!textNodeId.has_value() || !layerOutputNodeId.has_value() ||
        !textToLayerEdgeId.has_value() || !layerToStackEdgeId.has_value() || !layerId.has_value() ||
        !slotId.has_value() || !textParameterId.has_value() || !positionParameterId.has_value() ||
        !opacityParameterId.has_value()) {
        return exhaustedIds();
    }

    auto& parameters = composition->parameters();
    if (!parameters.insert({*textParameterId, std::string(document::kTextParameterSchemaKey),
                            document::ConstantValueSource{text_}}) ||
        !parameters.insert({*positionParameterId,
                            std::string(document::kPositionParameterSchemaKey),
                            document::ConstantValueSource{position_}}) ||
        !parameters.insert({*opacityParameterId, std::string(document::kOpacityParameterSchemaKey),
                            document::ConstantValueSource{opacity_}})) {
        return OperationResult::rejected(OperationIssueCode::DuplicateId,
                                         "Text layer parameters could not be inserted");
    }

    auto& graph = composition->graph();
    document::NodeRecord textNode{
        *textNodeId,
        std::string(document::kTextSourceNodeType),
        {{std::string(document::kTextParameterRole), *textParameterId}},
    };
    document::NodeRecord layerOutputNode{
        *layerOutputNodeId,
        std::string(document::kLayerOutputNodeType),
        {
            {std::string(document::kPositionParameterRole), *positionParameterId},
            {std::string(document::kOpacityParameterRole), *opacityParameterId},
        },
    };
    document::LayerOutputBoundary layerBoundary{
        *layerOutputNodeId,
        *layerId,
        name_,
        std::string(document::kLayerOutputOutputPort),
    };
    document::EdgeRecord textToLayerEdge{
        *textToLayerEdgeId,
        {*textNodeId, std::string(document::kTextSourceOutputPort)},
        document::NodeInputRef{*layerOutputNodeId,
                               std::string(document::kLayerOutputContentInputPort)},
    };
    document::EdgeRecord layerToStackEdge{
        *layerToStackEdgeId,
        {*layerOutputNodeId, std::string(document::kLayerOutputOutputPort)},
        document::LayerStackInputRef{graph.layerStack().nodeId(), *slotId,
                                     std::string(document::kLayerStackContentInputRole)},
    };
    if (!graph.addNode(std::move(textNode)) || !graph.addNode(std::move(layerOutputNode)) ||
        !graph.addLayerOutput(std::move(layerBoundary)) ||
        !graph.layerStack().append({*slotId, *layerId}) ||
        !graph.addEdge(std::move(textToLayerEdge)) || !graph.addEdge(std::move(layerToStackEdge))) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Text layer topology could not be inserted");
    }

    return OperationResult::applied({
        {std::string(kAddTextLayerLayerOutput), DurableObjectId{*layerId}},
        {std::string(kAddTextLayerSlotOutput), DurableObjectId{*slotId}},
        {std::string(kAddTextLayerTextNodeOutput), DurableObjectId{*textNodeId}},
        {std::string(kAddTextLayerLayerOutputNodeOutput), DurableObjectId{*layerOutputNodeId}},
        {std::string(kAddTextLayerTextParameterOutput), DurableObjectId{*textParameterId}},
        {std::string(kAddTextLayerPositionParameterOutput), DurableObjectId{*positionParameterId}},
        {std::string(kAddTextLayerOpacityParameterOutput), DurableObjectId{*opacityParameterId}},
        {std::string(kAddTextLayerTextToLayerEdgeOutput), DurableObjectId{*textToLayerEdgeId}},
        {std::string(kAddTextLayerLayerToStackEdgeOutput), DurableObjectId{*layerToStackEdgeId}},
    });
}

std::string_view SetProjectName::typeId() const noexcept { return "bloom.project.set-name"; }

OperationResult SetProjectName::apply(document::Draft& draft) const {
    if (name_.empty()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Project name must not be empty");
    }
    if (draft.project().name() == name_) {
        return OperationResult::noChange();
    }
    draft.project().setName(name_);
    return OperationResult::applied();
}

std::string_view SetCompositionName::typeId() const noexcept {
    return "bloom.composition.set-name";
}

OperationResult SetCompositionName::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (name_.empty()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition name must not be empty");
    }
    if (composition->name() == name_) {
        return OperationResult::noChange();
    }
    composition->setName(name_);
    return OperationResult::applied();
}

std::string_view SetCompositionDuration::typeId() const noexcept {
    return "bloom.composition.set-duration";
}

OperationResult SetCompositionDuration::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (composition->duration() == duration_) {
        return OperationResult::noChange();
    }
    if (!composition->setDuration(duration_)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition duration must be greater than zero");
    }
    return OperationResult::applied();
}

std::string_view SetParameterSource::typeId() const noexcept {
    return "bloom.parameter.set-source";
}

OperationResult SetParameterSource::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }

    auto* parameter = composition->parameters().find(parameterId_);
    if (parameter == nullptr) {
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Parameter " + std::to_string(parameterId_.value()) +
                                             " does not exist");
    }
    if (parameter->source == source_) {
        return OperationResult::noChange();
    }
    if (!composition->parameters().setSource(parameterId_, source_)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Parameter source is invalid");
    }
    return OperationResult::applied();
}

std::string_view MoveLayerBefore::typeId() const noexcept { return "bloom.layer.move-before"; }

OperationResult MoveLayerBefore::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }

    auto& stack = composition->graph().layerStack();
    const auto entries = stack.entries();
    const auto moving = std::find_if(entries.begin(), entries.end(),
                                     [this](const auto& entry) { return entry.slotId == slotId_; });
    if (moving == entries.end()) {
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Layer slot " + std::to_string(slotId_.value()) +
                                             " does not exist");
    }
    if (beforeSlotId_ == slotId_) {
        return OperationResult::noChange();
    }

    if (beforeSlotId_.has_value()) {
        const auto before = std::find_if(entries.begin(), entries.end(), [this](const auto& entry) {
            return entry.slotId == *beforeSlotId_;
        });
        if (before == entries.end()) {
            return OperationResult::rejected(OperationIssueCode::MissingReference,
                                             "Destination layer slot " +
                                                 std::to_string(beforeSlotId_->value()) +
                                                 " does not exist");
        }
        if (std::next(moving) == before) {
            return OperationResult::noChange();
        }
    } else if (std::next(moving) == entries.end()) {
        return OperationResult::noChange();
    }

    if (!stack.moveBefore(slotId_, beforeSlotId_)) {
        return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                         "Layer move could not be applied");
    }
    return OperationResult::applied();
}

} // namespace bloom::commands
