#include "command_test_support.hpp"

#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_operations.hpp>

#include <algorithm>
#include <limits>
#include <set>

namespace bloom::commands::test {
namespace {

using core::RationalTime;
using document::LayerMaskId;
using document::LayerMaskMode;
using document::PathAnchor;
using document::PathValue;

[[nodiscard]] PathValue square(const double size) {
    PathValue path;
    path.closed = true;
    path.anchors = {
        {{0, 0}, {}, {}}, {{size, 0}, {}, {}}, {{size, size}, {}, {}}, {{0, size}, {}, {}}};
    return path;
}

[[nodiscard]] PathValue nonFinitePath() {
    auto path = square(1);
    path.anchors[0].point.x = std::numeric_limits<double>::quiet_NaN();
    return path;
}

[[nodiscard]] bool sameTruth(const Composition& left, const Composition& right) {
    return std::ranges::equal(left.parameters().records(), right.parameters().records()) &&
           std::ranges::equal(left.animationCurves().records(),
                              right.animationCurves().records()) &&
           std::ranges::equal(left.graph().nodes(), right.graph().nodes()) &&
           std::ranges::equal(left.graph().layerOutputs(), right.graph().layerOutputs());
}

[[nodiscard]] const document::LayerOutputBoundary& layerOf(const Document& document,
                                                           const LayerId id = kFirstLayerId) {
    const auto* layer =
        document.snapshot().project().findComposition(kCompositionId)->graph().findLayer(id);
    requireFixture(layer != nullptr, "layer must exist");
    return *layer;
}

[[nodiscard]] bool valid(const Document& document) {
    return document.snapshot().project().validate().ok();
}

template <typename Op, typename... Args>
[[nodiscard]] CommandResult run(Document& document, CommandStack& stack, Args&&... args) {
    Transaction transaction("Mask edit", document.snapshot().revision());
    transaction.emplace<Op>(std::forward<Args>(args)...);
    return stack.execute(std::move(transaction));
}

[[nodiscard]] LayerMaskId addMask(TestContext& test, Document& document, CommandStack& stack,
                                  const LayerId layer = kFirstLayerId,
                                  const LayerMaskMode mode = LayerMaskMode::Add) {
    const auto result = run<AddLayerMask>(document, stack, kCompositionId, layer, square(10), mode);
    const auto mask = result.outputId<LayerMaskId>("mask");
    test.expect(result.changed() && mask.has_value() && valid(document), "mask is added");
    return mask.value_or(LayerMaskId{});
}

void testAddRemoveUndo(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();
    const auto first = addMask(test, document, stack);
    const auto second = addMask(test, document, stack, kFirstLayerId, LayerMaskMode::Subtract);
    const auto& masks = layerOf(document).masks;
    test.expect(masks.size() == 2 && masks[0].id == first && masks[1].id == second &&
                    masks[0].name == "Mask 1" && masks[1].name == "Mask 2" &&
                    masks[1].mode == LayerMaskMode::Subtract,
                "masks append in order with default names");
    const auto* path = composition(document.snapshot()).parameters().find(masks[0].path);
    test.expect(path && std::holds_alternative<ConstantValueSource>(path->source),
                "path parameter is constant");
    const auto afterAdd = document.snapshot();
    const auto removed =
        run<RemoveLayerMask>(document, stack, kCompositionId, kFirstLayerId, first);
    test.expect(removed.changed() && layerOf(document).masks.size() == 1 && valid(document) &&
                    composition(document.snapshot()).parameters().records().size() ==
                        composition(original).parameters().records().size() + 4,
                "remove erases the mask and its four parameters");
    test.expect(stack.undo().changed() &&
                    sameTruth(composition(document.snapshot()), composition(afterAdd)),
                "remove undo restores exact records");
    test.expect(stack.undo().changed() && stack.undo().changed() &&
                    sameTruth(composition(document.snapshot()), composition(original)),
                "add undo restores exact records");
    test.expect(
        !run<RemoveLayerMask>(document, stack, kCompositionId, kFirstLayerId, first).changed(),
        "removing a missing mask is rejected");
    test.expect(!run<AddLayerMask>(document, stack, kCompositionId, kFirstLayerId, nonFinitePath())
                     .changed(),
                "an invalid path is rejected");
    test.expect(
        !run<AddLayerMask>(document, stack, kCompositionId, LayerId::fromRaw(999), square(10))
             .changed(),
        "a missing layer is rejected");
}

void testLimitAndLock(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    for (std::size_t index = 0; index < document::kMaximumLayerMasks; ++index)
        (void)addMask(test, document, stack);
    test.expect(
        run<AddLayerMask>(document, stack, kCompositionId, kFirstLayerId, square(10)).status ==
            CommandStatus::Rejected,
        "the 65th mask is rejected");
    Document locked(makeProject());
    CommandStack lockedStack(locked);
    (void)addMask(test, locked, lockedStack);
    test.expect(lockedStack
                    .execute([&] {
                        Transaction t("Lock", locked.snapshot().revision());
                        t.emplace<SetLayerLocked>(kCompositionId, kFirstLayerId, true);
                        return t;
                    }())
                    .changed(),
                "lock layer");
    test.expect(
        run<AddLayerMask>(locked, lockedStack, kCompositionId, kFirstLayerId, square(5)).status ==
            CommandStatus::Rejected,
        "a locked layer rejects mask edits");
}

void testFieldEdits(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto a = addMask(test, document, stack);
    const auto b = addMask(test, document, stack);
    const auto c = addMask(test, document, stack);
    const auto base = document.snapshot();
    auto moved =
        run<MoveLayerMask>(document, stack, kCompositionId, kFirstLayerId, c, std::size_t{0});
    const auto& masks = layerOf(document).masks;
    test.expect(moved.changed() && masks[0].id == c && masks[1].id == a && masks[2].id == b &&
                    valid(document),
                "move reorders");
    test.expect(stack.undo().changed() &&
                    sameTruth(composition(document.snapshot()), composition(base)),
                "move undo is exact");
    test.expect(
        run<MoveLayerMask>(document, stack, kCompositionId, kFirstLayerId, a, std::size_t{0})
                .status == CommandStatus::NoChange,
        "same index is no change");
    test.expect(
        run<MoveLayerMask>(document, stack, kCompositionId, kFirstLayerId, a, std::size_t{3})
                .status == CommandStatus::Rejected,
        "out of range index is rejected");
    const auto mode = run<SetLayerMaskMode>(document, stack, kCompositionId, kFirstLayerId, a,
                                            LayerMaskMode::Intersect);
    test.expect(mode.changed() && layerOf(document).masks[0].mode == LayerMaskMode::Intersect &&
                    mode.affectedTimes.has_value(),
                "mode set with footprint");
    test.expect(run<SetLayerMaskMode>(document, stack, kCompositionId, kFirstLayerId, a,
                                      LayerMaskMode::Intersect)
                        .status == CommandStatus::NoChange,
                "same mode is no change");
    test.expect(run<SetLayerMaskInverted>(document, stack, kCompositionId, kFirstLayerId, a, true)
                        .changed() &&
                    layerOf(document).masks[0].inverted,
                "invert set");
    test.expect(
        run<RenameLayerMask>(document, stack, kCompositionId, kFirstLayerId, a, "Hero").changed() &&
            layerOf(document).masks[0].name == "Hero" && valid(document),
        "rename set");
    test.expect(
        run<RenameLayerMask>(document, stack, kCompositionId, kFirstLayerId, a, "").status ==
            CommandStatus::Rejected,
        "empty name rejected");
    const auto before = document.snapshot();
    test.expect(stack.undo().changed() && stack.undo().changed() && stack.undo().changed() &&
                    sameTruth(composition(document.snapshot()), composition(base)),
                "undoing all edits restores exact records");
    (void)before;
}

void testPathAnimation(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto mask = addMask(test, document, stack);
    const auto base = document.snapshot();
    // Constant replacement.
    test.expect(run<SetLayerMaskPath>(document, stack, kCompositionId, kFirstLayerId, mask,
                                      RationalTime::fromInteger(0), square(20))
                    .changed(),
                "constant path replaced");
    const auto pathId = layerOf(document).masks[0].path;
    const auto constantPath = [&] {
        const auto* record = composition(document.snapshot()).parameters().find(pathId);
        return std::get<PathValue>(std::get<ConstantValueSource>(record->source).value);
    };
    test.expect(constantPath() == square(20) &&
                    run<SetLayerMaskPath>(document, stack, kCompositionId, kFirstLayerId, mask,
                                          RationalTime::fromInteger(0), square(20))
                            .status == CommandStatus::NoChange,
                "constant value stored, repeat is no change");
    test.expect(stack.undo().changed() &&
                    sameTruth(composition(document.snapshot()), composition(base)),
                "constant replace undo exact");
    // Animate.
    const auto animated = run<AnimateLayerMaskPath>(document, stack, kCompositionId, kFirstLayerId,
                                                    mask, RationalTime::fromInteger(1));
    const auto curveId = animated.outputId<document::AnimationCurveId>("curve");
    test.expect(animated.changed() && curveId && valid(document), "animate creates a path curve");
    const auto& store = composition(document.snapshot()).animationCurves();
    const auto* curve = curveId ? store.findPath(*curveId) : nullptr;
    test.expect(curve && curve->keyframes.size() == 1 &&
                    curve->keyframes[0].time == RationalTime::fromInteger(1) &&
                    curve->keyframes[0].value == square(10),
                "one key holding the constant");
    test.expect(run<AnimateLayerMaskPath>(document, stack, kCompositionId, kFirstLayerId, mask,
                                          RationalTime::fromInteger(2))
                        .status == CommandStatus::NoChange,
                "animating twice is no change");
    // Keyed set: new key then update existing.
    test.expect(run<SetLayerMaskPath>(document, stack, kCompositionId, kFirstLayerId, mask,
                                      RationalTime::fromInteger(3), square(30))
                    .changed(),
                "new key inserted");
    test.expect(run<SetLayerMaskPath>(document, stack, kCompositionId, kFirstLayerId, mask,
                                      RationalTime::fromInteger(1), square(15))
                    .changed(),
                "existing key updated");
    const auto* after =
        curveId ? composition(document.snapshot()).animationCurves().findPath(*curveId) : nullptr;
    test.expect(after && after->keyframes.size() == 2 && after->keyframes[0].value == square(15) &&
                    after->keyframes[1].time == RationalTime::fromInteger(3) &&
                    after->keyframes[1].value == square(30) && valid(document),
                "two keys, edited values");
    const auto keyed = document.snapshot();
    // Stop animating.
    test.expect(run<StopAnimatingLayerMaskPath>(document, stack, kCompositionId, kFirstLayerId,
                                                mask, square(12))
                        .changed() &&
                    !composition(document.snapshot()).animationCurves().findPath(*curveId) &&
                    constantPath() == square(12) && valid(document),
                "stop animating restores a constant and drops the curve");
    test.expect(stack.undo().changed() &&
                    sameTruth(composition(document.snapshot()), composition(keyed)),
                "stop animating undo exact");
    // Removing the mask removes the curve.
    test.expect(
        run<RemoveLayerMask>(document, stack, kCompositionId, kFirstLayerId, mask).changed() &&
            composition(document.snapshot()).animationCurves().records().empty() && valid(document),
        "removing an animated mask erases its curve");
}

void testTrackMatte(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const document::TrackMatte matte{kSecondLayerId, document::TrackMatteChannel::Luma, true};
    const auto base = document.snapshot();
    const auto set = run<SetTrackMatte>(document, stack, kCompositionId, kFirstLayerId, matte);
    test.expect(set.changed() && layerOf(document).trackMatte == matte && valid(document) &&
                    set.affectedTimes.has_value(),
                "matte set");
    test.expect(run<SetTrackMatte>(document, stack, kCompositionId, kFirstLayerId, matte).status ==
                    CommandStatus::NoChange,
                "same matte is no change");
    test.expect(run<SetTrackMatte>(document, stack, kCompositionId, kSecondLayerId,
                                   document::TrackMatte{kFirstLayerId})
                        .status == CommandStatus::Rejected,
                "a cycle is rejected");
    test.expect(run<SetTrackMatte>(document, stack, kCompositionId, kFirstLayerId,
                                   document::TrackMatte{kFirstLayerId})
                        .status == CommandStatus::Rejected,
                "self is rejected");
    test.expect(run<SetTrackMatte>(document, stack, kCompositionId, kFirstLayerId,
                                   document::TrackMatte{LayerId::fromRaw(999)})
                        .status == CommandStatus::Rejected,
                "a missing source is rejected");
    test.expect(run<SetTrackMatte>(document, stack, kCompositionId, kFirstLayerId, std::nullopt)
                        .changed() &&
                    !layerOf(document).trackMatte,
                "matte cleared");
    test.expect(stack.undo().changed() && layerOf(document).trackMatte == matte &&
                    stack.undo().changed() &&
                    sameTruth(composition(document.snapshot()), composition(base)),
                "matte undo exact");
}

void testDeleteDuplicateSplit(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto base = document.snapshot();
    const auto mask = addMask(test, document, stack);
    (void)run<AnimateLayerMaskPath>(document, stack, kCompositionId, kFirstLayerId, mask,
                                    RationalTime::fromInteger(1));
    (void)run<SetTrackMatte>(document, stack, kCompositionId, kFirstLayerId,
                             document::TrackMatte{kSecondLayerId});
    const auto withMask = document.snapshot();
    const auto& oldMask = layerOf(document).masks[0];

    // Duplicate.
    const auto duplicated = run<DuplicateNodes>(document, stack, kCompositionId,
                                                std::set<NodeId>{kFirstLayerNodeId}, Vec2d{0, 0});
    const auto copyId =
        duplicated.outputId<LayerId>("layer." + std::to_string(kFirstLayerId.value()));
    test.expect(duplicated.changed() && copyId && valid(document), "duplicate validates");
    if (copyId) {
        const auto& copy = layerOf(document, *copyId);
        test.expect(
            copy.masks.size() == 1 && copy.masks[0].id != oldMask.id &&
                copy.masks[0].path != oldMask.path && copy.masks[0].feather != oldMask.feather &&
                copy.masks[0].expansion != oldMask.expansion &&
                copy.masks[0].opacity != oldMask.opacity && copy.masks[0].name == oldMask.name,
            "copy owns fresh mask ids and parameters");
        test.expect(copy.trackMatte == layerOf(document).trackMatte, "copy keeps the matte source");
        const auto& curves = composition(document.snapshot()).animationCurves().records();
        test.expect(curves.size() == 2, "copy owns its own curve");
        // Delete the copy: its parameters and curve go.
        const auto copyNode = copy.nodeId;
        test.expect(run<RemoveNodes>(document, stack, kCompositionId, std::set<NodeId>{copyNode})
                            .changed() &&
                        composition(document.snapshot()).animationCurves().records().size() == 1 &&
                        valid(document),
                    "deleting the copy erases its mask parameters and curve");
    }
    // Delete the original layer: all mask parameters and curves go.
    test.expect(
        run<RemoveNodes>(document, stack, kCompositionId, std::set<NodeId>{kFirstLayerNodeId})
                .changed() &&
            composition(document.snapshot()).animationCurves().records().empty() &&
            composition(document.snapshot()).parameters().records().size() ==
                composition(base).parameters().records().size() - 6 &&
            valid(document),
        "deleting a layer erases its mask parameters and curves");
    test.expect(stack.undo().changed() &&
                    sameTruth(composition(document.snapshot()), composition(document.snapshot())) &&
                    valid(document),
                "delete undo validates");
    (void)withMask;
}

void testDeleteMatteSource(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    (void)run<SetTrackMatte>(document, stack, kCompositionId, kFirstLayerId,
                             document::TrackMatte{kSecondLayerId});
    const auto removed =
        run<RemoveNodes>(document, stack, kCompositionId, std::set<NodeId>{kSecondLayerNodeId});
    test.expect(removed.changed() && !layerOf(document).trackMatte && valid(document) &&
                    !removed.affectedTimes.has_value(),
                "deleting a matte source clears the matte and renders whole");
}

void testSplit(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    (void)addMask(test, document, stack);
    const auto split = run<SplitLayerAtTime>(document, stack, kCompositionId, kFirstLayerId,
                                             RationalTime::fromInteger(4));
    const auto copy = split.outputId<LayerId>("layer");
    test.expect(split.changed() && copy && valid(document) &&
                    layerOf(document, *copy).masks.size() == 1 &&
                    layerOf(document, *copy).masks[0].id != layerOf(document).masks[0].id,
                "splitting copies masks with fresh ids");
    (void)run<SetTrackMatte>(document, stack, kCompositionId, kFirstLayerId,
                             document::TrackMatte{kSecondLayerId});
    test.expect(run<SplitLayerAtTime>(document, stack, kCompositionId, kSecondLayerId,
                                      RationalTime::fromInteger(4))
                        .status == CommandStatus::Rejected,
                "a matte source cannot be split");
}

void testDuplicateComposition(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    (void)addMask(test, document, stack);
    (void)run<SetTrackMatte>(document, stack, kCompositionId, kFirstLayerId,
                             document::TrackMatte{kSecondLayerId});
    const auto result = run<DuplicateComposition>(document, stack, kCompositionId);
    test.expect(result.changed() && valid(document), "composition clone with masks validates");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testAddRemoveUndo(test);
        bloom::commands::test::testLimitAndLock(test);
        bloom::commands::test::testFieldEdits(test);
        bloom::commands::test::testPathAnimation(test);
        bloom::commands::test::testTrackMatte(test);
        bloom::commands::test::testDeleteDuplicateSplit(test);
        bloom::commands::test::testDeleteMatteSource(test);
        bloom::commands::test::testSplit(test);
        bloom::commands::test::testDuplicateComposition(test);
    } catch (const std::exception& error) {
        test.fail(std::string("unexpected test exception: ") + error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
