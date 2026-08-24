#include "command_test_support.hpp"

#include <limits>
#include <optional>

namespace bloom::commands::test {
namespace {

class BreakProject final : public Operation {
  public:
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.test.break-project";
    }

    [[nodiscard]] OperationResult apply(document::Draft& draft) const override {
        auto& graph = draft.project().findComposition(kCompositionId)->graph();
        graph.setCompositionOutput({NodeId::fromRaw(9999), "image"});
        return OperationResult::applied();
    }
};

class ExhaustNodeIds final : public Operation {
  public:
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.test.exhaust-node-ids";
    }

    [[nodiscard]] OperationResult apply(document::Draft& draft) const override {
        draft.ids().reserveExisting(NodeId::fromRaw(std::numeric_limits<std::uint64_t>::max()));
        draft.project().setName("Exhausted");
        return OperationResult::applied();
    }
};

void testAtomicTransactionUndoAndRedo(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();

    Transaction transaction("Rename and fade", original.revision());
    transaction.emplace<SetProjectName>("Renamed Project");
    transaction.emplace<SetParameterSource>(kCompositionId, kOpacityId,
                                            document::ParameterSource{ConstantValueSource{0.5}});
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

void testKnownParameterSchemaRejectionsAreAtomic(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);

    Transaction valid("Set valid opacity", document.snapshot().revision());
    valid.emplace<SetParameterSource>(kCompositionId, kOpacityId,
                                      document::ParameterSource{ConstantValueSource{0.5}});
    test.expect(stack.execute(std::move(valid)).changed(),
                "known-schema rejection fixture should publish one valid edit");
    const auto baseline = document.snapshot();
    const auto baselineHistorySize = stack.size();

    Transaction wrongType("Reject wrong opacity type", baseline.revision());
    wrongType.emplace<SetParameterSource>(
        kCompositionId, kOpacityId,
        document::ParameterSource{ConstantValueSource{std::string("opaque")}});
    const auto wrongTypeResult = stack.execute(std::move(wrongType));
    test.expect(wrongTypeResult.status == CommandStatus::Rejected,
                "opacity command should reject a wrong-type constant");
    test.expect(document.snapshot().revision() == baseline.revision() &&
                    std::abs(opacity(document.snapshot()) - 0.5) < 0.0001 &&
                    stack.size() == baselineHistorySize && stack.canUndo() && !stack.canRedo(),
                "wrong-type rejection should preserve revision, value, and history cursor");

    Transaction wrongRange("Reject out-of-range opacity", baseline.revision());
    wrongRange.emplace<SetParameterSource>(kCompositionId, kOpacityId,
                                           document::ParameterSource{ConstantValueSource{1.5}});
    const auto wrongRangeResult = stack.execute(std::move(wrongRange));
    test.expect(wrongRangeResult.status == CommandStatus::Rejected,
                "opacity command should reject an out-of-range constant");
    test.expect(document.snapshot().revision() == baseline.revision() &&
                    std::abs(opacity(document.snapshot()) - 0.5) < 0.0001 &&
                    stack.size() == baselineHistorySize && stack.canUndo() && !stack.canRedo(),
                "range rejection should preserve revision, value, and history cursor");
    test.expect(stack.undo().changed() && std::abs(opacity(document.snapshot()) - 1.0) < 0.0001,
                "prior valid history should remain exactly undoable after rejections");
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
    test.expect(layerOrder(document.snapshot()) ==
                    std::vector<LayerSlotId>{kSecondSlotId, kFirstSlotId},
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
    test.expect(noChangeResult.status == CommandStatus::NoChange,
                "idempotent operation should produce a no-change result");
    test.expect(document.snapshot().revision() == beforeNoChange.revision(),
                "no-change transaction should not advance revision");
    test.expect(stack.size() == 1, "no-change transaction should not create history");
}

void testExhaustionSurvivesUndoAndRedo(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    Transaction exhaust("Exhaust node IDs", document.snapshot().revision());
    exhaust.emplace<ExhaustNodeIds>();
    test.expect(stack.execute(std::move(exhaust)).changed(),
                "allocator exhaustion fixture should publish");
    test.expect(stack.undo().changed(), "allocator exhaustion should be undoable");
    auto afterUndo = document.draft(document.snapshot());
    test.expect(!afterUndo.ids().allocateNode().has_value(),
                "undo must retain a previously published exhausted sentinel");
    test.expect(stack.redo().changed(), "allocator exhaustion should be redoable");
    auto afterRedo = document.draft(document.snapshot());
    test.expect(!afterRedo.ids().allocateNode().has_value(),
                "redo must retain the exhausted sentinel");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testAtomicTransactionUndoAndRedo(test);
        bloom::commands::test::testRejectedAndInvalidTransactionsAreAtomic(test);
        bloom::commands::test::testKnownParameterSchemaRejectionsAreAtomic(test);
        bloom::commands::test::testStaleExecuteUndoAndRedoAreNoOps(test);
        bloom::commands::test::testLayerOrderNoChangeAndRedoInvalidation(test);
        bloom::commands::test::testExhaustionSurvivesUndoAndRedo(test);
    } catch (const std::exception& error) {
        test.fail(std::string("unexpected test exception: ") + error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
