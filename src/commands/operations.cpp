#include <bloom/commands/operations.hpp>

#include <bloom/core/utf8.hpp>
#include <bloom/document/layer_stack.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

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

struct CompositionCloneIds final {
    document::CompositionId composition;
    std::unordered_map<document::NodeId, document::NodeId> nodes;
    std::unordered_map<document::EdgeId, document::EdgeId> edges;
    std::unordered_map<document::LayerId, document::LayerId> layers;
    std::unordered_map<document::LayerSlotId, document::LayerSlotId> slots;
    std::unordered_map<document::ParameterId, document::ParameterId> parameters;
    std::unordered_map<document::AnimationCurveId, document::AnimationCurveId> curves;
    std::unordered_map<document::KeyframeId, document::KeyframeId> keyframes;
    std::unordered_map<document::NodeGroupId, document::NodeGroupId> groups;
};

template <typename Curve, typename Callback>
void forEachStoredKeyframe(Curve& curve, Callback&& callback) {
    using CurveType = std::remove_cvref_t<Curve>;
    if constexpr (std::is_same_v<CurveType, document::ScalarAnimationCurve>) {
        for (auto& keyframe : curve.keyframes)
            callback(keyframe);
    } else {
        if (std::ranges::all_of(curve.components, [](const auto& component) {
                return component.keyframes.empty();
            })) {
            for (auto& keyframe : curve.keyframes)
                callback(keyframe);
        } else {
            for (auto& component : curve.components)
                for (auto& keyframe : component.keyframes)
                    callback(keyframe);
        }
    }
}

template <typename Curve> std::size_t storedKeyframeCount(const Curve& curve) {
    using CurveType = std::remove_cvref_t<Curve>;
    if constexpr (std::is_same_v<CurveType, document::ScalarAnimationCurve>) {
        return curve.keyframes.size();
    } else {
        if (std::ranges::all_of(curve.components, [](const auto& component) {
                return component.keyframes.empty();
            })) {
            return curve.keyframes.size();
        }
        std::size_t count = 0;
        for (const auto& component : curve.components)
            count += component.keyframes.size();
        return count;
    }
}

[[nodiscard]] std::optional<CompositionCloneIds>
allocateCompositionCloneIds(document::Draft& draft, const document::Composition& source) {
    CompositionCloneIds ids;
    const auto compositionId = draft.ids().allocateComposition();
    if (!compositionId) {
        return std::nullopt;
    }
    ids.composition = *compositionId;
    for (const auto& node : source.graph().nodes()) {
        const auto id = draft.ids().allocateNode();
        if (!id) {
            return std::nullopt;
        }
        ids.nodes.emplace(node.id, *id);
    }
    for (const auto& edge : source.graph().edges()) {
        const auto id = draft.ids().allocateEdge();
        if (!id) {
            return std::nullopt;
        }
        ids.edges.emplace(edge.id, *id);
    }
    for (const auto& boundary : source.graph().layerOutputs()) {
        const auto id = draft.ids().allocateLayer();
        if (!id) {
            return std::nullopt;
        }
        ids.layers.emplace(boundary.layerId, *id);
    }
    for (const auto& stack : source.graph().merges())
        for (const auto& entry : stack.entries()) {
            const auto id = draft.ids().allocateLayerSlot();
            if (!id) {
                return std::nullopt;
            }
            ids.slots.emplace(entry.slotId, *id);
        }
    for (const auto& parameter : source.parameters().records()) {
        const auto id = draft.ids().allocateParameter();
        if (!id) {
            return std::nullopt;
        }
        ids.parameters.emplace(parameter.id, *id);
    }
    std::size_t keyframeCount = 0;
    for (const auto& record : source.animationCurves().records())
        keyframeCount +=
            std::visit([](const auto& curve) { return storedKeyframeCount(curve); }, record);
    for (const auto& record : source.animationCurves().records()) {
        const auto curveId = draft.ids().allocateAnimationCurve();
        if (!curveId) {
            return std::nullopt;
        }
        ids.curves.emplace(document::animationCurveId(record), *curveId);
        std::visit(
            [&](auto& curve) {
                forEachStoredKeyframe(curve, [&](const auto& keyframe) {
                    const auto keyframeId = draft.ids().allocateKeyframe();
                    if (!keyframeId) {
                        return;
                    }
                    ids.keyframes.emplace(keyframe.id, *keyframeId);
                });
            },
            record);
        if (ids.keyframes.size() != keyframeCount) {
            return std::nullopt;
        }
    }
    for (const auto& [groupId, unused] : source.nodeGroups()) {
        static_cast<void>(unused);
        const auto id = draft.ids().allocateNodeGroup();
        if (!id) {
            return std::nullopt;
        }
        ids.groups.emplace(groupId, *id);
    }
    return ids;
}

