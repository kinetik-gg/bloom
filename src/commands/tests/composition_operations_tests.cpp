#include "command_test_support.hpp"

#include <bloom/document/new_project.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>

namespace bloom::commands::test {
namespace {

using document::CompositionFormat;
using document::FrameRate;
using document::NodeId;

void testAddCompositionBuildsTopologyAndUndoRedo(TestContext& test) {
    const auto frameRate = FrameRate::create(30, 1).value_or(FrameRate::framesPerSecond24());
    const auto format =
        CompositionFormat::create(1280, 720, core::PixelAspectRatio::square(), frameRate)
            .value_or(CompositionFormat{});

    Document document(
        document::makeNewProject("Project", "Main", core::RationalTime::fromInteger(10)).project);
    CommandStack stack(document);
    const auto before = document.snapshot();
    Transaction transaction("Add Composition", before.revision());
    transaction.emplace<AddComposition>("Second", format, frameRate,
                                        core::RationalTime::fromInteger(20));
    const auto result = stack.execute(std::move(transaction));
    const auto compositionId = result.outputId<document::CompositionId>(kAddCompositionOutput)
                                   .value_or(document::CompositionId{});
    const auto after = document.snapshot();
    const auto* added = after.project().findComposition(compositionId);

    test.expect(result.changed() && compositionId.isValid() && added != nullptr,
                "AddComposition creates and returns a composition");
    test.expect(added != nullptr && added->name() == "Second" &&
                    added->duration() == core::RationalTime::fromInteger(20) &&
                    added->format() == format && added->graph().nodes().size() == 2 &&
                    added->graph().edges().size() == 1 &&
                    added->graph().compositionOutput().has_value(),
                "AddComposition builds the Merge-to-Output topology and requested settings");
    test.expect(after.revision().value() == before.revision().value() + 1 && stack.size() == 1 &&
                    stack.trackedRevision() == after.revision(),
                "AddComposition has one dirty revision and one history entry");

    test.expect(stack.undo().changed() && document.snapshot().project().compositions().size() == 1,
                "AddComposition is one undo step");
    test.expect(stack.redo().changed() &&
                    document.snapshot().project().findComposition(compositionId) != nullptr,
                "AddComposition redo restores the pinned composition");
}

void testDeleteCompositionAllowsLastAndUndoRedo(TestContext& test) {
    Document document(
        document::makeNewProject("Project", "Main", core::RationalTime::fromInteger(10)).project);
    CommandStack stack(document);
    const auto initialId = document::CompositionId::fromRaw(1);
    const auto format = CompositionFormat::create(640, 480).value_or(CompositionFormat{});

    Transaction add("Add Composition", document.snapshot().revision());
    add.emplace<AddComposition>("Second", format, core::RationalTime::fromInteger(8));
    const auto added = stack.execute(std::move(add));
    const auto secondId = added.outputId<document::CompositionId>(kAddCompositionOutput)
                              .value_or(document::CompositionId{});

    const auto beforeDelete = document.snapshot();
    Transaction remove("Delete Composition", beforeDelete.revision());
    remove.emplace<DeleteComposition>(initialId);
    const auto removed = stack.execute(std::move(remove));
    test.expect(removed.changed() &&
                    document.snapshot().project().findComposition(initialId) == nullptr,
                "DeleteComposition removes the requested composition");
    test.expect(stack.undo().changed() &&
                    document.snapshot().project().findComposition(initialId) != nullptr,
                "DeleteComposition undo restores the composition");
    test.expect(stack.redo().changed() &&
                    document.snapshot().project().findComposition(initialId) == nullptr,
                "DeleteComposition redo reapplies the deletion");

    // Deleting the FINAL composition is allowed and yields the same usable empty project a blank
    // startup begins with; undo restores it as one history step.
    const auto lastBefore = document.snapshot();
    Transaction last("Delete Last Composition", lastBefore.revision());
    last.emplace<DeleteComposition>(secondId);
    const auto lastDelete = stack.execute(std::move(last));
    test.expect(lastDelete.changed() && document.snapshot().project().compositions().empty() &&
                    document.snapshot().project().validate().ok(),
                "DeleteComposition deletes the last composition and leaves a valid empty project");
    test.expect(stack.size() == 3, "deleting the last composition is a single history entry");
    test.expect(stack.undo().changed() &&
                    document.snapshot().project().findComposition(secondId) != nullptr &&
                    document.snapshot().project().validate().ok(),
                "undo restores the final composition");
    test.expect(stack.redo().changed() && document.snapshot().project().compositions().empty(),
                "redo removes the final composition again");
}

void testDuplicateCompositionRemapsDeepDocumentState(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);

