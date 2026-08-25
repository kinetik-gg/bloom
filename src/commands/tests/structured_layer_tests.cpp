#include "command_test_support.hpp"

#include <bloom/core/pixel_aspect_ratio.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

namespace bloom::commands::test {
namespace {

using document::EdgeRecord;
using document::LayerStackEntry;
using document::NodeInputRef;
using document::NodeRecord;
using document::ParameterRecord;

struct SolidOutputIds final {
    LayerId layerId;
    LayerSlotId slotId;
    NodeId solidNodeId;
    NodeId layerOutputNodeId;
    ParameterId colorParameterId;
    ParameterId positionParameterId;
    ParameterId opacityParameterId;
    EdgeId solidToLayerEdgeId;
    EdgeId layerToStackEdgeId;
};

template <typename Id, std::size_t LeftSize, std::size_t RightSize>
[[nodiscard]] bool areDisjoint(const std::array<Id, LeftSize>& left,
                               const std::array<Id, RightSize>& right) {
    return std::ranges::none_of(
        left, [&right](const Id value) { return std::ranges::find(right, value) != right.end(); });
}

[[nodiscard]] std::optional<SolidOutputIds> solidOutputIds(const CommandResult& result) {
    const auto layerId = result.outputId<LayerId>(kAddSolidLayerLayerOutput);
    const auto slotId = result.outputId<LayerSlotId>(kAddSolidLayerSlotOutput);
    const auto solidNodeId = result.outputId<NodeId>(kAddSolidLayerSolidNodeOutput);
    const auto layerOutputNodeId = result.outputId<NodeId>(kAddSolidLayerLayerOutputNodeOutput);
    const auto colorParameterId = result.outputId<ParameterId>(kAddSolidLayerColorParameterOutput);
    const auto positionParameterId =
        result.outputId<ParameterId>(kAddSolidLayerPositionParameterOutput);
    const auto opacityParameterId =
        result.outputId<ParameterId>(kAddSolidLayerOpacityParameterOutput);
    const auto solidToLayerEdgeId = result.outputId<EdgeId>(kAddSolidLayerSolidToLayerEdgeOutput);
    const auto layerToStackEdgeId = result.outputId<EdgeId>(kAddSolidLayerLayerToStackEdgeOutput);
    if (!layerId || !slotId || !solidNodeId || !layerOutputNodeId || !colorParameterId ||
        !positionParameterId || !opacityParameterId || !solidToLayerEdgeId || !layerToStackEdgeId) {
        return std::nullopt;
    }
    return SolidOutputIds{*layerId,
                          *slotId,
                          *solidNodeId,
                          *layerOutputNodeId,
                          *colorParameterId,
                          *positionParameterId,
                          *opacityParameterId,
                          *solidToLayerEdgeId,
                          *layerToStackEdgeId};
}

void expectSolidState(TestContext& test, const document::Snapshot& snapshot,
                      const SolidOutputIds& ids, const std::string_view name,
                      const core::Color4d color, const Vec2d position, const double opacityValue) {
    const auto& value = composition(snapshot);
    const NodeRecord expectedSolidNode{
        ids.solidNodeId,
        std::string(document::kSolidSourceNodeType),
        {{std::string(document::kSolidColorParameterRole), ids.colorParameterId}},
        document::kSolidSourceNodeSchemaVersion,
    };
    const NodeRecord expectedLayerOutputNode{
        ids.layerOutputNodeId,
        std::string(document::kLayerOutputNodeType),
        {
            {std::string(document::kPositionParameterRole), ids.positionParameterId},
            {std::string(document::kOpacityParameterRole), ids.opacityParameterId},
        },
        document::kLayerOutputNodeSchemaVersion,
    };
    const LayerOutputBoundary expectedBoundary{ids.layerOutputNodeId, ids.layerId,
                                               std::string(name),
                                               std::string(document::kLayerOutputOutputPort)};
    const LayerStackEntry expectedStackEntry{ids.slotId, ids.layerId};
    const ParameterRecord expectedColorParameter{
        ids.colorParameterId,
        std::string(document::kSolidColorParameterSchemaKey),
        ConstantValueSource{color},
    };
    const ParameterRecord expectedPositionParameter{
        ids.positionParameterId,
        std::string(document::kPositionParameterSchemaKey),
        ConstantValueSource{position},
    };
    const ParameterRecord expectedOpacityParameter{
        ids.opacityParameterId,
        std::string(document::kOpacityParameterSchemaKey),
        ConstantValueSource{opacityValue},
    };
    const EdgeRecord expectedSolidToLayerEdge{
        ids.solidToLayerEdgeId,
        {ids.solidNodeId, std::string(document::kSolidSourceOutputPort)},
        NodeInputRef{ids.layerOutputNodeId, std::string(document::kLayerOutputContentInputPort)},
    };
    const EdgeRecord expectedLayerToStackEdge{
        ids.layerToStackEdgeId,
        {ids.layerOutputNodeId, std::string(document::kLayerOutputOutputPort)},
        LayerStackInputRef{value.graph().layerStack().nodeId(), ids.slotId,
                           std::string(document::kLayerStackContentInputRole)},
    };

    const auto* solidNode = value.graph().findNode(ids.solidNodeId);
    const auto* layerOutputNode = value.graph().findNode(ids.layerOutputNodeId);
    test.expect(solidNode != nullptr && *solidNode == expectedSolidNode,
                "solid source should preserve exact type, schema, and color binding");
    test.expect(layerOutputNode != nullptr && *layerOutputNode == expectedLayerOutputNode,
                "layer output should preserve exact schema, position, and opacity bindings");
    test.expect(std::ranges::find(value.graph().layerOutputs(), expectedBoundary) !=
                    value.graph().layerOutputs().end(),
                "solid layer boundary should preserve exact name, port, and stable IDs");
    const auto* entry = value.graph().layerStack().find(ids.slotId);
    test.expect(entry != nullptr && *entry == expectedStackEntry,
                "solid stack projection should preserve exact slot-to-layer identity");

    const auto* colorParameter = value.parameters().find(ids.colorParameterId);
    const auto* positionParameter = value.parameters().find(ids.positionParameterId);
    const auto* opacityParameter = value.parameters().find(ids.opacityParameterId);
    test.expect(colorParameter != nullptr && *colorParameter == expectedColorParameter,
                "solid color parameter should preserve exact schema and straight HDR value");
    test.expect(positionParameter != nullptr && *positionParameter == expectedPositionParameter,
                "solid position parameter should preserve exact schema and value");
    test.expect(opacityParameter != nullptr && *opacityParameter == expectedOpacityParameter,
                "solid opacity parameter should preserve exact schema and value");
    test.expect(std::ranges::find(value.graph().edges(), expectedSolidToLayerEdge) !=
                    value.graph().edges().end(),
                "solid source edge should preserve exact ports and identity");
    test.expect(std::ranges::find(value.graph().edges(), expectedLayerToStackEdge) !=
                    value.graph().edges().end(),
                "solid stack edge should preserve exact role, slot, and identity");
}

[[nodiscard]] bool hasSameTruth(const Composition& left, const Composition& right) {
    return left.id() == right.id() && left.name() == right.name() &&
           left.duration() == right.duration() && left.format() == right.format() &&
           std::ranges::equal(left.parameters().records(), right.parameters().records()) &&
           std::ranges::equal(left.graph().nodes(), right.graph().nodes()) &&
           std::ranges::equal(left.graph().edges(), right.graph().edges()) &&
           std::ranges::equal(left.graph().layerOutputs(), right.graph().layerOutputs()) &&
           std::ranges::equal(left.graph().layerStack().entries(),
                              right.graph().layerStack().entries()) &&
           left.graph().compositionOutput() == right.graph().compositionOutput();
}

void testAddTextLayerBuildsOneCanonicalTopology(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();
    const auto& originalComposition = composition(original);
    const auto originalNodeCount = originalComposition.graph().nodes().size();
    const auto originalEdgeCount = originalComposition.graph().edges().size();
    const auto originalParameterCount = originalComposition.parameters().records().size();

    Transaction add("Add text layer", original.revision());
    add.emplace<AddTextLayer>(kCompositionId, "Title", "Hello, Bloom!", Vec2d{960.0, 540.0}, 0.75);
    const auto result = stack.execute(std::move(add));
    test.expect(result.status == CommandStatus::Succeeded && result.outputs.size() == 9,
                "AddTextLayer should commit one topology edit and report every durable ID");

    const auto layerId = result.outputId<LayerId>(kAddTextLayerLayerOutput);
    const auto slotId = result.outputId<LayerSlotId>(kAddTextLayerSlotOutput);
    const auto textNodeId = result.outputId<NodeId>(kAddTextLayerTextNodeOutput);
    const auto layerOutputNodeId = result.outputId<NodeId>(kAddTextLayerLayerOutputNodeOutput);
    const auto textParameterId = result.outputId<ParameterId>(kAddTextLayerTextParameterOutput);
    const auto positionParameterId =
        result.outputId<ParameterId>(kAddTextLayerPositionParameterOutput);
    const auto opacityParameterId =
        result.outputId<ParameterId>(kAddTextLayerOpacityParameterOutput);
    test.expect(layerId && slotId && textNodeId && layerOutputNodeId && textParameterId &&
                    positionParameterId && opacityParameterId,
                "AddTextLayer should return every typed object identity");
    if (!layerId || !slotId || !textNodeId || !layerOutputNodeId || !textParameterId ||
        !positionParameterId || !opacityParameterId) {
        return;
    }

    const auto added = document.snapshot();
    const auto& addedComposition = composition(added);
    test.expect(addedComposition.graph().nodes().size() == originalNodeCount + 2 &&
                    addedComposition.graph().edges().size() == originalEdgeCount + 2 &&
                    addedComposition.parameters().records().size() == originalParameterCount + 3 &&
                    added.project().validate().ok(),
                "AddTextLayer should create one valid source-to-boundary-to-stack topology");
    const auto* textParameter = addedComposition.parameters().find(*textParameterId);
    const auto* textConstant = textParameter == nullptr
                                   ? nullptr
                                   : std::get_if<ConstantValueSource>(&textParameter->source);
    const auto* text =
        textConstant == nullptr ? nullptr : std::get_if<std::string>(&textConstant->value);
    test.expect(text != nullptr && *text == "Hello, Bloom!",
                "created text parameter should preserve its exact text");

    test.expect(stack.undo().changed(), "AddTextLayer should undo as one history entry");
    const auto undone = document.snapshot();
    test.expect(composition(undone).graph().nodes().size() == originalNodeCount &&
                    composition(undone).graph().edges().size() == originalEdgeCount &&
                    composition(undone).parameters().records().size() == originalParameterCount,
                "AddTextLayer undo should remove its complete topology exactly");
    test.expect(stack.redo().changed(), "AddTextLayer should redo as one history entry");
    const auto boundaries = composition(document.snapshot()).graph().layerOutputs();
    test.expect(std::ranges::find_if(boundaries,
                                     [&](const auto& item) {
                                         return item.layerId == *layerId &&
                                                item.nodeId == *layerOutputNodeId;
                                     }) != boundaries.end(),
                "AddTextLayer redo should restore the exact layer identity");
}

void testCompositionFormatCommand(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();
    const auto frameRate = document::FrameRate::create(24'000, 1'001);
    if (!frameRate.has_value()) {
        test.expect(false, "custom frame rate fixture must be valid");
        return;
    }
    const auto format = document::CompositionFormat::create(
        3'840, 2'160, core::PixelAspectRatio::square(), frameRate.value());
    if (!format.has_value()) {
        test.expect(false, "custom format fixture must be valid");
        return;
    }
    const auto& formatValue = format.value();

    Transaction change("Change composition format", original.revision());
    change.emplace<SetCompositionFormat>(kCompositionId, formatValue);
    test.expect(stack.execute(std::move(change)).changed() &&
                    composition(document.snapshot()).format() == formatValue,
                "composition format command should publish exact render settings");
    test.expect(stack.undo().changed() &&
                    composition(document.snapshot()).format() == composition(original).format(),
                "composition format command should undo exactly");
    test.expect(stack.redo().changed() && composition(document.snapshot()).format() == formatValue,
                "composition format command should redo exactly");
}

void testAddSolidLayerBuildsOneCanonicalTopology(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();
    constexpr core::Color4d color{-0.25, 1.5, 0.25, 0.8};

    Transaction add("Add solid layer", original.revision());
    add.emplace<AddSolidLayer>(kCompositionId, "Plate", color, Vec2d{320.0, 180.0}, 0.75);
    const auto result = stack.execute(std::move(add));
    const auto ids = solidOutputIds(result);
    if (!ids) {
        test.fail("AddSolidLayer should expose all nine typed durable IDs");
        return;
    }
    test.expect(result.changed() && result.outputs.size() == 9 &&
                    document.snapshot().project().validate().ok(),
                "AddSolidLayer should publish one valid topology and every durable ID");
    expectSolidState(test, document.snapshot(), *ids, "Plate", color, Vec2d{320.0, 180.0}, 0.75);

    test.expect(stack.undo().changed() &&
                    hasSameTruth(composition(document.snapshot()), composition(original)),
                "AddSolidLayer undo should restore exact prior composition truth");
    test.expect(stack.redo().changed(), "AddSolidLayer should redo as one history entry");
    expectSolidState(test, document.snapshot(), *ids, "Plate", color, Vec2d{320.0, 180.0}, 0.75);
}

void testPublishedSolidBranchIdsAreNeverReused(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);