template <typename Id>
[[nodiscard]] Id remap(const std::unordered_map<Id, Id>& mapping, const Id id) {
    return mapping.at(id);
}

[[nodiscard]] std::optional<document::Composition>
cloneComposition(document::Draft& draft, const document::Composition& source,
                 const std::string& name) {
    const auto ids = allocateCompositionCloneIds(draft, source);
    if (!ids) {
        return std::nullopt;
    }

    const auto newLayerStackId = source.graph().merges().empty()
                                     ? document::NodeId{}
                                     : remap(ids->nodes, source.graph().merges().front().nodeId());
    document::CanonicalGraph graph(newLayerStackId);
    for (const auto& sourceNode : source.graph().nodes()) {
        auto node = sourceNode;
        node.id = remap(ids->nodes, sourceNode.id);
        for (auto& binding : node.parameters) {
            binding.parameterId = remap(ids->parameters, binding.parameterId);
        }
        if (!graph.addNode(std::move(node))) {
            return std::nullopt;
        }
    }
    for (const auto& sourceBoundary : source.graph().layerOutputs()) {
        auto boundary = sourceBoundary;
        boundary.nodeId = remap(ids->nodes, sourceBoundary.nodeId);
        boundary.layerId = remap(ids->layers, sourceBoundary.layerId);
        if (sourceBoundary.parent)
            boundary.parent = remap(ids->layers, *sourceBoundary.parent);
        if (!graph.addLayerOutput(std::move(boundary))) {
            return std::nullopt;
        }
    }
    for (const auto& stack : source.graph().merges())
        graph.merge(remap(ids->nodes, stack.nodeId()))->setEnabled(stack.enabled());
    for (const auto& stack : source.graph().merges())
        for (const auto& sourceEntry : stack.entries()) {
            if (!graph.merge(remap(ids->nodes, stack.nodeId()))
                     ->append({remap(ids->slots, sourceEntry.slotId),
                               sourceEntry.layerId.isValid()
                                   ? remap(ids->layers, sourceEntry.layerId)
                                   : document::LayerId{}})) {
                return std::nullopt;
            }
        }
    for (const auto& sourceEdge : source.graph().edges()) {
        auto edge = sourceEdge;
        edge.id = remap(ids->edges, sourceEdge.id);
        edge.source.nodeId = remap(ids->nodes, sourceEdge.source.nodeId);
        std::visit(
            [&](auto& destination) {
                using Destination = std::decay_t<decltype(destination)>;
                if constexpr (std::is_same_v<Destination, document::NodeInputRef>) {
                    destination.nodeId = remap(ids->nodes, destination.nodeId);
                } else {
                    destination.stackNodeId = remap(ids->nodes, destination.stackNodeId);
                    destination.slotId = remap(ids->slots, destination.slotId);
                }
            },
            edge.destination);
        if (!graph.addEdge(std::move(edge))) {
            return std::nullopt;
        }
    }
    if (source.graph().compositionOutput().has_value()) {
        auto output = *source.graph().compositionOutput();
        output.nodeId = remap(ids->nodes, output.nodeId);
        graph.setCompositionOutput(std::move(output));
    }

    document::Composition composition(ids->composition, name, source.duration(), std::move(graph),
                                      source.format());
    composition.setWorkArea(source.workArea());
    composition.setSafeAreas(source.safeAreas());
    composition.setBackgroundColor(source.backgroundColor());
    for (const auto& sourceParameter : source.parameters().records()) {
        auto parameter = sourceParameter;
        parameter.id = remap(ids->parameters, sourceParameter.id);
        std::visit(
            [&](auto& valueSource) {
                using Source = std::decay_t<decltype(valueSource)>;
                if constexpr (std::is_same_v<Source, document::AnimationCurveSource>) {
                    valueSource.curveId = remap(ids->curves, valueSource.curveId);
                } else if constexpr (std::is_same_v<Source, document::DriverBindingSource>) {
                    valueSource.sourceNodeId = remap(ids->nodes, valueSource.sourceNodeId);
                }
            },
            parameter.source);
        if (!composition.parameters().insert(std::move(parameter))) {
            return std::nullopt;
        }
    }
    for (const auto& sourceRecord : source.animationCurves().records()) {
        auto record = sourceRecord;
        std::visit(
            [&](auto& curve) {
                curve.id = remap(ids->curves, curve.id);
                forEachStoredKeyframe(curve, [&](auto& keyframe) {
                    keyframe.id = remap(ids->keyframes, keyframe.id);
                });
            },
            record);
        const auto copiedCurveId = document::animationCurveId(record);
        if (!composition.animationCurves().insert(std::move(record))) {
            return std::nullopt;
        }
        static_cast<void>(
            composition.animationCurves().synchronizeCompatibilityProjection(copiedCurveId));
    }
    for (const auto& [nodeId, layout] : source.nodeLayout()) {
        composition.nodeLayout().emplace(remap(ids->nodes, nodeId), layout);
    }
    for (const auto& [groupId, sourceGroup] : source.nodeGroups()) {
        auto group = sourceGroup;
        group.id = remap(ids->groups, groupId);
        group.members.clear();
        for (const auto member : sourceGroup.members) {
            group.members.insert(remap(ids->nodes, member));
        }
        composition.nodeGroups().emplace(group.id, std::move(group));
    }
    return composition;
}

