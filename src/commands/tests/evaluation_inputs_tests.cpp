// #399: presentation-only edits never invalidate render caches. The document classifies every field
// (bloom/document/evaluation_inputs.hpp) and the command stack proves each transaction's render
// impact from it, so a layout, naming or organization edit -- and its undo and redo -- publishes a
// render-neutral result, while every evaluation input still invalidates.
#include "command_test_support.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/evaluation_inputs.hpp>

#include <array>
#include <functional>
#include <map>
#include <set>
#include <string_view>

namespace bloom::commands::test {
namespace {
using namespace document;

// Applies `edit` to an uncommittable copy of the fixture and reports whether the projection still
// calls the two projects evaluation-equal.
[[nodiscard]] bool neutralEdit(const std::function<void(Project&)>& edit) {
    Document document(makeProject());
    const auto before = document.snapshot();
    auto draft = Draft::isolatedFrom(before);
    edit(draft.project());
    return sameEvaluationInputs(before.project(), std::as_const(draft).project());
}

[[nodiscard]] Composition& mainComposition(Project& project) {
    return *project.findComposition(kCompositionId);
}
[[nodiscard]] LayerOutputBoundary& firstLayer(Project& project) {
    return *mainComposition(project).graph().findLayer(kFirstLayerId);
}

void testClassification(TestContext& test) {
    test.expect(neutralEdit([](Project&) {}), "an unchanged project is evaluation-equal");

    // Rule 1: presentation collections, whole -- any field of a group record.
    test.expect(neutralEdit([](Project& project) {
                    mainComposition(project).nodeGroups().emplace(
                        NodeGroupId::fromRaw(900), NodeGroupRecord{.id = NodeGroupId::fromRaw(900),
                                                                   .name = "Frame",
                                                                   .members = {kFirstLayerNodeId},
                                                                   .padding = {40.0, 40.0}});
                }),
                "a node group (frame, name, padding) is presentation");
    test.expect(neutralEdit([](Project& project) { project.setName("Renamed"); }),
                "the project name is presentation");

    // Rule 2: node layout is presentation except mute.
    test.expect(neutralEdit([](Project& project) {
                    auto& record = mainComposition(project).nodeLayout()[kFirstLayerNodeId];
                    record.position = {512.0, -64.0};
                    record.width = 300.0;
                    record.collapsed = true;
                }),
                "card position, width and collapse are presentation");
    test.expect(!neutralEdit([](Project& project) {
        mainComposition(project).nodeLayout()[kFirstLayerNodeId].muted = true;
    }),
                "mute is an evaluation input");

    // Rule 3: a layer boundary's presentation fields.
    test.expect(neutralEdit([](Project& project) {
                    auto& layer = firstLayer(project);
                    layer.name = "Renamed layer";
                    layer.labelColor = std::array<std::uint8_t, 3>{200, 40, 40};
                    layer.locked = true;
                }),
                "a layer's name, label color and lock are presentation");
    test.expect(!neutralEdit([](Project& project) { firstLayer(project).enabled = false; }),
                "layer enable is an evaluation input");
    test.expect(!neutralEdit([](Project& project) { firstLayer(project).solo = true; }),
                "layer solo is an evaluation input");
    test.expect(!neutralEdit([](Project& project) {
        firstLayer(project).inPoint = core::RationalTime::fromInteger(2);
    }),
                "a layer's range is an evaluation input");

    // Rule 4: composition settings.
    test.expect(neutralEdit([](Project& project) {
                    auto& composition = mainComposition(project);
                    composition.setName("Renamed composition");
                    composition.setWorkArea(WorkArea{core::RationalTime::fromInteger(1),
                                                     core::RationalTime::fromInteger(3)});
                    composition.setSafeAreas(SafeAreaSettings{.action = 0.8, .title = 0.7});
                    composition.setBackgroundColor({0.2, 0.3, 0.4, 1.0});
                }),
                "composition name, work area, safe areas and background are presentation");
    test.expect(!neutralEdit([](Project& project) {
        static_cast<void>(
            mainComposition(project).setDuration(core::RationalTime::fromInteger(20)));
    }),
                "composition duration is an evaluation input");

    // Parameters and topology are evaluation inputs.
    test.expect(!neutralEdit([](Project& project) {
        static_cast<void>(
            mainComposition(project).parameters().setSource(kOpacityId, ConstantValueSource{0.5}));
    }),
                "a parameter value is an evaluation input");
    test.expect(!neutralEdit([](Project& project) {
        static_cast<void>(mainComposition(project).graph().layerStack().erase(kSecondSlotId));
    }),
                "a stack entry is an evaluation input");

    test.expect(nodeMuted(NodeLayout{{kFirstLayerNodeId, NodeLayoutRecord{.muted = true}}},
                          kFirstLayerNodeId) &&
                    !nodeMuted(NodeLayout{}, kFirstLayerNodeId),
                "nodeMuted reads the one evaluated layout field");
}

struct Fixture {
    Document document{makeProject()};
    CommandStack stack{document};

