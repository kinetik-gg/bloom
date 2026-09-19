#include "command_test_support.hpp"

#include <algorithm>
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

// A deliberately unclassified operation: it leaves renderAffecting() at its default, so it proves
// the conservative CommandStack behavior without depending on any node command class.
class UnclassifiedRename final : public Operation {
  public:
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.test.unclassified-rename";
    }

    [[nodiscard]] OperationResult apply(document::Draft& draft) const override {
        draft.project().setName("Unclassified");
        return OperationResult::applied();
    }
};

// TEMPORAL-1: an operation that proves a finite footprint but does not change the document (so it
// can be layered many times in one transaction) without depending on a real layer command.
class FootprintOp final : public Operation {
  public:
    FootprintOp(AffectedTimeFootprint footprint, bool renderAffecting = true)
        : footprint_(std::move(footprint)), renderAffecting_(renderAffecting) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.test.footprint";
    }
    [[nodiscard]] OperationResult apply(document::Draft&) const override {
        auto result = OperationResult::applied();
        result.affectedTimes = footprint_;
        return result;
    }
    [[nodiscard]] bool renderAffecting() const noexcept override { return renderAffecting_; }

  private:
    AffectedTimeFootprint footprint_;
    bool renderAffecting_;
};

// A render-neutral operation (renderAffecting false) that also carries no footprint.
class NeutralOp final : public Operation {
  public:
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.test.neutral"; }
    [[nodiscard]] OperationResult apply(document::Draft&) const override {
        return OperationResult::applied();
    }
    [[nodiscard]] bool renderAffecting() const noexcept override { return false; }
};