// One parameter a source node owns: its node-local role, its global schema key, its initial value,
// and the command-result output name the caller reads its freshly allocated ID back from. A solid
// source has exactly one (color); a text source has seven (content, size, color, alignment, line
// height, letter spacing, font), in the order its registered definition declares them.
struct StructuredSourceParameter {
    std::string_view role;
    std::string_view schemaKey;
    document::ParameterValue value;
    std::string_view outputName;
};

struct StructuredLayerIds {
    document::NodeId sourceNodeId;
    document::NodeId layerOutputNodeId;
    document::EdgeId sourceToLayerEdgeId;
    document::EdgeId layerToStackEdgeId;
    document::LayerId layerId;
    document::LayerSlotId slotId;
    std::vector<document::ParameterId> sourceParameterIds;
    document::ParameterId positionParameterId;
    document::ParameterId anchorParameterId;
    document::ParameterId scaleParameterId;
    document::ParameterId rotationParameterId;
    document::ParameterId opacityParameterId;
    document::ParameterId blendModeParameterId;
};

struct StructuredLayerDescriptor {
    std::string_view sourceNodeType;
    std::uint32_t sourceNodeSchemaVersion;
    std::string_view sourceOutputPort;
    std::vector<StructuredSourceParameter> sourceParameters;
};

struct StructuredLayerOutputNames {
    std::string_view layer;
    std::string_view slot;
    std::string_view sourceNode;
    std::string_view layerOutputNode;
    std::string_view positionParameter;
    std::string_view anchorParameter;
    std::string_view scaleParameter;
    std::string_view rotationParameter;
    std::string_view opacityParameter;
    std::string_view blendModeParameter;
    std::string_view sourceToLayerEdge;
    std::string_view layerToStackEdge;
};

