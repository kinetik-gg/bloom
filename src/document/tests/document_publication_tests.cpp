#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using bloom::core::RationalTime;
using bloom::document::AnimationCurveId;
using bloom::document::AnimationCurveSource;
using bloom::document::CanonicalGraph;
using bloom::document::CommitStatus;
using bloom::document::Composition;
using bloom::document::CompositionId;
using bloom::document::ConstantValueSource;
using bloom::document::Document;
using bloom::document::DriverBindingId;
using bloom::document::DriverBindingSource;
using bloom::document::EdgeId;
using bloom::document::ExtensionRecordId;
using bloom::document::IdAllocator;
using bloom::document::IdAllocatorHighWater;
using bloom::document::KeyframeId;
using bloom::document::LayerId;
using bloom::document::LayerSlotId;
using bloom::document::LayerStackInputRef;
using bloom::document::NodeId;
using bloom::document::NodeInputRef;
using bloom::document::NodeRecord;
using bloom::document::ParameterId;
using bloom::document::Project;
using bloom::document::ProjectId;
using bloom::document::ScalarAnimationCurve;
using bloom::document::ScalarKeyframe;
using bloom::document::ValidationCode;
using bloom::document::ValidationResult;
using bloom::document::Vec2d;

static_assert(std::is_aggregate_v<IdAllocatorHighWater>);
static_assert(std::is_trivially_copyable_v<IdAllocatorHighWater>);

class ExpectationContext final {
  public:
    bool expect(const bool condition, std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return true;
        }

        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
        return false;
    }

    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

template <typename Id> [[nodiscard]] constexpr Id id(const std::uint64_t value) noexcept {
    return Id::fromRaw(value);
}

[[nodiscard]] bool hasOnlyIssueAt(const ValidationResult& validation, const ValidationCode code,
                                  const std::string_view path) {
    return validation.issues().size() == 1 && validation.issues().front().code == code &&
           validation.issues().front().path == path;
}

struct SingleLayerCompositionIds final {
    CompositionId composition;
    NodeId sourceNode;
    NodeId layerOutputNode;
    NodeId stackNode;
    NodeId outputNode;
    NodeId parameterReferenceNode;
    EdgeId sourceEdge;
    EdgeId stackEdge;
    EdgeId outputEdge;
    ParameterId positionParameter;
    ParameterId opacityParameter;
    LayerId layer;
    LayerSlotId slot;
};

[[nodiscard]] constexpr SingleLayerCompositionIds
compositionIds(const std::uint64_t composition, const std::uint64_t base) noexcept {
    return {
        id<CompositionId>(composition),
        id<NodeId>(base),
        id<NodeId>(base + 1),
        id<NodeId>(base + 2),
        id<NodeId>(base + 3),
        id<NodeId>(base + 4),
        id<EdgeId>(base),
        id<EdgeId>(base + 1),
        id<EdgeId>(base + 2),
        id<ParameterId>(base),
        id<ParameterId>(base + 1),
        id<LayerId>(base),
        id<LayerSlotId>(base),
    };
}