    Transaction group("Group Nodes", document.snapshot().revision());
    group.emplace<GroupNodes>(kCompositionId, std::set<NodeId>{kFirstLayerNodeId}, "Frame");
    test.expect(stack.execute(std::move(group)).changed(), "duplicate fixture creates a group");
    Transaction move("Move Nodes", document.snapshot().revision());
    move.emplace<MoveNodes>(kCompositionId,
                            std::map<NodeId, Vec2d>{{kFirstLayerNodeId, Vec2d{42.0, 84.0}}});
    test.expect(stack.execute(std::move(move)).changed(), "duplicate fixture moves a node");
    Transaction animate("Animate Opacity", document.snapshot().revision());
    animate.emplace<CreateAnimationForParameter>(kCompositionId, kOpacityId,
                                                 core::RationalTime::fromInteger(0));
    test.expect(stack.execute(std::move(animate)).changed(), "duplicate fixture creates animation");

    const auto before = document.snapshot();
    const auto* source = before.project().findComposition(kCompositionId);
    requireFixture(source != nullptr, "duplicate source must exist");
    Transaction duplicate("Duplicate Composition", before.revision());
    duplicate.emplace<DuplicateComposition>(kCompositionId);
    const auto result = stack.execute(std::move(duplicate));
    const auto copyId = result.outputId<document::CompositionId>(kDuplicateCompositionOutput)
                            .value_or(document::CompositionId{});
    const auto after = document.snapshot();
    const auto* copy = after.project().findComposition(copyId);

    test.expect(result.changed() && copyId.isValid() && copy != nullptr &&
                    copy->name() == "Main copy" && copy->format() == source->format() &&
                    copy->duration() == source->duration(),
                "DuplicateComposition creates the named settings-preserving copy");
    test.expect(copy != nullptr && copy->graph().nodes().size() == source->graph().nodes().size() &&
                    copy->graph().edges().size() == source->graph().edges().size() &&
                    copy->parameters().records().size() == source->parameters().records().size() &&
                    copy->animationCurves().records().size() ==
                        source->animationCurves().records().size() &&
                    copy->nodeLayout().size() == source->nodeLayout().size() &&
                    copy->nodeGroups().size() == source->nodeGroups().size(),
                "DuplicateComposition preserves graph, values, animation, layout, and groups");

    if (copy != nullptr) {
        test.expect(std::ranges::none_of(copy->graph().nodes(),
                                         [&](const auto& node) {
                                             return source->graph().findNode(node.id) != nullptr;
                                         }) &&
                        std::ranges::none_of(copy->graph().edges(),
                                             [&](const auto& edge) {
                                                 return std::ranges::any_of(
                                                     source->graph().edges(), [&](const auto& old) {
                                                         return old.id == edge.id;
                                                     });
                                             }),
                    "DuplicateComposition gives graph records fresh IDs");
        test.expect(std::ranges::all_of(copy->nodeGroups(),
                                        [&](const auto& entry) {
                                            return std::ranges::none_of(
                                                entry.second.members, [&](const auto nodeId) {
                                                    return source->graph().findNode(nodeId) !=
                                                           nullptr;
                                                });
                                        }),
                    "DuplicateComposition remaps group membership to copied nodes");
        const auto* copiedOpacity = copy->parameters().find(
            std::ranges::find_if(copy->parameters().records(), [](const auto& parameter) {
                return parameter.schemaKey == document::kOpacityParameterSchemaKey;
            })->id);
        test.expect(
            copiedOpacity != nullptr &&
                std::holds_alternative<document::AnimationCurveSource>(copiedOpacity->source),
            "DuplicateComposition preserves animated parameter sources");
    }