// SPLIT-1: a finite footprint plus ordered remaps, to exercise CommandStack aggregation, inversion,
// and forward replay without depending on a real layer command.
class RemapOp final : public Operation {
  public:
    RemapOp(AffectedTimeFootprint footprint, std::vector<LayerIdentityRemap> remaps)
        : footprint_(std::move(footprint)), remaps_(std::move(remaps)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.test.remap"; }
    [[nodiscard]] OperationResult apply(document::Draft&) const override {
        auto result = OperationResult::applied();
        result.affectedTimes = footprint_;
        result.layerIdentityRemaps = remaps_;
        return result;
    }

  private:
    AffectedTimeFootprint footprint_;
    std::vector<LayerIdentityRemap> remaps_;
};

[[nodiscard]] LayerIdentityRemap remap(const std::uint64_t before, const std::uint64_t after) {
    return LayerIdentityRemap{.compositionId = kCompositionId,
                              .start = core::RationalTime::fromInteger(1),
                              .end = core::RationalTime::fromInteger(4),
                              .beforeLayerId = LayerId::fromRaw(before),
                              .afterLayerId = LayerId::fromRaw(after),
                              .beforeNodeId = NodeId::fromRaw(before + 100),
                              .afterNodeId = NodeId::fromRaw(after + 100)};
}

[[nodiscard]] AffectedTimeFootprint
footprint(const std::vector<std::pair<core::RationalTime, core::RationalTime>>& spans) {
    AffectedTimeFootprint result;
    result.compositionId = kCompositionId;
    for (const auto& [start, end] : spans)
        result.intervals.push_back({start, end});
    return result;
}

void testLayerIdentityRemapAggregation(TestContext& test) {
    const auto t = [](const std::int64_t value) { return core::RationalTime::fromInteger(value); };

    // normalize: malformed descriptor, mixed composition, and cap overflow all fail closed.
    {
        auto malformed = remap(30, 31);
        malformed.start = t(4);
        malformed.end = t(1);
        test.expect(!normalizeLayerIdentityRemaps({malformed}).has_value(),
                    "an inverted remap range is not provable");
    }
    {
        auto foreign = remap(30, 31);
        foreign.compositionId = CompositionId::fromRaw(999);
        test.expect(!normalizeLayerIdentityRemaps({remap(30, 31), foreign}).has_value(),
                    "a mixed-composition remap list is not provable");
    }
    {
        std::vector<LayerIdentityRemap> many;
        many.reserve(10);
        for (std::uint64_t index = 0; index < 10; ++index)
            many.push_back(remap(30 + index * 2, 31 + index * 2));
        test.expect(!normalizeLayerIdentityRemaps(many).has_value(),
                    "remap cap overflow falls back to whole render");
    }

    Document document(makeProject());
    CommandStack stack(document);
    // Forward aggregation preserves order; undo reverses and swaps; redo replays forward.
    {
        Transaction transaction("Two ordered remaps", document.snapshot().revision());
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{remap(30, 31)});
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{remap(31, 32)});
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.changed() && result.affectedTimes.has_value() &&
                        result.layerIdentityRemaps.has_value() &&
                        result.layerIdentityRemaps->size() == 2 &&
                        result.layerIdentityRemaps->at(0).beforeLayerId == LayerId::fromRaw(30) &&
                        result.layerIdentityRemaps->at(1).beforeLayerId == LayerId::fromRaw(31),
                    "forward execute preserves remap order");
        const auto undo = stack.undo();
        // Forward [30->31, 31->32] inverted: reverse order then swap each => [32->31, 31->30].
        test.expect(undo.changed() && undo.layerIdentityRemaps.has_value() &&
                        undo.layerIdentityRemaps->size() == 2 &&
                        undo.layerIdentityRemaps->at(0).beforeLayerId == LayerId::fromRaw(32) &&
                        undo.layerIdentityRemaps->at(0).afterLayerId == LayerId::fromRaw(31) &&
                        undo.layerIdentityRemaps->at(1).beforeLayerId == LayerId::fromRaw(31) &&
                        undo.layerIdentityRemaps->at(1).afterLayerId == LayerId::fromRaw(30),
                    "undo reverses order and swaps before/after IDs");
        const auto redo = stack.redo();
        test.expect(redo.changed() && redo.layerIdentityRemaps.has_value() &&
                        redo.layerIdentityRemaps->at(0).beforeLayerId == LayerId::fromRaw(30) &&
                        redo.layerIdentityRemaps->at(1).beforeLayerId == LayerId::fromRaw(31),
                    "redo replays the stored forward ordered list");
    }
    // An unclassified op in the same transaction clears remaps and forces whole render.
    {
        Transaction transaction("Remap plus unclassified", document.snapshot().revision());
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{remap(40, 41)});
        transaction.emplace<SetProjectName>("Whole");
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.changed() && !result.affectedTimes.has_value() &&
                        !result.layerIdentityRemaps.has_value(),
                    "an unclassified op clears remaps and forces whole render");
    }
    // A neutral op beside a remap leaves the remap intact.
    {
        Transaction transaction("Remap plus neutral", document.snapshot().revision());
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{remap(50, 51)});
        transaction.emplace<NeutralOp>();
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.changed() && result.affectedTimes.has_value() &&
                        result.layerIdentityRemaps.has_value() &&
                        result.layerIdentityRemaps->size() == 1,
                    "a neutral op leaves a remap intact");
    }
    // A rejected transaction advertises no remap evidence.
    {
        Transaction transaction("Rejected remap", document.snapshot().revision());
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{remap(60, 61)});
        transaction.emplace<SetCompositionName>(CompositionId::fromRaw(9999), "Missing");
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.status == CommandStatus::Rejected &&
                        !result.layerIdentityRemaps.has_value(),
                    "a rejected transaction advertises no remap evidence");
    }
    // FAIL-CLOSED at the CommandStack level (not merely the normalize helper): any remap
    // normalization, composition, or cap failure must make the whole transaction whole-render and
    // publish NEITHER affectedTimes nor remaps, and undo/redo must stay whole too.
    const auto expectWhole = [&test](const CommandResult& result, const std::string_view message) {
        test.expect(result.changed() && result.renderAffecting &&
                        !result.affectedTimes.has_value() &&
                        !result.layerIdentityRemaps.has_value(),
                    message);
    };
    {
        // More than kMaxLayerIdentityRemaps remaps spread across applied ops.
        Transaction transaction("Cap overflow remaps", document.snapshot().revision());
        for (std::uint64_t index = 0; index < kMaxLayerIdentityRemaps + 1; ++index) {
            transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}),
                                         std::vector{remap(70 + index * 2, 71 + index * 2)});
        }
        const auto result = stack.execute(std::move(transaction));
        expectWhole(result, "a cap-overflow remap transaction is whole-render with no metadata");
        const auto undo = stack.undo();
        expectWhole(undo, "undo of a cap-overflow remap transaction stays whole-render");
        const auto redo = stack.redo();
        expectWhole(redo, "redo of a cap-overflow remap transaction stays whole-render");
    }
    {
        // A malformed descriptor (inverted range) inside an applied op.
        auto malformed = remap(90, 91);
        malformed.start = t(4);
        malformed.end = t(1);
        Transaction transaction("Malformed remap", document.snapshot().revision());
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{malformed});
        const auto result = stack.execute(std::move(transaction));
        expectWhole(result, "a malformed remap descriptor is whole-render with no metadata");
    }
    {
        // Footprint composition A but remap composition B.
        auto foreign = remap(92, 93);
        foreign.compositionId = CompositionId::fromRaw(999);
        Transaction transaction("Footprint/remap composition mismatch",
                                document.snapshot().revision());
        transaction.emplace<RemapOp>(footprint({{t(1), t(4)}}), std::vector{foreign});
        const auto result = stack.execute(std::move(transaction));
        expectWhole(result,
                    "a footprint/remap composition mismatch is whole-render with no metadata");
        const auto undo = stack.undo();
        expectWhole(undo, "undo of a composition-mismatch remap transaction stays whole-render");
        const auto redo = stack.redo();
        expectWhole(redo, "redo of a composition-mismatch remap transaction stays whole-render");
    }
}

