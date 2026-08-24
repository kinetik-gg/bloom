#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/result.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/layer_stack.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bloom::commands::AddTextLayer;
using bloom::commands::CommandStack;
using bloom::commands::CommandStatus;
using bloom::commands::MoveLayerBefore;
using bloom::commands::Operation;
using bloom::commands::OperationResult;
using bloom::commands::SetCompositionName;
using bloom::commands::SetParameterSource;
using bloom::commands::SetProjectName;
using bloom::commands::Transaction;
using bloom::document::CanonicalGraph;
using bloom::document::Composition;
using bloom::document::CompositionId;
using bloom::document::ConstantValueSource;
using bloom::document::Document;
using bloom::document::EdgeId;
using bloom::document::LayerId;
using bloom::document::LayerSlotId;
using bloom::document::LayerStackInputRef;
using bloom::document::NodeId;
using bloom::document::ParameterId;
using bloom::document::Project;
using bloom::document::ProjectId;
using bloom::document::Vec2d;

constexpr ProjectId kProjectId = ProjectId::fromRaw(1);
constexpr CompositionId kCompositionId = CompositionId::fromRaw(10);
constexpr NodeId kLayerStackNodeId = NodeId::fromRaw(20);
constexpr NodeId kFirstLayerNodeId = NodeId::fromRaw(21);
constexpr NodeId kSecondLayerNodeId = NodeId::fromRaw(22);
constexpr LayerId kFirstLayerId = LayerId::fromRaw(30);
constexpr LayerId kSecondLayerId = LayerId::fromRaw(31);
constexpr LayerSlotId kFirstSlotId = LayerSlotId::fromRaw(40);
constexpr LayerSlotId kSecondSlotId = LayerSlotId::fromRaw(41);
constexpr EdgeId kFirstEdgeId = EdgeId::fromRaw(50);
constexpr EdgeId kSecondEdgeId = EdgeId::fromRaw(51);
constexpr ParameterId kOpacityId = ParameterId::fromRaw(60);