[[nodiscard]] Composition makeSingleLayerComposition(const SingleLayerCompositionIds& ids) {
    CanonicalGraph graph(ids.stackNode);
    NodeRecord sourceNode{ids.sourceNode, "com.example.source", {}, 1};
    NodeRecord layerOutputNode{
        ids.layerOutputNode,
        std::string(bloom::document::kLayerOutputNodeType),
        {
            {std::string(bloom::document::kPositionParameterRole), ids.positionParameter},
            {std::string(bloom::document::kOpacityParameterRole), ids.opacityParameter},
        },
        bloom::document::kLayerOutputNodeSchemaVersion,
    };
    NodeRecord stackNode{ids.stackNode,
                         std::string(bloom::document::kLayerStackNodeType),
                         {},
                         bloom::document::kLayerStackNodeSchemaVersion};
    NodeRecord outputNode{ids.outputNode,
                          std::string(bloom::document::kCompositionOutputNodeType),
                          {},
                          bloom::document::kCompositionOutputNodeSchemaVersion};
    NodeRecord parameterReferenceNode{ids.parameterReferenceNode,
                                      "com.example.parameter-reference",
                                      {{"value", ids.positionParameter}},
                                      1};

    const bool graphBuilt =
        graph.addNode(std::move(sourceNode)) && graph.addNode(std::move(layerOutputNode)) &&
        graph.addNode(std::move(stackNode)) && graph.addNode(std::move(outputNode)) &&
        graph.addNode(std::move(parameterReferenceNode)) &&
        graph.addLayerOutput({ids.layerOutputNode, ids.layer, "Layer",
                              std::string(bloom::document::kLayerOutputOutputPort)}) &&
        graph.layerStack().append({ids.slot, ids.layer}) &&
        graph.addEdge({ids.sourceEdge,
                       {ids.sourceNode, "image"},
                       NodeInputRef{ids.layerOutputNode,
                                    std::string(bloom::document::kLayerOutputContentInputPort)}}) &&
        graph.addEdge(
            {ids.stackEdge,
             {ids.layerOutputNode, std::string(bloom::document::kLayerOutputOutputPort)},
             LayerStackInputRef{ids.stackNode, ids.slot,
                                std::string(bloom::document::kLayerStackContentInputRole)}}) &&
        graph.addEdge({ids.outputEdge,
                       {ids.stackNode, std::string(bloom::document::kLayerStackOutputPort)},
                       NodeInputRef{ids.outputNode,
                                    std::string(bloom::document::kCompositionOutputInputPort)}});
    graph.setCompositionOutput(
        {ids.outputNode, std::string(bloom::document::kCompositionOutputOutputPort)});

    Composition composition(ids.composition, "Composition", RationalTime::fromInteger(1),
                            std::move(graph));
    const bool parametersBuilt =
        composition.parameters().insert({ids.positionParameter,
                                         std::string(bloom::document::kPositionParameterSchemaKey),
                                         ConstantValueSource{Vec2d{0.0, 0.0}}}) &&
        composition.parameters().insert({ids.opacityParameter,
                                         std::string(bloom::document::kOpacityParameterSchemaKey),
                                         ConstantValueSource{1.0}});
    if (!graphBuilt || !parametersBuilt) {
        throw std::logic_error("Could not build global ID validation fixture");
    }
    return composition;
}

[[nodiscard]] Project
makeProject(const SingleLayerCompositionIds& first,
            const std::optional<SingleLayerCompositionIds>& second = std::nullopt) {
    Project project(id<ProjectId>(1), "Project");
    if (!project.addComposition(makeSingleLayerComposition(first)) ||
        (second.has_value() && !project.addComposition(makeSingleLayerComposition(*second)))) {
        throw std::logic_error("Could not build multi-composition project fixture");
    }
    return project;
}

enum class DeclarationNamespace : std::uint8_t {
    Node,
    Edge,
    Parameter,
    Layer,
    LayerSlot,
};

struct CollisionCase final {
    DeclarationNamespace idNamespace;
    std::string_view path;
    std::string_view label;
};

inline constexpr std::array kCollisionCases{
    CollisionCase{DeclarationNamespace::Node, "compositions[2].graph.nodes[100].id", "node"},
    CollisionCase{DeclarationNamespace::Edge, "compositions[2].graph.edges[100].id", "edge"},
    CollisionCase{DeclarationNamespace::Parameter, "compositions[2].parameters[100].id",
                  "parameter"},
    CollisionCase{DeclarationNamespace::Layer, "compositions[2].graph.layerOutputs[100].layerId",
                  "layer"},
    CollisionCase{DeclarationNamespace::LayerSlot,
                  "compositions[2].graph.layerStack.entries[100].slotId", "layer-slot"},
};

[[nodiscard]] SingleLayerCompositionIds
withCollision(SingleLayerCompositionIds second, const SingleLayerCompositionIds& first,
              const DeclarationNamespace idNamespace) noexcept {
    switch (idNamespace) {
    case DeclarationNamespace::Node:
        second.sourceNode = first.sourceNode;
        break;
    case DeclarationNamespace::Edge:
        second.sourceEdge = first.sourceEdge;
        break;
    case DeclarationNamespace::Parameter:
        second.positionParameter = first.positionParameter;
        break;
    case DeclarationNamespace::Layer:
        second.layer = first.layer;
        break;
    case DeclarationNamespace::LayerSlot:
        second.slot = first.slot;
        break;
    }
    return second;
}