    template <typename Op, typename... Args> CommandResult run(Args&&... args) {
        Transaction transaction("Edit", document.snapshot().revision());
        transaction.emplace<Op>(std::forward<Args>(args)...);
        return stack.execute(std::move(transaction));
    }
};

// The edit, its undo and its redo are all applied and all render-neutral.
void expectNeutralRoundTrip(TestContext& test, Fixture& fixture, const CommandResult& result,
                            const std::string_view what) {
    test.expect(result.changed() && !result.renderAffecting && !result.affectedTimes.has_value(),
                what);
    const auto undo = fixture.stack.undo();
    test.expect(undo.changed() && !undo.renderAffecting, "undo of a presentation edit is neutral");
    const auto redo = fixture.stack.redo();
    test.expect(redo.changed() && !redo.renderAffecting, "redo of a presentation edit is neutral");
}

void expectAffecting(TestContext& test, Fixture& fixture, const CommandResult& result,
                     const std::string_view what) {
    test.expect(result.changed() && result.renderAffecting, what);
    const auto undo = fixture.stack.undo();
    test.expect(undo.changed() && undo.renderAffecting, "undo of an evaluation edit invalidates");
    const auto redo = fixture.stack.redo();
    test.expect(redo.changed() && redo.renderAffecting, "redo of an evaluation edit invalidates");
}

void testPresentationCommands(TestContext& test) {
    Fixture fixture;
    // Operations that never declared themselves render-neutral, proven so from the document.
    expectNeutralRoundTrip(test, fixture,
                           fixture.run<RenameLayer>(kCompositionId, kFirstLayerId, "Hero"),
                           "renaming a layer (its node's display name) is render-neutral");
    expectNeutralRoundTrip(test, fixture,
                           fixture.run<SetLayerLabelColor>(kCompositionId, kFirstLayerId,
                                                           std::array<std::uint8_t, 3>{1, 2, 3}),
                           "a layer label color is render-neutral");
    expectNeutralRoundTrip(test, fixture,
                           fixture.run<SetLayerLocked>(kCompositionId, kSecondLayerId, true),
                           "locking a layer is render-neutral");
    expectNeutralRoundTrip(test, fixture,
                           fixture.run<SetCompositionName>(kCompositionId, "Renamed"),
                           "renaming a composition is render-neutral");
    expectNeutralRoundTrip(test, fixture,
                           fixture.run<SetCompositionSafeAreas>(
                               kCompositionId, SafeAreaSettings{.action = 0.85, .title = 0.75}),
                           "safe areas are render-neutral");
    expectNeutralRoundTrip(test, fixture, fixture.run<SetProjectName>("Renamed project"),
                           "renaming the project is render-neutral");

    // Operations that declare it, now also verified.
    expectNeutralRoundTrip(
        test, fixture,
        fixture.run<MoveNodes>(kCompositionId,
                               std::map<NodeId, Vec2d>{{kFirstLayerNodeId, Vec2d{70, 80}}}),
        "moving a node is render-neutral");
    expectNeutralRoundTrip(test, fixture,
                           fixture.run<SetNodeCollapsed>(kCompositionId, kFirstLayerNodeId, true),
                           "collapsing a node is render-neutral");
    const auto grouped =
        fixture.run<GroupNodes>(kCompositionId, std::set<NodeId>{kFirstLayerNodeId});
    const auto groupId = grouped.outputId<NodeGroupId>(kGroupNodesOutput);
    expectNeutralRoundTrip(test, fixture, grouped, "framing nodes in a group is render-neutral");
    test.expect(groupId.has_value(), "the group exists after redo");
    if (groupId.has_value())
        expectNeutralRoundTrip(test, fixture,
                               fixture.run<RenameGroup>(kCompositionId, *groupId, "Renamed frame"),
                               "renaming a group is render-neutral");

    // A presentation edit never hides an evaluation edit in the same transaction.
    {
        Transaction mixed("Rename and fade", fixture.document.snapshot().revision());
        mixed.emplace<RenameLayer>(kCompositionId, kFirstLayerId, "Faded");
        mixed.emplace<SetParameterSource>(kCompositionId, kOpacityId,
                                          ParameterSource{ConstantValueSource{0.4}});
        const auto result = fixture.stack.execute(std::move(mixed));
        test.expect(result.changed() && result.renderAffecting,
                    "a rename beside a parameter edit still invalidates");
    }
}

void testEvaluationCommands(TestContext& test) {
    Fixture fixture;
    expectAffecting(test, fixture,
                    fixture.run<SetParameterSource>(kCompositionId, kOpacityId,
                                                    ParameterSource{ConstantValueSource{0.3}}),
                    "a parameter edit invalidates");
    expectAffecting(test, fixture,
                    fixture.run<SetLayerEnabled>(kCompositionId, kFirstLayerId, false),
                    "disabling a layer invalidates");
    expectAffecting(test, fixture,
                    fixture.run<SetNodeMuted>(kCompositionId, kFirstLayerNodeId, true),
                    "muting a node invalidates");
    const LayerStackInputRef firstSlot{kLayerStackNodeId, kFirstSlotId,
                                       std::string(kLayerStackContentInputRole)};
    expectAffecting(test, fixture, fixture.run<DisconnectInput>(kCompositionId, firstSlot),
                    "disconnecting an input invalidates");
    test.expect(fixture.stack.undo().changed(), "the connection is restored");
    expectAffecting(
        test, fixture,
        fixture.run<SetCompositionDuration>(kCompositionId, core::RationalTime::fromInteger(12)),
        "a composition duration edit invalidates");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testClassification(test);
        bloom::commands::test::testPresentationCommands(test);
        bloom::commands::test::testEvaluationCommands(test);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return test.failures() == 0 ? 0 : 1;
}