class TestContext final {
  public:
    void expect(bool condition, std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    void fail(std::string_view message,
              const std::source_location location = std::source_location::current()) {
        expect(false, message, location);
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

void requireFixture(bool condition, std::string_view message) {
    if (!condition) {
        throw std::logic_error(std::string(message));
    }
}

Project makeProject() {
    CanonicalGraph graph(kLayerStackNodeId);
    requireFixture(
        graph.addNode({kLayerStackNodeId, std::string(bloom::document::kLayerStackNodeType), {}}),
        "fixture Layer Stack node must be accepted");
    requireFixture(
        graph.addNode({kFirstLayerNodeId, std::string(bloom::document::kLayerOutputNodeType), {}}),
        "fixture first Layer Output node must be accepted");
    requireFixture(
        graph.addNode({kSecondLayerNodeId, std::string(bloom::document::kLayerOutputNodeType), {}}),
        "fixture second Layer Output node must be accepted");
    requireFixture(graph.addLayerOutput({kFirstLayerNodeId, kFirstLayerId, "First", "image"}),
                   "fixture first Layer Output boundary must be accepted");
    requireFixture(graph.addLayerOutput({kSecondLayerNodeId, kSecondLayerId, "Second", "image"}),
                   "fixture second Layer Output boundary must be accepted");
    requireFixture(graph.layerStack().append({kFirstSlotId, kFirstLayerId}),
                   "fixture first layer slot must be accepted");
    requireFixture(graph.layerStack().append({kSecondSlotId, kSecondLayerId}),
                   "fixture second layer slot must be accepted");
    requireFixture(
        graph.addEdge({
            kFirstEdgeId,
            {kFirstLayerNodeId, "image"},
            LayerStackInputRef{kLayerStackNodeId, kFirstSlotId,
                               std::string(bloom::document::kLayerStackContentInputRole)},
        }),
        "fixture first stack edge must be accepted");
    requireFixture(
        graph.addEdge({
            kSecondEdgeId,
            {kSecondLayerNodeId, "image"},
            LayerStackInputRef{kLayerStackNodeId, kSecondSlotId,
                               std::string(bloom::document::kLayerStackContentInputRole)},
        }),
        "fixture second stack edge must be accepted");
    graph.setCompositionOutput({kLayerStackNodeId, "image"});

    Composition composition(kCompositionId, "Main", bloom::core::RationalTime::fromInteger(10),
                            std::move(graph));
    requireFixture(
        composition.parameters().insert({kOpacityId, "opacity", ConstantValueSource{1.0}}),
        "fixture opacity parameter must be accepted");

    Project project(kProjectId, "Original Project");
    requireFixture(project.addComposition(std::move(composition)),
                   "fixture composition must be accepted");
    requireFixture(project.validate().ok(), "fixture project must validate");
    return project;
}

const Composition& composition(const bloom::document::Snapshot& snapshot) {
    const auto* result = snapshot.project().findComposition(kCompositionId);
    requireFixture(result != nullptr, "fixture composition must remain addressable");
    return *result;
}

double opacity(const bloom::document::Snapshot& snapshot) {
    const auto* parameter = composition(snapshot).parameters().find(kOpacityId);
    requireFixture(parameter != nullptr, "fixture opacity must remain addressable");
    const auto* constant = std::get_if<ConstantValueSource>(&parameter->source);
    requireFixture(constant != nullptr, "fixture opacity must remain constant");
    const auto* value = std::get_if<double>(&constant->value);
    requireFixture(value != nullptr, "fixture opacity must remain a double");
    return *value;
}

std::vector<LayerSlotId> layerOrder(const bloom::document::Snapshot& snapshot) {
    std::vector<LayerSlotId> result;
    for (const auto& entry : composition(snapshot).graph().layerStack().entries()) {
        result.push_back(entry.slotId);
    }
    return result;
}

class BreakProject final : public Operation {
  public:
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.test.break-project";
    }

    [[nodiscard]] OperationResult apply(bloom::document::Draft& draft) const override {
        auto& graph = draft.project().findComposition(kCompositionId)->graph();
        graph.setCompositionOutput({NodeId::fromRaw(9999), "image"});
        return OperationResult::applied();
    }
};

void testAtomicTransactionUndoAndRedo(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();

    Transaction transaction("Rename and fade", original.revision());
    transaction.emplace<SetProjectName>("Renamed Project");
    transaction.emplace<SetParameterSource>(
        kCompositionId, kOpacityId, bloom::document::ParameterSource{ConstantValueSource{0.5}});
    const auto execute = stack.execute(std::move(transaction));
    test.expect(execute.status == CommandStatus::Succeeded,
                "two-operation transaction should commit");
    test.expect(stack.size() == 1, "transaction should create exactly one history entry");
    test.expect(stack.canUndo(), "committed transaction should be undoable");
    test.expect(!stack.canRedo(), "new transaction should not create redo history");

    const auto changed = document.snapshot();
    test.expect(changed.revision() > original.revision(),
                "execute should advance the document revision");
    test.expect(changed.project().name() == "Renamed Project",
                "execute should publish the new project name");
    test.expect(std::abs(opacity(changed) - 0.5) < 0.0001,
                "execute should publish the new opacity");
    test.expect(original.project().name() == "Original Project",
                "original snapshot should remain immutable after execute");
    test.expect(std::abs(opacity(original) - 1.0) < 0.0001,
                "original snapshot should retain its exact parameter source");

    const auto undo = stack.undo();
    const auto restored = document.snapshot();
    test.expect(undo.status == CommandStatus::Succeeded, "undo should restore history");
    test.expect(restored.revision() > changed.revision(),
                "undo should publish at a new monotonic revision");
    test.expect(restored.project().name() == original.project().name(),
                "undo should restore the exact project name");
    test.expect(std::abs(opacity(restored) - opacity(original)) < 0.0001,
                "undo should restore the exact parameter source");
    test.expect(layerOrder(restored) == layerOrder(original),
                "undo should preserve exact stable layer order and IDs");
    test.expect(stack.canRedo(), "successful undo should make redo available");

    const auto redo = stack.redo();
    const auto redone = document.snapshot();
    test.expect(redo.status == CommandStatus::Succeeded, "redo should restore history");
    test.expect(redone.revision() > restored.revision(),
                "redo should publish at a new monotonic revision");
    test.expect(redone.project().name() == changed.project().name(),
                "redo should restore the exact changed project name");
    test.expect(std::abs(opacity(redone) - opacity(changed)) < 0.0001,
                "redo should restore the exact changed parameter source");
    test.expect(layerOrder(redone) == layerOrder(changed),
                "redo should preserve exact stable layer order and IDs");
}

void testRejectedAndInvalidTransactionsAreAtomic(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();

    Transaction rejected("Rejected", original.revision());
    rejected.emplace<SetProjectName>("Must Not Publish");
    rejected.emplace<SetCompositionName>(CompositionId::fromRaw(9999), "Missing");
    const auto rejectedResult = stack.execute(std::move(rejected));
    const auto afterRejected = document.snapshot();
    test.expect(rejectedResult.status == CommandStatus::Rejected,
                "invalid operation target should reject the whole transaction");
    test.expect(rejectedResult.operationFailures.size() == 1,
                "rejected transaction should identify one failing operation");
    if (!rejectedResult.operationFailures.empty()) {
        test.expect(rejectedResult.operationFailures.front().operationIndex == 1,
                    "failure should identify the second operation");
    }
    test.expect(afterRejected.revision() == original.revision(),
                "rejected transaction should not publish a revision");
    test.expect(afterRejected.project().name() == original.project().name(),
                "earlier draft mutations should not leak from a rejected transaction");
    test.expect(stack.size() == 0, "rejected transaction should not create history");

    Transaction invalid("Invalid draft", original.revision());
    invalid.emplace<SetProjectName>("Must Also Not Publish");
    invalid.emplace<BreakProject>();
    const auto invalidResult = stack.execute(std::move(invalid));
    const auto afterInvalid = document.snapshot();
    test.expect(invalidResult.status == CommandStatus::ValidationFailed,
                "invalid final draft should fail document validation");
    test.expect(!invalidResult.validation.ok(),
                "validation failure should carry structured document diagnostics");
    test.expect(afterInvalid.revision() == original.revision(),
                "invalid final draft should not publish a revision");
    test.expect(afterInvalid.project().name() == original.project().name(),
                "invalid final draft should preserve exact prior state");
    test.expect(stack.size() == 0, "invalid final draft should not create history");
}

void testStaleExecuteUndoAndRedoAreNoOps(TestContext& test) {
    {
        Document document(makeProject());
        CommandStack stack(document);
        const auto stale = document.snapshot();
        auto externalDraft = document.draft(stale);
        externalDraft.project().setName("External");
        test.expect(document.commit(stale.revision(), std::move(externalDraft)).committed(),
                    "stale-execute setup commit should succeed");

        Transaction transaction("Stale execute", stale.revision());
        transaction.emplace<SetProjectName>("Must Not Publish");
        const auto result = stack.execute(std::move(transaction));
        const auto after = document.snapshot();
        test.expect(result.status == CommandStatus::StaleRevision,
                    "execute should reject an obsolete base revision");
        test.expect(result.expectedRevision == stale.revision(),
                    "stale execute should report the expected revision");
        test.expect(after.project().name() == "External",
                    "stale execute should preserve newer document state");
        test.expect(stack.size() == 0, "stale execute should not create history");
    }

    {
        Document document(makeProject());
        CommandStack stack(document);
        Transaction transaction("Tracked edit", document.snapshot().revision());
        transaction.emplace<SetProjectName>("Tracked");
        test.expect(stack.execute(std::move(transaction)).changed(),
                    "stale-undo setup command should commit");

        const auto beforeExternal = document.snapshot();
        auto externalDraft = document.draft(beforeExternal);
        externalDraft.project().findComposition(kCompositionId)->setName("External");
        test.expect(
            document.commit(beforeExternal.revision(), std::move(externalDraft)).committed(),
            "stale-undo setup external commit should succeed");
        const auto external = document.snapshot();

        const auto result = stack.undo();
        const auto after = document.snapshot();
        test.expect(result.status == CommandStatus::StaleRevision,
                    "undo should reject history after an external commit");
        test.expect(stack.canUndo(), "stale undo should retain its history cursor");
        test.expect(after.revision() == external.revision(),
                    "stale undo should not publish a revision");
        test.expect(after.project().name() == external.project().name(),
                    "stale undo should preserve the external project state");
        test.expect(composition(after).name() == "External",
                    "stale undo should preserve the external composition state");
    }

    {
        Document document(makeProject());
        CommandStack stack(document);
        Transaction transaction("Tracked edit", document.snapshot().revision());
        transaction.emplace<SetProjectName>("Tracked");
        test.expect(stack.execute(std::move(transaction)).changed(),
                    "stale-redo setup command should commit");
        test.expect(stack.undo().changed(), "stale-redo setup undo should commit");

        const auto beforeExternal = document.snapshot();
        auto externalDraft = document.draft(beforeExternal);
        externalDraft.project().findComposition(kCompositionId)->setName("External");
        test.expect(
            document.commit(beforeExternal.revision(), std::move(externalDraft)).committed(),
            "stale-redo setup external commit should succeed");
        const auto external = document.snapshot();

        const auto result = stack.redo();
        const auto after = document.snapshot();
        test.expect(result.status == CommandStatus::StaleRevision,
                    "redo should reject history after an external commit");
        test.expect(stack.canRedo(), "stale redo should retain its history cursor");
        test.expect(after.revision() == external.revision(),
                    "stale redo should not publish a revision");
        test.expect(after.project().name() == external.project().name(),
                    "stale redo should preserve the external project state");
        test.expect(composition(after).name() == "External",
                    "stale redo should preserve the external composition state");
    }
}

void testLayerOrderNoChangeAndRedoInvalidation(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();

    Transaction move("Move second layer", original.revision());
    move.emplace<MoveLayerBefore>(kCompositionId, kSecondSlotId, kFirstSlotId);
    test.expect(stack.execute(std::move(move)).changed(), "layer move should commit");
    const auto moved = document.snapshot();
    test.expect(layerOrder(moved) == std::vector<LayerSlotId>{kSecondSlotId, kFirstSlotId},
                "layer move should change only stable slot order");

    test.expect(stack.undo().changed(), "layer move should be undoable");
    test.expect(layerOrder(document.snapshot()) == layerOrder(original),
                "undo should restore exact stable slot order");
    test.expect(stack.canRedo(), "undo should make the layer move redoable");

    Transaction replacement("Replace redo", document.snapshot().revision());
    replacement.emplace<SetProjectName>("Replacement");
    test.expect(stack.execute(std::move(replacement)).changed(),
                "new edit after undo should commit");
    test.expect(!stack.canRedo(), "new edit after undo should invalidate redo history");
    test.expect(stack.size() == 1, "invalidated redo branch should be discarded");

    const auto beforeNoChange = document.snapshot();
    Transaction noChange("No change", beforeNoChange.revision());
    noChange.emplace<SetProjectName>("Replacement");
    const auto noChangeResult = stack.execute(std::move(noChange));
    const auto afterNoChange = document.snapshot();
    test.expect(noChangeResult.status == CommandStatus::NoChange,
                "idempotent operation should produce a no-change result");
    test.expect(afterNoChange.revision() == beforeNoChange.revision(),
                "no-change transaction should not advance revision");
    test.expect(stack.size() == 1, "no-change transaction should not create history");
}

void testAddTextLayerBuildsOneCanonicalTopology(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();
    const auto& originalComposition = composition(original);
    const auto originalNodeCount = originalComposition.graph().nodes().size();
    const auto originalEdgeCount = originalComposition.graph().edges().size();
    const auto originalLayerCount = originalComposition.graph().layerOutputs().size();
    const auto originalSlotCount = originalComposition.graph().layerStack().entries().size();
    const auto originalParameterCount = originalComposition.parameters().records().size();

    Transaction add("Add text layer", original.revision());
    add.emplace<AddTextLayer>(kCompositionId, "Title", "Hello, Bloom!", Vec2d{960.0, 540.0}, 0.75);
    const auto result = stack.execute(std::move(add));
    test.expect(result.status == CommandStatus::Succeeded,
                "AddTextLayer should commit one atomic topology edit");
    test.expect(result.outputs.size() == 9,
                "AddTextLayer should report every allocated durable ID");

    const auto layerId = result.outputId<LayerId>(bloom::commands::kAddTextLayerLayerOutput);
    const auto slotId = result.outputId<LayerSlotId>(bloom::commands::kAddTextLayerSlotOutput);
    const auto textNodeId = result.outputId<NodeId>(bloom::commands::kAddTextLayerTextNodeOutput);
    const auto layerOutputNodeId =
        result.outputId<NodeId>(bloom::commands::kAddTextLayerLayerOutputNodeOutput);
    const auto textParameterId =
        result.outputId<ParameterId>(bloom::commands::kAddTextLayerTextParameterOutput);
    const auto positionParameterId =
        result.outputId<ParameterId>(bloom::commands::kAddTextLayerPositionParameterOutput);
    const auto opacityParameterId =
        result.outputId<ParameterId>(bloom::commands::kAddTextLayerOpacityParameterOutput);
    test.expect(layerId.has_value(), "AddTextLayer should return its LayerId");
    test.expect(slotId.has_value(), "AddTextLayer should return its stable LayerSlotId");
    test.expect(textNodeId.has_value(), "AddTextLayer should return its text source NodeId");
    test.expect(layerOutputNodeId.has_value(),
                "AddTextLayer should return its Layer Output NodeId");
    test.expect(textParameterId.has_value(), "AddTextLayer should return its text ParameterId");
    test.expect(positionParameterId.has_value(),
                "AddTextLayer should return its position ParameterId");
    test.expect(opacityParameterId.has_value(),
                "AddTextLayer should return its opacity ParameterId");

    const auto added = document.snapshot();
    const auto& addedComposition = composition(added);
    test.expect(addedComposition.graph().nodes().size() == originalNodeCount + 2,
                "AddTextLayer should add a text source and one Layer Output node");
    test.expect(addedComposition.graph().edges().size() == originalEdgeCount + 2,
                "AddTextLayer should connect text-to-layer and layer-to-stack");
    test.expect(addedComposition.graph().layerOutputs().size() == originalLayerCount + 1,
                "AddTextLayer should add one durable layer boundary");
    test.expect(addedComposition.graph().layerStack().entries().size() == originalSlotCount + 1,
                "AddTextLayer should append one stable stack slot");
    test.expect(addedComposition.parameters().records().size() == originalParameterCount + 3,
                "AddTextLayer should add text, position, and opacity parameters");
    test.expect(added.project().validate().ok(),
                "AddTextLayer topology should validate as one canonical graph");

    if (layerId.has_value() && layerOutputNodeId.has_value()) {
        const auto boundaries = addedComposition.graph().layerOutputs();
        const auto boundary =
            std::find_if(boundaries.begin(), boundaries.end(),
                         [layerId](const auto& item) { return item.layerId == *layerId; });
        test.expect(boundary != boundaries.end(), "created LayerId should address a boundary");
        if (boundary != boundaries.end()) {
            test.expect(boundary->nodeId == *layerOutputNodeId,
                        "created boundary should own the returned Layer Output node");
            test.expect(boundary->name == "Title", "created boundary should own the layer name");
        }
    }
    if (slotId.has_value() && layerId.has_value()) {
        const auto* entry = addedComposition.graph().layerStack().find(*slotId);
        test.expect(entry != nullptr, "created LayerSlotId should address a stack entry");
        if (entry != nullptr) {
            test.expect(entry->layerId == *layerId,
                        "created stack entry should reference the created LayerId");
        }
    }
    if (textParameterId.has_value()) {
        const auto* parameter = addedComposition.parameters().find(*textParameterId);
        test.expect(parameter != nullptr, "created text ParameterId should address a parameter");
        if (parameter != nullptr) {
            const auto* constant = std::get_if<ConstantValueSource>(&parameter->source);
            const auto* text =
                constant == nullptr ? nullptr : std::get_if<std::string>(&constant->value);
            test.expect(text != nullptr && *text == "Hello, Bloom!",
                        "created text parameter should preserve its source text");
        }
    }
    if (positionParameterId.has_value()) {
        const auto* parameter = addedComposition.parameters().find(*positionParameterId);
        const auto* constant =
            parameter == nullptr ? nullptr : std::get_if<ConstantValueSource>(&parameter->source);
        const auto* position = constant == nullptr ? nullptr : std::get_if<Vec2d>(&constant->value);
        test.expect(position != nullptr && *position == Vec2d{960.0, 540.0},
                    "created position parameter should preserve the requested Vec2d");
    }
    if (opacityParameterId.has_value()) {
        const auto* parameter = addedComposition.parameters().find(*opacityParameterId);
        const auto* constant =
            parameter == nullptr ? nullptr : std::get_if<ConstantValueSource>(&parameter->source);
        const auto* value = constant == nullptr ? nullptr : std::get_if<double>(&constant->value);
        test.expect(value != nullptr && std::abs(*value - 0.75) < 0.0001,
                    "created opacity parameter should preserve the requested value");
    }

    test.expect(stack.undo().changed(), "AddTextLayer should undo as one history entry");
    const auto undone = document.snapshot();
    test.expect(composition(undone).graph().nodes().size() == originalNodeCount,
                "undo should remove both created nodes exactly");
    test.expect(composition(undone).graph().edges().size() == originalEdgeCount,
                "undo should remove both created edges exactly");
    test.expect(composition(undone).parameters().records().size() == originalParameterCount,
                "undo should remove all created parameters exactly");

    test.expect(stack.redo().changed(), "AddTextLayer should redo as one history entry");
    const auto redone = document.snapshot();
    test.expect(redone.revision() > undone.revision(),
                "AddTextLayer redo should use a new monotonic revision");
    if (layerId.has_value()) {
        const auto boundaries = composition(redone).graph().layerOutputs();
        test.expect(std::find_if(boundaries.begin(), boundaries.end(),
                                 [layerId](const auto& item) {
                                     return item.layerId == *layerId;
                                 }) != boundaries.end(),
                    "redo should restore the exact created LayerId");
    }
}

} // namespace

int main() {
    TestContext test;
    try {
        testAtomicTransactionUndoAndRedo(test);
        testRejectedAndInvalidTransactionsAreAtomic(test);
        testStaleExecuteUndoAndRedoAreNoOps(test);
        testLayerOrderNoChangeAndRedoInvalidation(test);
        testAddTextLayerBuildsOneCanonicalTopology(test);
    } catch (const std::exception& error) {
        test.fail(std::string("unexpected test exception: ") + error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