void testFiniteTimeFootprintAggregation(TestContext& test) {
    const auto t = [](const std::int64_t value) { return core::RationalTime::fromInteger(value); };

    // normalize: overlapping and exactly-adjacent intervals merge deterministically.
    {
        const auto normalized =
            normalizeAffectedTimeFootprint(footprint({{t(5), t(7)}, {t(1), t(3)}, {t(3), t(5)}}));
        test.expect(normalized.has_value() && normalized->intervals.size() == 1 &&
                        normalized->intervals.front().start == t(1) &&
                        normalized->intervals.front().end == t(7),
                    "overlapping/adjacent intervals normalize to one");
    }
    // An empty footprint is deliberate and stays empty.
    {
        const auto normalized = normalizeAffectedTimeFootprint(footprint({}));
        test.expect(normalized.has_value() && normalized->intervals.empty(),
                    "an explicitly empty footprint stays empty");
    }
    // An inverted interval is not provable and falls back to whole render.
    {
        test.expect(!normalizeAffectedTimeFootprint(footprint({{t(4), t(2)}})).has_value(),
                    "an inverted interval is not provable");
    }
    // A different composition fails the merge closed.
    {
        auto other = footprint({{t(1), t(2)}});
        other.compositionId = CompositionId::fromRaw(999);
        test.expect(!mergeAffectedTimeFootprints(footprint({{t(0), t(1)}}), other).has_value(),
                    "a mixed-composition union is not provable");
    }
    // Too many disjoint intervals exceed the cap and fall back to whole render.
    {
        std::vector<std::pair<core::RationalTime, core::RationalTime>> many;
        many.reserve(10);
        for (std::int64_t index = 0; index < 10; ++index)
            many.emplace_back(t(index * 4), t(index * 4 + 1));
        test.expect(!normalizeAffectedTimeFootprint(footprint(many)).has_value(),
                    "pathological interval growth falls back to whole render");
    }

    // CommandStack aggregation: two finite ops union and normalize; an unclassified op dominates.
    Document document(makeProject());
    CommandStack stack(document);
    {
        Transaction transaction("Two finite ops", document.snapshot().revision());
        transaction.emplace<FootprintOp>(footprint({{t(4), t(10)}}));
        transaction.emplace<FootprintOp>(footprint({{t(2), t(4)}}));
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.changed() && result.renderAffecting &&
                        result.affectedTimes.has_value() &&
                        result.affectedTimes->intervals.size() == 1 &&
                        result.affectedTimes->intervals.front().start == t(2) &&
                        result.affectedTimes->intervals.front().end == t(10),
                    "two finite ops union into one normalized interval");
        const auto undo = stack.undo();
        test.expect(undo.changed() && undo.affectedTimes.has_value() &&
                        undo.affectedTimes->intervals.front().start == t(2) &&
                        undo.affectedTimes->intervals.front().end == t(10),
                    "undo replays the aggregated finite footprint");
        const auto redo = stack.redo();
        test.expect(redo.changed() && redo.affectedTimes.has_value() &&
                        redo.affectedTimes->intervals.size() == 1,
                    "redo replays the aggregated finite footprint");
    }
    {
        Transaction transaction("Finite plus unclassified", document.snapshot().revision());
        transaction.emplace<FootprintOp>(footprint({{t(1), t(2)}}));
        transaction.emplace<SetProjectName>("Whole render");
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.changed() && result.renderAffecting && !result.affectedTimes.has_value(),
                    "a finite footprint beside an unclassified op falls back to whole render");
    }
    {
        Transaction transaction("Finite plus neutral", document.snapshot().revision());
        transaction.emplace<FootprintOp>(footprint({{t(1), t(2)}}));
        transaction.emplace<NeutralOp>();
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.changed() && result.renderAffecting &&
                        result.affectedTimes.has_value() &&
                        result.affectedTimes->intervals.size() == 1,
                    "a neutral op does not widen a finite footprint");
    }
    {
        // A rejected transaction never advertises reuse evidence.
        Transaction transaction("Rejected finite", document.snapshot().revision());
        transaction.emplace<FootprintOp>(footprint({{t(1), t(2)}}));
        transaction.emplace<SetCompositionName>(CompositionId::fromRaw(9999), "Missing");
        const auto result = stack.execute(std::move(transaction));
        test.expect(result.status == CommandStatus::Rejected && !result.affectedTimes.has_value(),
                    "a rejected transaction advertises no footprint");
    }
}