// Allocation ORDER matches the registered Layer Output parameter order -- position, anchor, scale,
// rotation, opacity, blendMode -- so the ids a layer publishes read in the same sequence the
// properties grid shows. The transform breadth slice (task S4) inserted anchor/scale/rotation
// between position and opacity and the blend-mode slice appends after opacity, both of which shift
// the ids of a newly created layer; nothing persisted depends on a particular id value, and the
// operation still publishes every id by NAME rather than by position.
[[nodiscard]] std::optional<StructuredLayerIds>
allocateStructuredLayerIds(document::IdAllocator& allocator,
                           const std::size_t sourceParameterCount) {
    const auto sourceNodeId = allocator.allocateNode();
    const auto layerOutputNodeId = allocator.allocateNode();
    const auto sourceToLayerEdgeId = allocator.allocateEdge();
    const auto layerToStackEdgeId = allocator.allocateEdge();
    const auto layerId = allocator.allocateLayer();
    const auto slotId = allocator.allocateLayerSlot();
    std::vector<document::ParameterId> sourceParameterIds;
    sourceParameterIds.reserve(sourceParameterCount);
    bool sourceParametersAllocated = true;
    for (std::size_t index = 0; index < sourceParameterCount; ++index) {
        const auto sourceParameterId = allocator.allocateParameter();
        if (!sourceParameterId.has_value()) {
            sourceParametersAllocated = false;
            break;
        }
        sourceParameterIds.push_back(*sourceParameterId);
    }
    const auto positionParameterId = allocator.allocateParameter();
    const auto anchorParameterId = allocator.allocateParameter();
    const auto scaleParameterId = allocator.allocateParameter();
    const auto rotationParameterId = allocator.allocateParameter();
    const auto opacityParameterId = allocator.allocateParameter();
    const auto blendModeParameterId = allocator.allocateParameter();
    if (!sourceNodeId.has_value() || !layerOutputNodeId.has_value() ||
        !sourceToLayerEdgeId.has_value() || !layerToStackEdgeId.has_value() ||
        !layerId.has_value() || !slotId.has_value() || !sourceParametersAllocated ||
        !positionParameterId.has_value() || !anchorParameterId.has_value() ||
        !scaleParameterId.has_value() || !rotationParameterId.has_value() ||
        !opacityParameterId.has_value() || !blendModeParameterId.has_value()) {
        return std::nullopt;
    }
    return StructuredLayerIds{*sourceNodeId,
                              *layerOutputNodeId,
                              *sourceToLayerEdgeId,
                              *layerToStackEdgeId,
                              *layerId,
                              *slotId,
                              std::move(sourceParameterIds),
                              *positionParameterId,
                              *anchorParameterId,
                              *scaleParameterId,
                              *rotationParameterId,
                              *opacityParameterId,
                              *blendModeParameterId};
}

