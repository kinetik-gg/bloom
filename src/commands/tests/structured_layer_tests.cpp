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
    ParameterId anchorParameterId;
    ParameterId scaleParameterId;
    ParameterId rotationParameterId;
    ParameterId opacityParameterId;
    ParameterId blendModeParameterId;
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
    const auto anchorParameterId =
        result.outputId<ParameterId>(kAddSolidLayerAnchorParameterOutput);
    const auto scaleParameterId = result.outputId<ParameterId>(kAddSolidLayerScaleParameterOutput);
    const auto rotationParameterId =
        result.outputId<ParameterId>(kAddSolidLayerRotationParameterOutput);
    const auto opacityParameterId =
        result.outputId<ParameterId>(kAddSolidLayerOpacityParameterOutput);
    const auto blendModeParameterId =
        result.outputId<ParameterId>(kAddSolidLayerBlendModeParameterOutput);
    const auto solidToLayerEdgeId = result.outputId<EdgeId>(kAddSolidLayerSolidToLayerEdgeOutput);
    const auto layerToStackEdgeId = result.outputId<EdgeId>(kAddSolidLayerLayerToStackEdgeOutput);
    if (!layerId || !slotId || !solidNodeId || !layerOutputNodeId || !colorParameterId ||
        !positionParameterId || !anchorParameterId || !scaleParameterId || !rotationParameterId ||
        !opacityParameterId || !blendModeParameterId || !solidToLayerEdgeId ||
        !layerToStackEdgeId) {
        return std::nullopt;
    }
    return SolidOutputIds{*layerId,
                          *slotId,
                          *solidNodeId,
                          *layerOutputNodeId,
                          *colorParameterId,
                          *positionParameterId,
                          *anchorParameterId,
                          *scaleParameterId,
                          *rotationParameterId,
                          *opacityParameterId,
                          *blendModeParameterId,
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
            {std::string(document::kAnchorParameterRole), ids.anchorParameterId},
            {std::string(document::kScaleParameterRole), ids.scaleParameterId},
            {std::string(document::kRotationParameterRole), ids.rotationParameterId},
            {std::string(document::kOpacityParameterRole), ids.opacityParameterId},
            {std::string(document::kBlendModeParameterRole), ids.blendModeParameterId},
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
    // A newly created layer always starts at the identity transform and Normal blending: the
    // command takes no anchor, scale, rotation, or blend-mode argument, so these four are pinned to
    // their schema defaults rather than to anything the caller passed.
    const ParameterRecord expectedAnchorParameter{
        ids.anchorParameterId,
        std::string(document::kAnchorParameterSchemaKey),
        ConstantValueSource{document::kDefaultAnchor},
    };
    const ParameterRecord expectedScaleParameter{
        ids.scaleParameterId,
        std::string(document::kScaleParameterSchemaKey),
        ConstantValueSource{document::kDefaultScale},
    };
    const ParameterRecord expectedRotationParameter{
        ids.rotationParameterId,
        std::string(document::kRotationParameterSchemaKey),
        ConstantValueSource{document::kDefaultRotationDegrees},
    };
    const ParameterRecord expectedBlendModeParameter{
        ids.blendModeParameterId,
        std::string(document::kBlendModeParameterSchemaKey),
        ConstantValueSource{document::kDefaultBlendModeValue},
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
    const auto* anchorParameter = value.parameters().find(ids.anchorParameterId);
    const auto* scaleParameter = value.parameters().find(ids.scaleParameterId);
    const auto* rotationParameter = value.parameters().find(ids.rotationParameterId);
    const auto* blendModeParameter = value.parameters().find(ids.blendModeParameterId);
    test.expect(anchorParameter != nullptr && *anchorParameter == expectedAnchorParameter &&
                    scaleParameter != nullptr && *scaleParameter == expectedScaleParameter &&
                    rotationParameter != nullptr &&
                    *rotationParameter == expectedRotationParameter &&
                    blendModeParameter != nullptr &&
                    *blendModeParameter == expectedBlendModeParameter,
                "a new solid layer starts at the identity transform and Normal blending");
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

// ADAPTED (task S3): this was testAddTextLayerRefusesUntilRenderable, which pinned the
// Unsupported refusal AddTextLayer returned while no portable CPU text renderer existed. Text
// layers now build the same canonical structured-layer topology a solid does, so what is pinned
// here is that topology, its parameter schemas and values, and that the whole thing is one undoable
// history entry. The atomicity proof the old case carried (a rejected operation publishes nothing,
// not even IDs) moves to testAddTextLayerRejectsInvalidInputs() below, which still has real
// refusals to exercise.
void testAddTextLayerBuildsOneCanonicalTopology(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto before = document.snapshot();
    const auto color = core::Color4d{0.9, 0.8, 0.7, 1.0};
    Transaction add("Add text layer", before.revision());
    add.emplace<AddTextLayer>(kCompositionId, "Title", "Hello, Bloom!", Vec2d{960, 540}, 0.75, 48.0,
                              color);
    const auto result = stack.execute(std::move(add));
    test.expect(result.changed(), "text layer creation applies");

    const auto layerId = result.outputId<LayerId>(kAddTextLayerLayerOutput);
    const auto slotId = result.outputId<LayerSlotId>(kAddTextLayerSlotOutput);
    const auto textNodeId = result.outputId<NodeId>(kAddTextLayerTextNodeOutput);
    const auto layerOutputNodeId = result.outputId<NodeId>(kAddTextLayerLayerOutputNodeOutput);
    const auto contentParameterId = result.outputId<ParameterId>(kAddTextLayerTextParameterOutput);
    const auto sizeParameterId = result.outputId<ParameterId>(kAddTextLayerSizeParameterOutput);
    const auto colorParameterId = result.outputId<ParameterId>(kAddTextLayerColorParameterOutput);
    const auto positionParameterId =
        result.outputId<ParameterId>(kAddTextLayerPositionParameterOutput);
    const auto anchorParameterId = result.outputId<ParameterId>(kAddTextLayerAnchorParameterOutput);
    const auto scaleParameterId = result.outputId<ParameterId>(kAddTextLayerScaleParameterOutput);
    const auto rotationParameterId =
        result.outputId<ParameterId>(kAddTextLayerRotationParameterOutput);
    const auto opacityParameterId =
        result.outputId<ParameterId>(kAddTextLayerOpacityParameterOutput);
    const auto blendModeParameterId =
        result.outputId<ParameterId>(kAddTextLayerBlendModeParameterOutput);
    const auto textToLayerEdgeId = result.outputId<EdgeId>(kAddTextLayerTextToLayerEdgeOutput);
    const auto layerToStackEdgeId = result.outputId<EdgeId>(kAddTextLayerLayerToStackEdgeOutput);
    if (!layerId || !slotId || !textNodeId || !layerOutputNodeId || !contentParameterId ||
        !sizeParameterId || !colorParameterId || !positionParameterId || !anchorParameterId ||
        !scaleParameterId || !rotationParameterId || !opacityParameterId ||
        !blendModeParameterId || !textToLayerEdgeId || !layerToStackEdgeId) {
        test.fail("text branch should return all fifteen durable IDs");
        return;
    }
    const std::array textParameters{*contentParameterId,  *sizeParameterId,
                                    *colorParameterId,    *positionParameterId,
                                    *anchorParameterId,   *scaleParameterId,
                                    *rotationParameterId, *opacityParameterId,
                                    *blendModeParameterId};
    test.expect(std::ranges::adjacent_find(textParameters) == textParameters.end(),
                "every parameter in a text branch has its own identity");

    const auto& value = composition(document.snapshot());
    const NodeRecord expectedTextNode{
        *textNodeId,
        std::string(document::kTextSourceNodeType),
        {
            {std::string(document::kTextParameterRole), *contentParameterId},
            {std::string(document::kTextSizeParameterRole), *sizeParameterId},
            {std::string(document::kTextColorParameterRole), *colorParameterId},
        },
        document::kTextSourceNodeSchemaVersion,
    };
    const auto* textNode = value.graph().findNode(*textNodeId);
    test.expect(textNode != nullptr && *textNode == expectedTextNode,
                "text source should bind content, size, and color in the registered order");

    const ParameterRecord expectedContent{*contentParameterId,
                                          std::string(document::kTextParameterSchemaKey),
                                          ConstantValueSource{std::string("Hello, Bloom!")}};
    const ParameterRecord expectedSize{*sizeParameterId,
                                       std::string(document::kTextSizeParameterSchemaKey),
                                       ConstantValueSource{48.0}};
    const ParameterRecord expectedColor{*colorParameterId,
                                        std::string(document::kTextColorParameterSchemaKey),
                                        ConstantValueSource{color}};
    const auto* content = value.parameters().find(*contentParameterId);
    const auto* size = value.parameters().find(*sizeParameterId);
    const auto* storedColor = value.parameters().find(*colorParameterId);
    test.expect(content != nullptr && *content == expectedContent,
                "text content parameter should preserve exact schema and UTF-8 value");
    test.expect(size != nullptr && *size == expectedSize,
                "text size parameter should preserve exact schema and em pixel value");
    test.expect(storedColor != nullptr && *storedColor == expectedColor,
                "text color parameter should preserve exact schema and straight authoring value");

    const LayerOutputBoundary expectedBoundary{*layerOutputNodeId, *layerId, "Title",
                                               std::string(document::kLayerOutputOutputPort)};
    test.expect(std::ranges::find(value.graph().layerOutputs(), expectedBoundary) !=
                    value.graph().layerOutputs().end(),
                "text layer boundary should preserve exact name, port, and stable IDs");
    const EdgeRecord expectedTextToLayerEdge{
        *textToLayerEdgeId,
        {*textNodeId, std::string(document::kTextSourceOutputPort)},
        NodeInputRef{*layerOutputNodeId, std::string(document::kLayerOutputContentInputPort)},
    };
    test.expect(std::ranges::find(value.graph().edges(), expectedTextToLayerEdge) !=
                    value.graph().edges().end(),
                "text source edge should reach the Layer Output content port");
    test.expect(value.graph().layerStack().find(*slotId) != nullptr,
                "text layer should occupy its own stable stack slot");
    test.expect(value.parameters().find(*positionParameterId) != nullptr &&
                    value.parameters().find(*anchorParameterId) != nullptr &&
                    value.parameters().find(*scaleParameterId) != nullptr &&
                    value.parameters().find(*rotationParameterId) != nullptr &&
                    value.parameters().find(*opacityParameterId) != nullptr &&
                    value.parameters().find(*blendModeParameterId) != nullptr,
                "a text layer owns the same six Layer Output parameters a solid does");

    test.expect(stack.size() == 1 && stack.canUndo(),
                "text layer creation is exactly one history entry");
    test.expect(stack.undo().changed() &&
                    hasSameTruth(composition(document.snapshot()), composition(before)),
                "AddTextLayer undo should restore exact prior composition truth");
    test.expect(document.snapshot()
                        .project()
                        .findComposition(kCompositionId)
                        ->graph()
                        .findNode(*textNodeId) == nullptr,
                "and leave no text node behind");
    test.expect(stack.redo().changed() && document.snapshot()
                                                  .project()
                                                  .findComposition(kCompositionId)
                                                  ->graph()
                                                  .findNode(*textNodeId) != nullptr,
                "AddTextLayer should redo as one history entry");
}

void testAddTextLayerRejectsInvalidInputs(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto before = document.snapshot();
    const auto rejects = [&](std::string name, std::string content, const Vec2d position,
                             const double opacityValue, const double size,
                             const core::Color4d color = core::Color4d{1.0, 1.0, 1.0, 1.0}) {
        Transaction transaction("Reject invalid text", before.revision());
        // A second operation in the same transaction proves the refusal is atomic across the whole
        // transaction, which is what the old refusal case proved with SetProjectName.
        transaction.emplace<SetProjectName>("Must not publish");
        transaction.emplace<AddTextLayer>(kCompositionId, std::move(name), std::move(content),
                                          position, opacityValue, size, color);
        return stack.execute(std::move(transaction)).status == CommandStatus::Rejected;
    };

    test.expect(rejects("", "Body", {}, 1.0, 72.0), "text layer should reject an empty name");
    test.expect(rejects("Size", "Body", {}, 1.0, 0.0),
                "text layer should reject a size of zero pixels");
    test.expect(rejects("Size", "Body", {}, 1.0, document::kMaximumTextSizePixels + 1.0),
                "text layer should reject a size past the schema maximum");
    test.expect(rejects("Size", "Body", {}, 1.0, std::numeric_limits<double>::infinity()),
                "text layer should reject a non-finite size");
    test.expect(rejects("Color", "Body", {}, 1.0, 72.0, {0.0, 0.0, 0.0, 1.5}),
                "text layer should reject color alpha outside the unit interval");
    test.expect(rejects("Content", std::string("\xff\xfe"), {}, 1.0, 72.0),
                "text layer should reject content that is not valid UTF-8");
    test.expect(
        rejects("Position", "Body", {std::numeric_limits<double>::infinity(), 0.0}, 1.0, 72.0),
        "text layer should reject a non-finite position");
    test.expect(rejects("Opacity", "Body", {}, -0.1, 72.0),
                "text layer should reject opacity outside the unit interval");

    const auto after = document.snapshot();
    test.expect(after.revision() == before.revision() &&
                    after.project().name() == before.project().name() &&
                    after.ids().highWater() == before.ids().highWater() &&
                    hasSameTruth(composition(after), composition(before)) && stack.size() == 0 &&
                    !stack.canUndo() && !stack.canRedo(),
                "every text refusal is atomic including IDs, names, and history");

    // Empty content is deliberately NOT a refusal: an artist adds a text layer and then types into
    // it, so a layer with nothing typed yet has to be a real, selectable, editable layer.
    Transaction empty("Add empty text layer", before.revision());
    empty.emplace<AddTextLayer>(kCompositionId, "Untyped", "", Vec2d{0.0, 0.0});
    test.expect(stack.execute(std::move(empty)).changed(),
                "a text layer with no content yet is accepted");
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
        test.fail("AddSolidLayer should expose all thirteen typed durable IDs");
        return;
    }
    test.expect(result.changed() && result.outputs.size() == 13 &&
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
        test.fail("first solid branch should return all thirteen durable IDs");
        return;
    }
    test.expect(stack.undo().changed(), "published solid branch should be undoable");

    Transaction replacementAdd("Replacement solid branch", document.snapshot().revision());
    replacementAdd.emplace<AddSolidLayer>(
        kCompositionId, "Replacement", core::Color4d{0.2, 0.3, 0.4, 1.0}, Vec2d{30.0, 40.0}, 0.5);
    const auto replacementIds = solidOutputIds(stack.execute(std::move(replacementAdd)));
    if (!replacementIds) {
        test.fail("replacement solid branch should return all thirteen durable IDs");
        return;
    }

    const std::array firstNodes{firstIds->solidNodeId, firstIds->layerOutputNodeId};
    const std::array replacementNodes{replacementIds->solidNodeId,
                                      replacementIds->layerOutputNodeId};
    const std::array firstParameters{firstIds->colorParameterId,    firstIds->positionParameterId,
                                     firstIds->anchorParameterId,   firstIds->scaleParameterId,
                                     firstIds->rotationParameterId, firstIds->opacityParameterId};
    const std::array replacementParameters{
        replacementIds->colorParameterId,    replacementIds->positionParameterId,
        replacementIds->anchorParameterId,   replacementIds->scaleParameterId,
        replacementIds->rotationParameterId, replacementIds->opacityParameterId};
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
        bloom::commands::test::testAddTextLayerRejectsInvalidInputs(test);
        bloom::commands::test::testCompositionFormatCommand(test);
        bloom::commands::test::testAddSolidLayerBuildsOneCanonicalTopology(test);
        bloom::commands::test::testPublishedSolidBranchIdsAreNeverReused(test);
        bloom::commands::test::testAddSolidLayerRejectsInvalidInputs(test);
    } catch (const std::exception& error) {
        test.fail(std::string("unexpected test exception: ") + error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