[[nodiscard]] Document makeDocument() {
    auto created =
        bloom::document::makeNewProject("Project", "Composition", RationalTime::fromInteger(1));
    return Document(std::move(created.project));
}

[[nodiscard]] Project makePersistedAllocatorProject() {
    auto project = makeProject(compositionIds(1, 100));
    auto* composition = project.findComposition(id<CompositionId>(1));
    const bool animationStateBuilt =
        composition != nullptr &&
        composition->animationCurves().insert(ScalarAnimationCurve{
            id<AnimationCurveId>(200),
            {ScalarKeyframe{id<KeyframeId>(201), RationalTime::fromInteger(0), 0.75}},
        }) &&
        composition->parameters().insert({id<ParameterId>(202),
                                          std::string(bloom::document::kOpacityParameterSchemaKey),
                                          AnimationCurveSource{id<AnimationCurveId>(200)}}) &&
        composition->parameters().insert({id<ParameterId>(203), "com.example.driver-value",
                                          DriverBindingSource{id<DriverBindingId>(204)}});
    if (!animationStateBuilt || !project.validate().ok()) {
        throw std::logic_error("Could not build persisted allocator fixture");
    }
    return project;
}

[[nodiscard]] IdAllocatorHighWater persistedAllocatorHighWater() noexcept {
    return {
        .composition = 9,
        .node = 150,
        .edge = 140,
        .layer = 130,
        .layerSlot = 120,
        .parameter = 250,
        .animationCurve = 220,
        .keyframe = 230,
        .driverBinding = 240,
        .extensionRecord = 260,
    };
}

void testInclusiveAllocatorState(ExpectationContext& expectations) {
    IdAllocator fresh;
    expectations.expect(fresh.highWater() == IdAllocatorHighWater{},
                        "a fresh allocator exposes zero inclusive high-water values");
    expectations.expect(!fresh.covers(NodeId{}) && !fresh.covers(id<NodeId>(1)),
                        "a fresh allocator covers no invalid or unissued node ID");

    const auto firstComposition = fresh.allocateComposition();
    const auto firstNode = fresh.allocateNode();
    const auto firstEdge = fresh.allocateEdge();
    const auto firstLayer = fresh.allocateLayer();
    const auto firstLayerSlot = fresh.allocateLayerSlot();
    const auto firstParameter = fresh.allocateParameter();
    const auto firstCurve = fresh.allocateAnimationCurve();
    const auto firstKeyframe = fresh.allocateKeyframe();
    const auto firstDriver = fresh.allocateDriverBinding();
    const auto firstExtension = fresh.allocateExtensionRecord();
    const IdAllocatorHighWater allOne{
        .composition = 1,
        .node = 1,
        .edge = 1,
        .layer = 1,
        .layerSlot = 1,
        .parameter = 1,
        .animationCurve = 1,
        .keyframe = 1,
        .driverBinding = 1,
        .extensionRecord = 1,
    };
    expectations.expect(
        firstComposition == id<CompositionId>(1) && firstNode == id<NodeId>(1) &&
            firstEdge == id<EdgeId>(1) && firstLayer == id<LayerId>(1) &&
            firstLayerSlot == id<LayerSlotId>(1) && firstParameter == id<ParameterId>(1) &&
            firstCurve == id<AnimationCurveId>(1) && firstKeyframe == id<KeyframeId>(1) &&
            firstDriver == id<DriverBindingId>(1) && firstExtension == id<ExtensionRecordId>(1) &&
            fresh.highWater() == allOne,
        "each typed namespace independently issues one from a fresh allocator");

    auto deletedGap = IdAllocator::fromHighWater(persistedAllocatorHighWater());
    expectations.expect(
        deletedGap.highWater() == persistedAllocatorHighWater() &&
            deletedGap.allocateNode() == id<NodeId>(151) &&
            deletedGap.allocateDriverBinding() == id<DriverBindingId>(241) &&
            deletedGap.allocateExtensionRecord() == id<ExtensionRecordId>(261),
        "restoration preserves deleted gaps and allocates above inclusive watermarks");

    IdAllocatorHighWater sharedRaw;
    sharedRaw.node = 41;
    sharedRaw.edge = 41;
    sharedRaw.driverBinding = 41;
    sharedRaw.extensionRecord = 41;
    auto typed = IdAllocator::fromHighWater(sharedRaw);
    const auto typedNode = typed.allocateNode();
    const auto typedEdge = typed.allocateEdge();
    const auto typedDriver = typed.allocateDriverBinding();
    const auto typedExtension = typed.allocateExtensionRecord();
    expectations.expect(
        typed.covers(id<NodeId>(41)) && typed.covers(id<EdgeId>(41)) &&
            typed.covers(id<DriverBindingId>(41)) && typed.covers(id<ExtensionRecordId>(41)) &&
            typedNode == id<NodeId>(42) && typedEdge == id<EdgeId>(42) &&
            typedDriver == id<DriverBindingId>(42) && typedExtension == id<ExtensionRecordId>(42),
        "equal raw IDs remain independent across typed namespaces");

    IdAllocatorHighWater uneven;
    uneven.node = 7;
    uneven.edge = 3;
    const auto coverage = IdAllocator::fromHighWater(uneven);
    expectations.expect(coverage.covers(id<NodeId>(7)) && !coverage.covers(id<NodeId>(8)) &&
                            !coverage.covers(id<EdgeId>(7)) && coverage.covers(id<EdgeId>(3)),
                        "coverage checks use the matching typed namespace only");

    IdAllocatorHighWater maximum;
    maximum.node = std::numeric_limits<std::uint64_t>::max();
    maximum.extensionRecord = std::numeric_limits<std::uint64_t>::max();
    auto exhausted = IdAllocator::fromHighWater(maximum);
    exhausted.reserveExisting(id<NodeId>(1));
    expectations.expect(!exhausted.allocateNode().has_value() &&
                            !exhausted.allocateExtensionRecord().has_value() &&
                            exhausted.highWater() == maximum,
                        "a restored maximum watermark is permanent namespace exhaustion");

    IdAllocatorHighWater beforeMaximum;
    beforeMaximum.node = std::numeric_limits<std::uint64_t>::max() - 1;
    auto finalAllocation = IdAllocator::fromHighWater(beforeMaximum);
    expectations.expect(
        finalAllocation.allocateNode() == id<NodeId>(std::numeric_limits<std::uint64_t>::max()) &&
            !finalAllocation.allocateNode().has_value() &&
            finalAllocation.highWater().node == std::numeric_limits<std::uint64_t>::max(),
        "the maximum ID is issued once before the namespace becomes exhausted");
}