[[nodiscard]] OperationResult
addStructuredLayer(document::Draft& draft, document::Composition& composition,
                   const std::string& name, StructuredLayerDescriptor descriptor,
                   const document::Vec2d position, const double opacity,
                   const StructuredLayerOutputNames& outputNames) {
    const auto ids = allocateStructuredLayerIds(draft.ids(), descriptor.sourceParameters.size());
    if (!ids.has_value()) {
        return exhaustedIds();
    }

    auto& parameters = composition.parameters();
    std::vector<document::ParameterBinding> sourceBindings;
    sourceBindings.reserve(descriptor.sourceParameters.size());
    for (std::size_t index = 0; index < descriptor.sourceParameters.size(); ++index) {
        auto& sourceParameter = descriptor.sourceParameters[index];
        const auto parameterId = ids->sourceParameterIds[index];
        if (!parameters.insert({parameterId, std::string(sourceParameter.schemaKey),
                                document::ConstantValueSource{std::move(sourceParameter.value)}})) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Layer parameters could not be inserted");
        }
        sourceBindings.push_back({std::string(sourceParameter.role), parameterId});
    }
    // The three transform breadth parameters and the blend mode are always created at their schema
    // defaults -- the identity transform and Normal blending -- so "add a layer" means exactly what
    // it meant before task S4 and the caller needs no new arguments. Anchor, scale, rotation, and
    // the blend mode are authored afterwards like any other parameter, through
    // SetParameterConstant.
    if (!parameters.insert({ids->positionParameterId,
                            std::string(document::kPositionParameterSchemaKey),
                            document::ConstantValueSource{position}}) ||
        !parameters.insert({ids->anchorParameterId,
                            std::string(document::kAnchorParameterSchemaKey),
                            document::ConstantValueSource{document::kDefaultAnchor}}) ||
        !parameters.insert({ids->scaleParameterId, std::string(document::kScaleParameterSchemaKey),
                            document::ConstantValueSource{document::kDefaultScale}}) ||
        !parameters.insert({ids->rotationParameterId,
                            std::string(document::kRotationParameterSchemaKey),
                            document::ConstantValueSource{document::kDefaultRotationDegrees}}) ||
        !parameters.insert({ids->opacityParameterId,
                            std::string(document::kOpacityParameterSchemaKey),
                            document::ConstantValueSource{opacity}}) ||
        !parameters.insert({ids->blendModeParameterId,
                            std::string(document::kBlendModeParameterSchemaKey),
                            document::ConstantValueSource{document::kDefaultBlendModeValue}})) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Layer parameters could not be inserted");
    }

    auto& graph = composition.graph();
    document::NodeRecord sourceNode{
        ids->sourceNodeId,
        std::string(descriptor.sourceNodeType),
        std::move(sourceBindings),
        descriptor.sourceNodeSchemaVersion,
    };
    document::NodeRecord layerOutputNode{
        ids->layerOutputNodeId,
        std::string(document::kLayerOutputNodeType),
        {
            {std::string(document::kPositionParameterRole), ids->positionParameterId},
            {std::string(document::kAnchorParameterRole), ids->anchorParameterId},
            {std::string(document::kScaleParameterRole), ids->scaleParameterId},
            {std::string(document::kRotationParameterRole), ids->rotationParameterId},
            {std::string(document::kOpacityParameterRole), ids->opacityParameterId},
            {std::string(document::kBlendModeParameterRole), ids->blendModeParameterId},
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
    // Where the new layer LANDS in the stack: on top, which is entry ZERO. The evaluator folds the
    // stack from its last entry to its first, so entry zero is the topmost layer and the timeline
    // lists it first. Appending put every new layer underneath every existing one instead -- so an
    // artist who added a layer and changed its blend mode saw nothing change, because the layer
    // they had just made was beneath an opaque one and had only the transparent backdrop under
    // itself, where every mode folds to Normal. That was the substance of the "blend modes are not
    // working" report, and every comparable tool adds a layer on top.
    const auto existingTop = graph.layerStack().entries().empty()
                                 ? std::nullopt
                                 : std::optional(graph.layerStack().entries().front().slotId);
    if (!graph.addNode(std::move(sourceNode)) || !graph.addNode(std::move(layerOutputNode)) ||
        !graph.addLayerOutput(std::move(layerBoundary)) ||
        !graph.layerStack().append({ids->slotId, ids->layerId}) ||
        !graph.layerStack().moveBefore(ids->slotId, existingTop) ||
        !graph.addEdge(std::move(sourceToLayerEdge)) ||
        !graph.addEdge(std::move(layerToStackEdge))) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Layer topology could not be inserted");
    }

    const auto defaults = document::defaultNodeLayout(graph.nodes());
    composition.nodeLayout().try_emplace(ids->sourceNodeId, defaults.at(ids->sourceNodeId));
    composition.nodeLayout().try_emplace(ids->layerOutputNodeId,
                                         defaults.at(ids->layerOutputNodeId));
    std::vector<OperationOutput> outputs{
        {std::string(outputNames.layer), DurableObjectId{ids->layerId}},
        {std::string(outputNames.slot), DurableObjectId{ids->slotId}},
        {std::string(outputNames.sourceNode), DurableObjectId{ids->sourceNodeId}},
        {std::string(outputNames.layerOutputNode), DurableObjectId{ids->layerOutputNodeId}},
        {std::string(outputNames.positionParameter), DurableObjectId{ids->positionParameterId}},
        {std::string(outputNames.anchorParameter), DurableObjectId{ids->anchorParameterId}},
        {std::string(outputNames.scaleParameter), DurableObjectId{ids->scaleParameterId}},
        {std::string(outputNames.rotationParameter), DurableObjectId{ids->rotationParameterId}},
        {std::string(outputNames.opacityParameter), DurableObjectId{ids->opacityParameterId}},
        {std::string(outputNames.blendModeParameter), DurableObjectId{ids->blendModeParameterId}},
        {std::string(outputNames.sourceToLayerEdge), DurableObjectId{ids->sourceToLayerEdgeId}},
        {std::string(outputNames.layerToStackEdge), DurableObjectId{ids->layerToStackEdgeId}},
    };
    for (std::size_t index = 0; index < descriptor.sourceParameters.size(); ++index) {
        outputs.push_back({std::string(descriptor.sourceParameters[index].outputName),
                           DurableObjectId{ids->sourceParameterIds[index]}});
    }
    return OperationResult::applied(std::move(outputs));
}

} // namespace