    const auto copyRevision = after.revision();
    test.expect(stack.undo().changed() &&
                    document.snapshot().project().findComposition(copyId) == nullptr,
                "DuplicateComposition is one undo step");
    test.expect(stack.redo().changed() &&
                    document.snapshot().project().findComposition(copyId) != nullptr &&
                    document.snapshot().revision().value() == copyRevision.value() + 2,
                "DuplicateComposition redo restores the exact copied IDs and revision parity");
}

void testSafeAreaSettingsAreUndoableAndDuplicated(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto settings = document::SafeAreaSettings{.action = 0.93, .title = 0.90};

    Transaction setSafeAreas("Set Safe Areas", document.snapshot().revision());
    setSafeAreas.emplace<SetCompositionSafeAreas>(kCompositionId, settings);
    const auto changed = stack.execute(std::move(setSafeAreas));
    const auto* authored = document.snapshot().project().findComposition(kCompositionId);
    test.expect(changed.changed() && authored != nullptr && authored->safeAreas() == settings,
                "SetCompositionSafeAreas writes the validated composition setting");
    test.expect(stack.undo().changed() &&
                    document.snapshot().project().findComposition(kCompositionId)->safeAreas() ==
                        document::SafeAreaSettings{},
                "SetCompositionSafeAreas is one undoable edit");
    test.expect(stack.redo().changed() &&
                    document.snapshot().project().findComposition(kCompositionId)->safeAreas() ==
                        settings,
                "SetCompositionSafeAreas redo restores the exact percentages");

    Transaction duplicate("Duplicate Composition", document.snapshot().revision());
    duplicate.emplace<DuplicateComposition>(kCompositionId);
    const auto result = stack.execute(std::move(duplicate));
    const auto copyId = result.outputId<document::CompositionId>(kDuplicateCompositionOutput)
                            .value_or(document::CompositionId{});
    const auto* copy = document.snapshot().project().findComposition(copyId);
    test.expect(copy != nullptr && copy->safeAreas() == settings,
                "DuplicateComposition carries per-composition safe-area settings");
}

void testWorkingColorSpaceOverrideIsUndoable(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    Transaction setWorkingSpace("Set Working Space", document.snapshot().revision());
    setWorkingSpace.emplace<SetCompositionWorkingColorSpace>(kCompositionId,
                                                             std::optional<std::string>{"ACEScg"});
    const auto changed = stack.execute(std::move(setWorkingSpace));
    const auto* composition = document.snapshot().project().findComposition(kCompositionId);
    test.expect(changed.changed() && composition != nullptr &&
                    composition->workingColorSpaceId() == std::optional<std::string>{"ACEScg"},
                "SetCompositionWorkingColorSpace writes the override");
    test.expect(
        stack.undo().changed() &&
            document.snapshot().project().findComposition(kCompositionId)->workingColorSpaceId() ==
                std::nullopt,
        "working colour-space override is one undo step");
    test.expect(
        stack.redo().changed() &&
            document.snapshot().project().findComposition(kCompositionId)->workingColorSpaceId() ==
                std::optional<std::string>{"ACEScg"},
        "redo restores the working colour-space override");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testAddCompositionBuildsTopologyAndUndoRedo(test);
        bloom::commands::test::testDeleteCompositionAllowsLastAndUndoRedo(test);
        bloom::commands::test::testDuplicateCompositionRemapsDeepDocumentState(test);
        bloom::commands::test::testSafeAreaSettingsAreUndoableAndDuplicated(test);
        bloom::commands::test::testWorkingColorSpaceOverrideIsUndoable(test);
    } catch (const std::exception& error) {
        test.fail(std::string("unexpected test exception: ") + error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
