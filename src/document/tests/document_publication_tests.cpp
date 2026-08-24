#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace {

using bloom::core::RationalTime;
using bloom::document::AnimationCurveId;
using bloom::document::AnimationCurveSource;
using bloom::document::CanonicalGraph;
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

[[nodiscard]] Document makeDocument() {
    auto created =
        bloom::document::makeNewProject("Project", "Composition", RationalTime::fromInteger(1));
    return Document(std::move(created.project));
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
    ExpectationContext expectations;
    testPublicationReconcilesAllocatorHighWater(expectations);
    testAllocatorExhaustionSurvivesRestore(expectations);
    return expectations.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}