std::string_view AddShapeLayer::typeId() const noexcept { return "bloom.layer.add-shape"; }
OperationResult AddShapeLayer::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return invalidComposition(composition_);
    if (kind_ < document::ShapeKind::Rectangle || kind_ > document::ShapeKind::Path)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Unknown shape kind");
    if (geometry_.position &&
        (!std::isfinite(geometry_.position->x) || !std::isfinite(geometry_.position->y)))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Shape position must be finite");
    const auto definition = document::shapeDefinition();
    StructuredLayerDescriptor descriptor{document::kShapeSourceNodeType, 1, "image", {}};
    for (const auto& parameter : definition.parameters) {
        auto value = parameter.defaultValue;
        if (parameter.role == "kind")
            value = static_cast<std::int64_t>(kind_);
        if (kind_ == document::ShapeKind::Line ||
            (kind_ == document::ShapeKind::Path && geometry_.path && !geometry_.path->closed)) {
            if (parameter.role == "fillEnabled")
                value = false;
            if (parameter.role == "strokeEnabled")
                value = true;
            if (parameter.role == "strokeColor")
                value = core::Color4d{1, 1, 1, 1};
        }
        if (parameter.role == "size" && geometry_.size)
            value = *geometry_.size;
        if (parameter.role == "lineStart" && geometry_.lineStart)
            value = *geometry_.lineStart;
        if (parameter.role == "lineEnd" && geometry_.lineEnd)
            value = *geometry_.lineEnd;
        if (parameter.role == "path" && geometry_.path)
            value = *geometry_.path;
        if (!document::shapeConstantMatchesSchema(parameter.schemaKey, value))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Invalid shape geometry");
        descriptor.sourceParameters.push_back(
            {parameter.role, parameter.schemaKey, std::move(value), parameter.role});
    }
    return addStructuredLayer(
        draft, *composition, std::string(document::shapeKindName(kind_)), descriptor,
        geometry_.position.value_or(
            document::Vec2d{static_cast<double>(composition->format().width()) / 2.0,
                            static_cast<double>(composition->format().height()) / 2.0}),
        1.0,
        {"layer", "slot", "shapeNode", "layerOutputNode", "positionParameter", "anchorParameter",
         "scaleParameter", "rotationParameter", "opacityParameter", "blendModeParameter",
         "shapeToLayerEdge", "layerToStackEdge"});
}

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
        {document::kSolidSourceNodeType,
         document::kSolidSourceNodeSchemaVersion,
         document::kSolidSourceOutputPort,
         {{document::kSolidColorParameterRole, document::kSolidColorParameterSchemaKey, color_,
           kAddSolidLayerColorParameterOutput},
          {document::kSolidWidthParameterRole, document::kSolidWidthParameterSchemaKey,
           static_cast<double>(composition->format().width()), "widthParameter"},
          {document::kSolidHeightParameterRole, document::kSolidHeightParameterSchemaKey,
           static_cast<double>(composition->format().height()), "heightParameter"}}},
        useCompositionCentre_
            ? document::Vec2d{static_cast<double>(composition->format().width()) / 2.0,
                              static_cast<double>(composition->format().height()) / 2.0}
            : position_,
        opacity_,
        {kAddSolidLayerLayerOutput, kAddSolidLayerSlotOutput, kAddSolidLayerSolidNodeOutput,
         kAddSolidLayerLayerOutputNodeOutput, kAddSolidLayerPositionParameterOutput,
         kAddSolidLayerAnchorParameterOutput, kAddSolidLayerScaleParameterOutput,
         kAddSolidLayerRotationParameterOutput, kAddSolidLayerOpacityParameterOutput,
         kAddSolidLayerBlendModeParameterOutput, kAddSolidLayerSolidToLayerEdgeOutput,
         kAddSolidLayerLayerToStackEdgeOutput});
}