void testRenderAffectingPublication(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    std::vector<CommandEventKind> events;
    (void)stack.addObserver([&events](const CommandEvent& event) { events.push_back(event.kind); });
    const auto published = [&events](const CommandEventKind kind) {
        return std::ranges::find(events, kind) != events.end();
    };

    events.clear();
    Transaction rejected("Rejected", document.snapshot().revision());
    rejected.emplace<SetCompositionName>(CompositionId::fromRaw(9999), "Missing");
    const auto rejectedResult = stack.execute(std::move(rejected));
    test.expect(rejectedResult.status == CommandStatus::Rejected &&
                    !published(CommandEventKind::RevisionChanged) && rejectedResult.renderAffecting,
                "a rejected transaction publishes no RevisionChanged and keeps the conservative "
                "render-affecting default");

    events.clear();
    Transaction rename("Rename", document.snapshot().revision());
    rename.emplace<SetProjectName>("Renamed");
    const auto renameResult = stack.execute(std::move(rename));
    test.expect(renameResult.changed() && published(CommandEventKind::RevisionChanged),
                "a successful unclassified transaction publishes RevisionChanged");

    events.clear();
    Transaction repeat("Repeat rename", document.snapshot().revision());
    repeat.emplace<SetProjectName>("Renamed");
    const auto repeatResult = stack.execute(std::move(repeat));
    test.expect(repeatResult.status == CommandStatus::NoChange &&
                    !published(CommandEventKind::RevisionChanged) && repeatResult.renderAffecting,
                "a no-change transaction publishes no RevisionChanged and keeps the conservative "
                "render-affecting default");

    events.clear();
    Transaction unclassified("Unclassified custom", document.snapshot().revision());
    unclassified.emplace<UnclassifiedRename>();
    const auto unclassifiedResult = stack.execute(std::move(unclassified));
    test.expect(unclassifiedResult.changed() && unclassifiedResult.renderAffecting,
                "an unclassified custom operation defaults to render-affecting");
}

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
        bloom::commands::test::testFiniteTimeFootprintAggregation(test);
        bloom::commands::test::testLayerIdentityRemapAggregation(test);
        bloom::commands::test::testRenderAffectingPublication(test);
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
