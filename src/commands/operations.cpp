#include <bloom/commands/operations.hpp>

#include <bloom/document/layer_stack.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
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

struct StructuredLayerIds {
    document::NodeId sourceNodeId;
    document::NodeId layerOutputNodeId;
    document::EdgeId sourceToLayerEdgeId;
    document::EdgeId layerToStackEdgeId;
    document::LayerId layerId;
    document::LayerSlotId slotId;
    document::ParameterId sourceParameterId;
    document::ParameterId positionParameterId;
    document::ParameterId opacityParameterId;
};

struct StructuredLayerDescriptor {
    std::string_view sourceNodeType;
    std::uint32_t sourceNodeSchemaVersion;
    std::string_view sourceOutputPort;
    std::string_view sourceParameterSchema;
    std::string_view sourceParameterRole;
    document::ParameterValue sourceValue;
};

struct StructuredLayerOutputNames {
    std::string_view layer;
    std::string_view slot;
    std::string_view sourceNode;
    std::string_view layerOutputNode;
    std::string_view sourceParameter;
    std::string_view positionParameter;
    std::string_view opacityParameter;
    std::string_view sourceToLayerEdge;
    std::string_view layerToStackEdge;
};

[[nodiscard]] std::optional<StructuredLayerIds>
allocateStructuredLayerIds(document::IdAllocator& allocator) {
    const auto sourceNodeId = allocator.allocateNode();
    const auto layerOutputNodeId = allocator.allocateNode();
    const auto sourceToLayerEdgeId = allocator.allocateEdge();
    const auto layerToStackEdgeId = allocator.allocateEdge();
    const auto layerId = allocator.allocateLayer();
    const auto slotId = allocator.allocateLayerSlot();
    const auto sourceParameterId = allocator.allocateParameter();
    const auto positionParameterId = allocator.allocateParameter();
    const auto opacityParameterId = allocator.allocateParameter();
    if (!sourceNodeId.has_value() || !layerOutputNodeId.has_value() ||
        !sourceToLayerEdgeId.has_value() || !layerToStackEdgeId.has_value() ||
        !layerId.has_value() || !slotId.has_value() || !sourceParameterId.has_value() ||
        !positionParameterId.has_value() || !opacityParameterId.has_value()) {
        return std::nullopt;
    }
    return StructuredLayerIds{
        *sourceNodeId, *layerOutputNodeId, *sourceToLayerEdgeId, *layerToStackEdgeId, *layerId,
        *slotId,       *sourceParameterId, *positionParameterId, *opacityParameterId};
}