OperationResult AddImageLayer::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    const auto* asset = draft.project().findAsset(asset_);
    if (!composition || !asset)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Image asset or composition does not exist");
    const std::string name =
        asset->kind == document::AssetKind::Sequence
            ? asset->manifest.pattern
            : asset->locator.path.substr(asset->locator.path.find_last_of('/') + 1);
    return addStructuredLayer(
        draft, *composition, name,
        {"bloom.image-source",
         1,
         "image",
         {{"asset", "bloom.image.asset", std::to_string(asset_.value()), "assetParameter"},
          {"startFrame", "bloom.image.start-frame", std::int64_t{0}, "startFrameParameter"},
          {"loopMode", "bloom.image.loop-mode", std::int64_t{0}, "loopModeParameter"},
          {"colorSpace", "bloom.image.color-space", std::int64_t{0}, "colorSpaceParameter"},
          {"premultiply", "bloom.image.premultiply", true, "premultiplyParameter"}}},
        {static_cast<double>(composition->format().width()) / 2.0,
         static_cast<double>(composition->format().height()) / 2.0},
        1.0,
        {"layer", "slot", "imageNode", "layerOutputNode", "positionParameter", "anchorParameter",
         "scaleParameter", "rotationParameter", "opacityParameter", "blendModeParameter",
         "imageToLayerEdge", "layerToStackEdge"});
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

    if (!std::isfinite(size_) || size_ <= 0.0 || size_ > document::kMaximumTextSizePixels) {
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "Text layer size must be finite, greater than zero, and no more than " +
                std::to_string(static_cast<std::int64_t>(document::kMaximumTextSizePixels)) +
                " pixels");
    }
    if (!color_.isValid()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Text layer color must be finite with alpha between zero "
                                         "and one");
    }
    // Empty content is accepted deliberately: a text layer the artist has not typed into yet is a
    // real, selectable, editable layer that simply renders nothing, and refusing it would make "add
    // a text layer, then type" impossible.
    if (!core::isValidUtf8(text_)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Text layer content must be valid UTF-8");
    }

    return addStructuredLayer(
        draft, *composition, name_,
        {document::kTextSourceNodeType,
         document::kTextSourceNodeSchemaVersion,
         document::kTextSourceOutputPort,
         // Order matches the registered text definition's parameter order exactly.
         {{document::kTextParameterRole, document::kTextParameterSchemaKey, text_,
           kAddTextLayerTextParameterOutput},
          {document::kTextSizeParameterRole, document::kTextSizeParameterSchemaKey, size_,
           kAddTextLayerSizeParameterOutput},
          {document::kTextColorParameterRole, document::kTextColorParameterSchemaKey, color_,
           kAddTextLayerColorParameterOutput},
          {document::kTextAlignmentParameterRole, document::kTextAlignmentParameterSchemaKey,
           std::int64_t{0}, "alignmentParameter"},
          {document::kTextLineHeightParameterRole, document::kTextLineHeightParameterSchemaKey, 1.0,
           "lineHeightParameter"},
          {document::kTextLetterSpacingParameterRole,
           document::kTextLetterSpacingParameterSchemaKey, 0.0, "letterSpacingParameter"},
          {document::kTextFontParameterRole, document::kTextFontParameterSchemaKey,
           document::kDefaultTextFontValue, kAddTextLayerFontParameterOutput}}},
        useCompositionCentre_
            ? document::Vec2d{static_cast<double>(composition->format().width()) / 2.0,
                              static_cast<double>(composition->format().height()) / 2.0}
            : position_,
        opacity_,
        {kAddTextLayerLayerOutput, kAddTextLayerSlotOutput, kAddTextLayerTextNodeOutput,
         kAddTextLayerLayerOutputNodeOutput, kAddTextLayerPositionParameterOutput,
         kAddTextLayerAnchorParameterOutput, kAddTextLayerScaleParameterOutput,
         kAddTextLayerRotationParameterOutput, kAddTextLayerOpacityParameterOutput,
         kAddTextLayerBlendModeParameterOutput, kAddTextLayerTextToLayerEdgeOutput,
         kAddTextLayerLayerToStackEdgeOutput});
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

std::string_view AddComposition::typeId() const noexcept { return "bloom.composition.add"; }