void testPersistedAllocatorConstruction(ExpectationContext& expectations) {
    Document inventoried(makePersistedAllocatorProject());
    const IdAllocatorHighWater declarations{
        .composition = 1,
        .node = 104,
        .edge = 102,
        .layer = 100,
        .layerSlot = 100,
        .parameter = 203,
        .animationCurve = 200,
        .keyframe = 201,
        .driverBinding = 204,
        .extensionRecord = 0,
    };
    expectations.expect(
        inventoried.snapshot().ids().highWater() == declarations,
        "ordinary document construction inventories every live allocator namespace");

    const auto persisted = persistedAllocatorHighWater();
    Document restored(makePersistedAllocatorProject(), persisted);
    const auto historical = restored.snapshot();
    expectations.expect(historical.ids().highWater() == persisted,
                        "persisted construction retains deleted gaps and extension state exactly");

    struct RequiredNamespace final {
        std::uint64_t IdAllocatorHighWater::* member;
        std::string_view label;
    };
    constexpr std::array requiredNamespaces{
        RequiredNamespace{&IdAllocatorHighWater::composition, "composition"},
        RequiredNamespace{&IdAllocatorHighWater::node, "node"},
        RequiredNamespace{&IdAllocatorHighWater::edge, "edge"},
        RequiredNamespace{&IdAllocatorHighWater::layer, "layer"},
        RequiredNamespace{&IdAllocatorHighWater::layerSlot, "layer-slot"},
        RequiredNamespace{&IdAllocatorHighWater::parameter, "parameter"},
        RequiredNamespace{&IdAllocatorHighWater::animationCurve, "animation-curve"},
        RequiredNamespace{&IdAllocatorHighWater::keyframe, "keyframe"},
        RequiredNamespace{&IdAllocatorHighWater::driverBinding, "driver-binding"},
    };
    for (const auto& requiredNamespace : requiredNamespaces) {
        auto belowDeclaration = declarations;
        --(belowDeclaration.*(requiredNamespace.member));
        bool rejected = false;
        try {
            [[maybe_unused]] Document invalid(makePersistedAllocatorProject(), belowDeclaration);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        const auto message =
            std::string("persisted construction rejects a watermark below a live ") +
            std::string(requiredNamespace.label) + " declaration";
        expectations.expect(rejected, message);
    }

    auto advancing = restored.draft(historical);
    const bool everyNamespaceAdvanced =
        advancing.ids().allocateComposition() == id<CompositionId>(10) &&
        advancing.ids().allocateNode() == id<NodeId>(151) &&
        advancing.ids().allocateEdge() == id<EdgeId>(141) &&
        advancing.ids().allocateLayer() == id<LayerId>(131) &&
        advancing.ids().allocateLayerSlot() == id<LayerSlotId>(121) &&
        advancing.ids().allocateParameter() == id<ParameterId>(251) &&
        advancing.ids().allocateAnimationCurve() == id<AnimationCurveId>(221) &&
        advancing.ids().allocateKeyframe() == id<KeyframeId>(231) &&
        advancing.ids().allocateDriverBinding() == id<DriverBindingId>(241) &&
        advancing.ids().allocateExtensionRecord() == id<ExtensionRecordId>(261);
    advancing.project().setName("Advanced");
    const auto committed = restored.commit(historical.revision(), std::move(advancing));
    if (!expectations.expect(everyNamespaceAdvanced && committed.committed() &&
                                 committed.snapshot.has_value(),
                             "a draft publishes advances in every allocator namespace")) {
        return;
    }
    if (!committed.snapshot.has_value()) {
        return;
    }
    const auto& committedSnapshot = *committed.snapshot;

    auto advanced = persisted;
    ++advanced.composition;
    ++advanced.node;
    ++advanced.edge;
    ++advanced.layer;
    ++advanced.layerSlot;
    ++advanced.parameter;
    ++advanced.animationCurve;
    ++advanced.keyframe;
    ++advanced.driverBinding;
    ++advanced.extensionRecord;
    expectations.expect(committedSnapshot.ids().highWater() == advanced,
                        "snapshot publication exposes the exact advanced watermarks");

    auto loweredDraft = restored.draft(committedSnapshot);
    loweredDraft.ids() = IdAllocator{};
    loweredDraft.project().setName("Attempted Lowering");
    const auto reconciled = restored.commit(committedSnapshot.revision(), std::move(loweredDraft));
    if (!expectations.expect(reconciled.committed() && reconciled.snapshot.has_value() &&
                                 reconciled.snapshot->ids().highWater() == advanced,
                             "commit cannot lower previously published allocator state")) {
        return;
    }
    if (!reconciled.snapshot.has_value()) {
        return;
    }

    const auto undone = restored.restore(reconciled.snapshot->revision(), historical);
    expectations.expect(undone.committed() && undone.snapshot.has_value() &&
                            undone.snapshot->ids().highWater() == advanced,
                        "restoring historical project truth cannot lower allocator state");
}

void testProjectGlobalDeclarationIds(ExpectationContext& expectations) {
    constexpr auto first = compositionIds(1, 100);
    constexpr auto second = [] {
        auto ids = compositionIds(2, 200);
        ids.sourceEdge = id<EdgeId>(104);
        return ids;
    }();
    const auto disjoint = makeProject(first, second);
    expectations.expect(first.sourceNode.value() == first.sourceEdge.value() &&
                            first.sourceNode.value() == first.positionParameter.value() &&
                            first.sourceNode.value() == first.layer.value() &&
                            first.sourceNode.value() == first.slot.value() &&
                            first.parameterReferenceNode.value() == second.sourceEdge.value(),
                        "the fixture reuses raw values across typed namespaces and compositions");
    expectations.expect(disjoint.validate().ok(),
                        "ordinary repeated parameter and layer references are not declarations");

    for (const auto& collision : kCollisionCases) {
        const auto collidingSecond = withCollision(second, first, collision.idNamespace);
        auto invalidProject = makeProject(first, collidingSecond);
        const auto validation = invalidProject.validate();
        bool constructorRejected = false;
        try {
            [[maybe_unused]] Document invalidDocument(std::move(invalidProject));
        } catch (const std::invalid_argument&) {
            constructorRejected = true;
        }
        const auto constructorMessage =
            std::string("document construction rejects a cross-composition ") +
            std::string(collision.label) + " declaration collision at its stable path";
        expectations.expect(
            hasOnlyIssueAt(validation, ValidationCode::DuplicateId, collision.path) &&
                constructorRejected,
            constructorMessage);

        Document document(makeProject(first));
        const auto before = document.snapshot();
        auto draft = document.draft(before);
        const bool compositionAdded =
            draft.project().addComposition(makeSingleLayerComposition(collidingSecond));
        const auto rejected = document.commit(before.revision(), std::move(draft));
        const auto after = document.snapshot();
        const auto commitMessage = std::string("commit rejects a cross-composition ") +
                                   std::string(collision.label) +
                                   " declaration collision without publishing it";
        expectations.expect(
            compositionAdded && rejected.status == CommitStatus::InvalidDraft &&
                !rejected.snapshot.has_value() &&
                hasOnlyIssueAt(rejected.validation, ValidationCode::DuplicateId, collision.path) &&
                after.revision() == before.revision() && after.project().compositions().size() == 1,
            commitMessage);
    }
}

void testPublicationReconcilesAllocatorHighWater(ExpectationContext& expectations) {
    auto document = makeDocument();
    const auto before = document.snapshot();
    auto draft = document.draft(before);

    constexpr auto positionId = id<ParameterId>(44);
    constexpr auto opacityId = id<ParameterId>(45);
    constexpr auto animationParameterId = id<ParameterId>(46);
    constexpr auto driverParameterId = id<ParameterId>(47);
    constexpr auto sourceNodeId = id<NodeId>(100);
    constexpr auto layerOutputNodeId = id<NodeId>(101);
    constexpr auto layerId = id<LayerId>(100);
    constexpr auto slotId = id<LayerSlotId>(100);
    constexpr auto sourceEdgeId = id<EdgeId>(100);
    constexpr auto stackEdgeId = id<EdgeId>(101);

    const bool layerInserted = [&] {
        auto* composition = draft.project().findComposition(id<CompositionId>(1));
        NodeRecord sourceNode{sourceNodeId, "com.example.source", {}, 1};
        NodeRecord layerOutputNode{
            layerOutputNodeId,
            std::string(bloom::document::kLayerOutputNodeType),
            {
                {std::string(bloom::document::kPositionParameterRole), positionId},
                {std::string(bloom::document::kOpacityParameterRole), opacityId},
            },
            bloom::document::kLayerOutputNodeSchemaVersion,
        };
        return composition != nullptr &&
               composition->animationCurves().insert(ScalarAnimationCurve{
                   id<AnimationCurveId>(100),
                   {ScalarKeyframe{id<KeyframeId>(100), RationalTime::fromInteger(0), 0.0}},
               }) &&
               composition->parameters().insert(
                   {positionId, std::string(bloom::document::kPositionParameterSchemaKey),
                    ConstantValueSource{Vec2d{10.0, 20.0}}}) &&
               composition->parameters().insert(
                   {opacityId, std::string(bloom::document::kOpacityParameterSchemaKey),
                    ConstantValueSource{0.5}}) &&
               composition->parameters().insert(
                   {animationParameterId, std::string(bloom::document::kOpacityParameterSchemaKey),
                    AnimationCurveSource{id<AnimationCurveId>(100)}}) &&
               composition->parameters().insert({driverParameterId, "com.example.driver",
                                                 DriverBindingSource{id<DriverBindingId>(100)}}) &&
               composition->graph().addNode(std::move(sourceNode)) &&
               composition->graph().addNode(std::move(layerOutputNode)) &&
               composition->graph().addLayerOutput(
                   {layerOutputNodeId, layerId, "Manual",
                    std::string(bloom::document::kLayerOutputOutputPort)}) &&
               composition->graph().layerStack().append({slotId, layerId}) &&
               composition->graph().addEdge(
                   {sourceEdgeId,
                    {sourceNodeId, "image"},
                    NodeInputRef{layerOutputNodeId,
                                 std::string(bloom::document::kLayerOutputContentInputPort)}}) &&
               composition->graph().addEdge(
                   {stackEdgeId,
                    {layerOutputNodeId, std::string(bloom::document::kLayerOutputOutputPort)},
                    LayerStackInputRef{composition->graph().layerStack().nodeId(), slotId,
                                       std::string(bloom::document::kLayerStackContentInputRole)}});
    }();

    CanonicalGraph secondGraph(id<NodeId>(200));
    const bool secondCompositionInserted =
        secondGraph.addNode({id<NodeId>(200),
                             std::string(bloom::document::kLayerStackNodeType),
                             {},
                             bloom::document::kLayerStackNodeSchemaVersion}) &&
        secondGraph.addNode({id<NodeId>(201),
                             std::string(bloom::document::kCompositionOutputNodeType),
                             {},
                             bloom::document::kCompositionOutputNodeSchemaVersion}) &&
        secondGraph.addEdge(
            {id<EdgeId>(200),
             {id<NodeId>(200), std::string(bloom::document::kLayerStackOutputPort)},
             NodeInputRef{id<NodeId>(201),
                          std::string(bloom::document::kCompositionOutputInputPort)}});
    secondGraph.setCompositionOutput(
        {id<NodeId>(201), std::string(bloom::document::kCompositionOutputOutputPort)});
    const bool projectExtended =
        secondCompositionInserted && draft.project().addComposition(Composition(
                                         id<CompositionId>(100), "Manual Composition",
                                         RationalTime::fromInteger(1), std::move(secondGraph)));

    const auto committed = document.commit(before.revision(), std::move(draft));
    if (!expectations.expect(layerInserted && projectExtended && committed.committed() &&
                                 committed.snapshot.has_value(),
                             "valid caller-supplied durable IDs publish successfully")) {
        return;
    }
    if (!committed.snapshot.has_value()) {
        return;
    }

    auto next = document.draft(*committed.snapshot);
    expectations.expect(next.ids().allocateComposition() == id<CompositionId>(101) &&
                            next.ids().allocateNode() == id<NodeId>(202) &&
                            next.ids().allocateEdge() == id<EdgeId>(201) &&
                            next.ids().allocateLayer() == id<LayerId>(101) &&
                            next.ids().allocateLayerSlot() == id<LayerSlotId>(101) &&
                            next.ids().allocateParameter() == id<ParameterId>(48) &&
                            next.ids().allocateAnimationCurve() == id<AnimationCurveId>(101) &&
                            next.ids().allocateKeyframe() == id<KeyframeId>(101) &&
                            next.ids().allocateDriverBinding() == id<DriverBindingId>(101),
                        "publication reconciles all currently durable allocator namespaces");
}

void testAllocatorExhaustionSurvivesRestore(ExpectationContext& expectations) {
    auto document = makeDocument();
    const auto historical = document.snapshot();
    auto exhaustedDraft = document.draft(historical);
    exhaustedDraft.ids().reserveExisting(id<NodeId>(std::numeric_limits<std::uint64_t>::max()));
    exhaustedDraft.ids().reserveExisting(
        id<ExtensionRecordId>(std::numeric_limits<std::uint64_t>::max()));
    exhaustedDraft.project().setName("Exhausted");
    const auto exhausted = document.commit(historical.revision(), std::move(exhaustedDraft));
    if (!expectations.expect(exhausted.committed() && exhausted.snapshot.has_value(),
                             "exhausted allocator fixture publishes")) {
        return;
    }
    if (!exhausted.snapshot.has_value()) {
        return;
    }

    const auto restored = document.restore(exhausted.snapshot->revision(), historical);
    if (!expectations.expect(restored.committed() && restored.snapshot.has_value(),
                             "historical project state restores after allocator exhaustion")) {
        return;
    }
    if (!restored.snapshot.has_value()) {
        return;
    }
    auto restoredDraft = document.draft(*restored.snapshot);
    expectations.expect(!restoredDraft.ids().allocateNode().has_value() &&
                            !restoredDraft.ids().allocateExtensionRecord().has_value(),
                        "restore preserves published exhaustion in independent namespaces");
}

} // namespace

int main() {
    try {
        ExpectationContext expectations;
        testInclusiveAllocatorState(expectations);
        testPersistedAllocatorConstruction(expectations);
        testProjectGlobalDeclarationIds(expectations);
        testPublicationReconcilesAllocatorHighWater(expectations);
        testAllocatorExhaustionSurvivesRestore(expectations);
        return expectations.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