[[nodiscard]] OperationResult
addStructuredLayer(document::Draft& draft, document::Composition& composition,
                   const std::string& name, StructuredLayerDescriptor descriptor,
                   const document::Vec2d position, const double opacity,
                   const StructuredLayerOutputNames& outputNames) {
    const auto ids = allocateStructuredLayerIds(draft.ids());
    if (!ids.has_value()) {
        return exhaustedIds();
    }

    auto& parameters = composition.parameters();
    if (!parameters.insert({ids->sourceParameterId, std::string(descriptor.sourceParameterSchema),
                            document::ConstantValueSource{std::move(descriptor.sourceValue)}}) ||
        !parameters.insert({ids->positionParameterId,
                            std::string(document::kPositionParameterSchemaKey),
                            document::ConstantValueSource{position}}) ||
        !parameters.insert({ids->opacityParameterId,
                            std::string(document::kOpacityParameterSchemaKey),
                            document::ConstantValueSource{opacity}})) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Layer parameters could not be inserted");
    }

    auto& graph = composition.graph();
    document::NodeRecord sourceNode{
        ids->sourceNodeId,
        std::string(descriptor.sourceNodeType),
        {{std::string(descriptor.sourceParameterRole), ids->sourceParameterId}},
        descriptor.sourceNodeSchemaVersion,
    };
    document::NodeRecord layerOutputNode{
        ids->layerOutputNodeId,
        std::string(document::kLayerOutputNodeType),
        {
            {std::string(document::kPositionParameterRole), ids->positionParameterId},
            {std::string(document::kOpacityParameterRole), ids->opacityParameterId},
        },
        document::kLayerOutputNodeSchemaVersion,
    };
    document::LayerOutputBoundary layerBoundary{
        ids->layerOutputNodeId,
        ids->layerId,
        name,
        std::string(document::kLayerOutputOutputPort),
    };
    document::EdgeRecord sourceToLayerEdge{
        ids->sourceToLayerEdgeId,
        {ids->sourceNodeId, std::string(descriptor.sourceOutputPort)},
        document::NodeInputRef{ids->layerOutputNodeId,
                               std::string(document::kLayerOutputContentInputPort)},
    };
    document::EdgeRecord layerToStackEdge{
        ids->layerToStackEdgeId,
        {ids->layerOutputNodeId, std::string(document::kLayerOutputOutputPort)},
        document::LayerStackInputRef{graph.layerStack().nodeId(), ids->slotId,
                                     std::string(document::kLayerStackContentInputRole)},
    };
    if (!graph.addNode(std::move(sourceNode)) || !graph.addNode(std::move(layerOutputNode)) ||
        !graph.addLayerOutput(std::move(layerBoundary)) ||
        !graph.layerStack().append({ids->slotId, ids->layerId}) ||
        !graph.addEdge(std::move(sourceToLayerEdge)) ||
        !graph.addEdge(std::move(layerToStackEdge))) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Layer topology could not be inserted");
    }

    return OperationResult::applied({
        {std::string(outputNames.layer), DurableObjectId{ids->layerId}},
        {std::string(outputNames.slot), DurableObjectId{ids->slotId}},
        {std::string(outputNames.sourceNode), DurableObjectId{ids->sourceNodeId}},
        {std::string(outputNames.layerOutputNode), DurableObjectId{ids->layerOutputNodeId}},
        {std::string(outputNames.sourceParameter), DurableObjectId{ids->sourceParameterId}},
        {std::string(outputNames.positionParameter), DurableObjectId{ids->positionParameterId}},
        {std::string(outputNames.opacityParameter), DurableObjectId{ids->opacityParameterId}},
        {std::string(outputNames.sourceToLayerEdge), DurableObjectId{ids->sourceToLayerEdgeId}},
        {std::string(outputNames.layerToStackEdge), DurableObjectId{ids->layerToStackEdgeId}},
    });
}

} // namespace

std::string_view AddSolidLayer::typeId() const noexcept { return "bloom.layer.add-solid"; }

OperationResult AddSolidLayer::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (name_.empty()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Solid layer name must not be empty");
    }
    if (!color_.isValid()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Solid layer color must be finite with alpha between zero "
                                         "and one");
    }
    if (!std::isfinite(position_.x) || !std::isfinite(position_.y)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Solid layer position must be finite");
    }
    if (!std::isfinite(opacity_) || opacity_ < 0.0 || opacity_ > 1.0) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Solid layer opacity must be between zero and one");
    }

    return addStructuredLayer(
        draft, *composition, name_,
        {document::kSolidSourceNodeType, document::kSolidSourceNodeSchemaVersion,
         document::kSolidSourceOutputPort, document::kSolidColorParameterSchemaKey,
         document::kSolidColorParameterRole, color_},
        position_, opacity_,
        {kAddSolidLayerLayerOutput, kAddSolidLayerSlotOutput, kAddSolidLayerSolidNodeOutput,
         kAddSolidLayerLayerOutputNodeOutput, kAddSolidLayerColorParameterOutput,
         kAddSolidLayerPositionParameterOutput, kAddSolidLayerOpacityParameterOutput,
         kAddSolidLayerSolidToLayerEdgeOutput, kAddSolidLayerLayerToStackEdgeOutput});
}

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

    return addStructuredLayer(
        draft, *composition, name_,
        {document::kTextSourceNodeType, document::kTextSourceNodeSchemaVersion,
         document::kTextSourceOutputPort, document::kTextParameterSchemaKey,
         document::kTextParameterRole, text_},
        position_, opacity_,
        {kAddTextLayerLayerOutput, kAddTextLayerSlotOutput, kAddTextLayerTextNodeOutput,
         kAddTextLayerLayerOutputNodeOutput, kAddTextLayerTextParameterOutput,
         kAddTextLayerPositionParameterOutput, kAddTextLayerOpacityParameterOutput,
         kAddTextLayerTextToLayerEdgeOutput, kAddTextLayerLayerToStackEdgeOutput});
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

std::string_view SetCompositionFormat::typeId() const noexcept {
    return "bloom.composition.set-format";
}

OperationResult SetCompositionFormat::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (composition->format() == format_) {
        return OperationResult::noChange();
    }
    composition->setFormat(format_);
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
