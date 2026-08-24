#include <bloom/core/rational_time.hpp>
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
using bloom::document::LayerId;
using bloom::document::LayerSlotId;
using bloom::document::LayerStackInputRef;
using bloom::document::NodeId;
using bloom::document::NodeInputRef;
using bloom::document::NodeRecord;
using bloom::document::ParameterId;
using bloom::document::Project;
using bloom::document::ProjectId;
using bloom::document::ValidationCode;
using bloom::document::ValidationResult;
using bloom::document::Vec2d;

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
               composition->parameters().insert(
                   {positionId, std::string(bloom::document::kPositionParameterSchemaKey),
                    ConstantValueSource{Vec2d{10.0, 20.0}}}) &&
               composition->parameters().insert(
                   {opacityId, std::string(bloom::document::kOpacityParameterSchemaKey),
                    ConstantValueSource{0.5}}) &&
               composition->parameters().insert(
                   {animationParameterId, "com.example.animation",
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

    auto next = document.draft(*committed.snapshot);
    expectations.expect(next.ids().allocateComposition() == id<CompositionId>(101) &&
                            next.ids().allocateNode() == id<NodeId>(202) &&
                            next.ids().allocateEdge() == id<EdgeId>(201) &&
                            next.ids().allocateLayer() == id<LayerId>(101) &&
                            next.ids().allocateLayerSlot() == id<LayerSlotId>(101) &&
                            next.ids().allocateParameter() == id<ParameterId>(48) &&
                            next.ids().allocateAnimationCurve() == id<AnimationCurveId>(101) &&
                            next.ids().allocateDriverBinding() == id<DriverBindingId>(101),
                        "publication reconciles all currently durable allocator namespaces");
}

void testAllocatorExhaustionSurvivesRestore(ExpectationContext& expectations) {
    auto document = makeDocument();
    const auto historical = document.snapshot();
    auto exhaustedDraft = document.draft(historical);
    exhaustedDraft.ids().reserveExisting(id<NodeId>(std::numeric_limits<std::uint64_t>::max()));
    exhaustedDraft.project().setName("Exhausted");
    const auto exhausted = document.commit(historical.revision(), std::move(exhaustedDraft));
    if (!expectations.expect(exhausted.committed() && exhausted.snapshot.has_value(),
                             "exhausted allocator fixture publishes")) {
        return;
    }

    const auto restored = document.restore(exhausted.snapshot->revision(), historical);
    if (!expectations.expect(restored.committed() && restored.snapshot.has_value(),
                             "historical project state restores after allocator exhaustion")) {
        return;
    }
    auto restoredDraft = document.draft(*restored.snapshot);
    expectations.expect(!restoredDraft.ids().allocateNode().has_value(),
                        "restore preserves the prior published exhausted allocator sentinel");
}

} // namespace

int main() {
    try {
        ExpectationContext expectations;
        testProjectGlobalDeclarationIds(expectations);
        testPublicationReconcilesAllocatorHighWater(expectations);
        testAllocatorExhaustionSurvivesRestore(expectations);
        return expectations.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