OperationResult AddComposition::apply(document::Draft& draft) const {
    if (!document::isValidHumanFacingName(name_)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition name is invalid");
    }
    if (duration_ <= core::RationalTime{}) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition duration must be greater than zero");
    }
    if (!background_.isValid())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Invalid composition background colour");
    const auto format = document::CompositionFormat::create(format_.width(), format_.height(),
                                                            format_.pixelAspect(), frameRate_);
    if (!format) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition format is invalid");
    }

    const auto compositionId = draft.ids().allocateComposition();
    const auto layerStackNodeId = draft.ids().allocateNode();
    const auto outputNodeId = draft.ids().allocateNode();
    const auto outputEdgeId = draft.ids().allocateEdge();
    if (!compositionId || !layerStackNodeId || !outputNodeId || !outputEdgeId) {
        return exhaustedIds();
    }

    document::CanonicalGraph graph(*layerStackNodeId);
    if (!graph.addNode({*layerStackNodeId,
                        std::string(document::kLayerStackNodeType),
                        {},
                        document::kLayerStackNodeSchemaVersion}) ||
        !graph.addNode({*outputNodeId,
                        std::string(document::kCompositionOutputNodeType),
                        {},
                        document::kCompositionOutputNodeSchemaVersion}) ||
        !graph.addEdge({*outputEdgeId,
                        {*layerStackNodeId, std::string(document::kLayerStackOutputPort)},
                        document::NodeInputRef{
                            *outputNodeId, std::string(document::kCompositionOutputInputPort)}})) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition topology could not be created");
    }
    graph.setCompositionOutput(
        {*outputNodeId, std::string(document::kCompositionOutputOutputPort)});

    if (!draft.project().addComposition(
            document::Composition(*compositionId, name_, duration_, std::move(graph), *format))) {
        return OperationResult::rejected(OperationIssueCode::DuplicateId,
                                         "Composition could not be added");
    }
    draft.project().findComposition(*compositionId)->setBackgroundColor(background_);
    return OperationResult::applied({{std::string(kAddCompositionOutput), *compositionId}});
}

std::string_view DeleteComposition::typeId() const noexcept { return "bloom.composition.delete"; }

OperationResult DeleteComposition::apply(document::Draft& draft) const {
    if (draft.project().findComposition(compositionId_) == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (draft.project().compositions().size() <= 1) {
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "The last composition cannot be deleted");
    }
    if (!draft.project().removeComposition(compositionId_)) {
        return invalidComposition(compositionId_);
    }
    return OperationResult::applied();
}

std::string_view DuplicateComposition::typeId() const noexcept {
    return "bloom.composition.duplicate";
}

OperationResult DuplicateComposition::apply(document::Draft& draft) const {
    const auto* source = draft.project().findComposition(compositionId_);
    if (source == nullptr) {
        return invalidComposition(compositionId_);
    }
    const std::string name = source->name() + " copy";
    if (!document::isValidHumanFacingName(name)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Duplicated composition name is invalid");
    }
    auto copy = cloneComposition(draft, *source, name);
    if (!copy) {
        return exhaustedIds();
    }
    const auto copyId = copy->id();
    if (!draft.project().addComposition(std::move(*copy))) {
        return OperationResult::rejected(OperationIssueCode::DuplicateId,
                                         "Composition could not be duplicated");
    }
    return OperationResult::applied({{std::string(kDuplicateCompositionOutput), copyId}});
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

std::string_view SetCompositionSafeAreas::typeId() const noexcept {
    return "bloom.composition.set-safe-areas";
}

OperationResult SetCompositionSafeAreas::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (!settings_.isValid()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition safe areas are invalid");
    }
    if (composition->safeAreas() == settings_) {
        return OperationResult::noChange();
    }
    composition->setSafeAreas(settings_);
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

    const auto* parameter = composition->parameters().find(parameterId_);
    if (parameter == nullptr) {
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Parameter " + std::to_string(parameterId_.value()) +
                                             " does not exist");
    }
    if (composition->parameterLocked(parameterId_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer is locked");
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
    if (const auto* layer = composition->graph().findLayer(moving->layerId); layer && layer->locked)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer is locked");
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