    Transaction firstAdd("First solid branch", document.snapshot().revision());
    firstAdd.emplace<AddSolidLayer>(kCompositionId, "Abandoned", core::Color4d{}, Vec2d{10.0, 20.0},
                                    1.0);
    const auto firstIds = solidOutputIds(stack.execute(std::move(firstAdd)));
    if (!firstIds) {
        test.fail("first solid branch should return all nine durable IDs");
        return;
    }
    test.expect(stack.undo().changed(), "published solid branch should be undoable");

    Transaction replacementAdd("Replacement solid branch", document.snapshot().revision());
    replacementAdd.emplace<AddSolidLayer>(
        kCompositionId, "Replacement", core::Color4d{0.2, 0.3, 0.4, 1.0}, Vec2d{30.0, 40.0}, 0.5);
    const auto replacementIds = solidOutputIds(stack.execute(std::move(replacementAdd)));
    if (!replacementIds) {
        test.fail("replacement solid branch should return all nine durable IDs");
        return;
    }

    const std::array firstNodes{firstIds->solidNodeId, firstIds->layerOutputNodeId};
    const std::array replacementNodes{replacementIds->solidNodeId,
                                      replacementIds->layerOutputNodeId};
    const std::array firstParameters{firstIds->colorParameterId, firstIds->positionParameterId,
                                     firstIds->opacityParameterId};
    const std::array replacementParameters{replacementIds->colorParameterId,
                                           replacementIds->positionParameterId,
                                           replacementIds->opacityParameterId};
    const std::array firstEdges{firstIds->solidToLayerEdgeId, firstIds->layerToStackEdgeId};
    const std::array replacementEdges{replacementIds->solidToLayerEdgeId,
                                      replacementIds->layerToStackEdgeId};
    test.expect(areDisjoint(firstNodes, replacementNodes) &&
                    areDisjoint(firstParameters, replacementParameters) &&
                    areDisjoint(firstEdges, replacementEdges) &&
                    firstIds->layerId != replacementIds->layerId &&
                    firstIds->slotId != replacementIds->slotId,
                "replacement edits must not reuse IDs from an undone published branch");
    test.expect(!stack.canRedo(), "replacement edit should discard the abandoned redo branch");
}

void testAddSolidLayerRejectsInvalidInputs(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto revision = document.snapshot().revision();
    const auto rejects = [&](std::string name, const core::Color4d color, const Vec2d position,
                             const double opacityValue,
                             const CompositionId compositionId = kCompositionId) {
        Transaction transaction("Reject invalid solid", revision);
        transaction.emplace<AddSolidLayer>(compositionId, std::move(name), color, position,
                                           opacityValue);
        return stack.execute(std::move(transaction)).status == CommandStatus::Rejected;
    };

    test.expect(rejects("", {}, {}, 1.0), "solid layer should reject an empty name");
    test.expect(rejects("Color", {0.0, 0.0, 0.0, 1.1}, {}, 1.0),
                "solid layer should reject alpha outside the unit interval");
    test.expect(rejects("Position", {}, {std::numeric_limits<double>::infinity(), 0.0}, 1.0),
                "solid layer should reject a non-finite position");
    test.expect(rejects("Opacity", {}, {}, -0.1),
                "solid layer should reject opacity outside the unit interval");
    test.expect(rejects("Missing", {}, {}, 1.0, CompositionId::fromRaw(9999)),
                "solid layer should reject a missing composition");
    test.expect(document.snapshot().revision() == revision && stack.size() == 0,
                "rejected solid inputs should preserve revision, graph, IDs, and history");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testAddTextLayerBuildsOneCanonicalTopology(test);
        bloom::commands::test::testCompositionFormatCommand(test);
        bloom::commands::test::testAddSolidLayerBuildsOneCanonicalTopology(test);
        bloom::commands::test::testPublishedSolidBranchIdsAreNeverReused(test);
        bloom::commands::test::testAddSolidLayerRejectsInvalidInputs(test);
    } catch (const std::exception& error) {
        test.fail(std::string("unexpected test exception: ") + error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
