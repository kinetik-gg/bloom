#include "command_test_support.hpp"

#include "layer_lock.hpp"

#include <bloom/commands/layer_operations.hpp>
#include <bloom/document/value_nodes.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace bloom::commands::test {
namespace {
using namespace document;

struct Fixture {
    Document document{makeProject()};
    CommandStack stack{document};
};

template <typename Op, typename... Args> CommandResult apply(Fixture& fixture, Args&&... args) {
    Transaction transaction("Eligibility fixture", fixture.document.snapshot().revision());
    transaction.emplace<Op>(kCompositionId, std::forward<Args>(args)...);
    return fixture.stack.execute(std::move(transaction));
}

NodeId addNode(Fixture& fixture, const std::string_view type) {
    const auto id =
        apply<AddNode>(fixture, std::string(type), Vec2d{}).outputId<NodeId>(kAddNodeOutput);
    if (!id)
        throw std::logic_error("eligibility fixture node");
    return *id;
}

void connect(Fixture& fixture, const NodeId from, const InputPortRef& to) {
    if (!apply<ConnectPorts>(fixture, OutputPortRef{from, "image"}, to).changed())
        throw std::logic_error("eligibility fixture connect");
}

// What the command stack would accept: the operation applies, the draft validates, and no locked
// layer changes. This is the reference every predicate is compared against.
bool reference(const Snapshot& snapshot, const Operation& operation) {
    Document isolated(snapshot.project(), snapshot.ids().highWater());
    auto draft = isolated.draft(isolated.snapshot());
    const auto result = operation.apply(draft);
    if (result.status == OperationStatus::Rejected)
        return false;
    if (detail::changesLockedLayers(snapshot.project(), draft.project()))
        return false;
    return draft.validate().ok();
}

struct Checker {
    TestContext& test;
    const Snapshot snapshot;
    const Composition& comp;
    std::vector<std::string> failures;

    Checker(TestContext& testContext, const Fixture& fixture)
        : test(testContext), snapshot(fixture.document.snapshot()),
          comp(*snapshot.project().findComposition(kCompositionId)) {}

    void same(const bool predicate, const Operation& operation, const std::string_view what) {
        test.expect(predicate == reference(snapshot, operation),
                    std::string("predicate disagrees with command acceptance: ") +
                        std::string(what));
    }

    std::vector<NodeId> nodeIds() const {
        std::vector<NodeId> ids{NodeId::fromRaw(9999)};
        for (const auto& node : comp.graph().nodes())
            ids.push_back(node.id);
        return ids;
    }

    void run() {
        const auto ids = nodeIds();
        for (const auto id : ids) {
            const auto label = "node " + std::to_string(id.value());
            same(canRemoveNodes(comp, {id}), RemoveNodes(kCompositionId, {id}), "remove " + label);
            same(canDissolveNode(comp, id), DissolveNode(kCompositionId, id), "dissolve " + label);
            for (const bool value : {false, true}) {
                same(canSetNodeMuted(comp, id, value), SetNodeMuted(kCompositionId, id, value),
                     "mute " + label);
                same(canSetNodeCollapsed(comp, id, value),
                     SetNodeCollapsed(kCompositionId, id, value), "collapse " + label);
            }
            same(canGroupNodes(comp, snapshot.ids(), {id}, kDefaultNodeGroupName),
                 GroupNodes(kCompositionId, {id}, std::string(kDefaultNodeGroupName)),
                 "group " + label);
        }
        // Multi-selection: every pair, plus all nodes together.
        for (std::size_t a = 0; a < ids.size(); ++a)
            for (std::size_t b = a + 1; b < ids.size(); ++b) {
                const std::set<NodeId> pair{ids[a], ids[b]};
                same(canRemoveNodes(comp, pair), RemoveNodes(kCompositionId, pair), "remove pair");
                same(canGroupNodes(comp, snapshot.ids(), pair, kDefaultNodeGroupName),
                     GroupNodes(kCompositionId, pair, std::string(kDefaultNodeGroupName)),
                     "group pair");
            }
        const std::set<NodeId> all(ids.begin(), ids.end());
        same(canRemoveNodes(comp, all), RemoveNodes(kCompositionId, all), "remove all + missing");
        same(canRemoveNodes(comp, {}), RemoveNodes(kCompositionId, {}), "remove empty");
        same(canGroupNodes(comp, snapshot.ids(), {}, kDefaultNodeGroupName),
             GroupNodes(kCompositionId, {}, std::string(kDefaultNodeGroupName)), "group empty");
        same(canGroupNodes(comp, snapshot.ids(), {kFirstLayerNodeId}, ""),
             GroupNodes(kCompositionId, {kFirstLayerNodeId}, ""), "group empty name");
        for (const auto& layer : comp.graph().layerOutputs()) {
            same(canRenameLayer(comp, layer.layerId, layer.name),
                 RenameLayer(kCompositionId, layer.layerId, layer.name), "rename same");
            same(canRenameLayer(comp, layer.layerId, "Renamed"),
                 RenameLayer(kCompositionId, layer.layerId, "Renamed"), "rename new");
            same(canRenameLayer(comp, layer.layerId, ""),
                 RenameLayer(kCompositionId, layer.layerId, ""), "rename empty");
        }
        same(canRenameLayer(comp, LayerId::fromRaw(9999), "Renamed"),
             RenameLayer(kCompositionId, LayerId::fromRaw(9999), "Renamed"), "rename missing");
    }
};

void testBase(TestContext& test) {
    Fixture fixture;
    Checker(test, fixture).run();
    const Checker checker(test, fixture);
    test.expect(!canRemoveNodes(checker.comp, {kLayerStackNodeId}),
                "protected Merge cannot be removed");
    test.expect(!canDissolveNode(checker.comp, kFirstLayerNodeId),
                "a Layer Output without a connected image input cannot be dissolved");
    test.expect(canRemoveNodes(checker.comp, {kFirstLayerNodeId}), "a Layer Output can be removed");
    test.expect(!canRemoveNodes(checker.comp, {NodeId::fromRaw(9999)}), "missing node refused");
}

void testLinkedGraph(TestContext& test) {
    Fixture fixture;
    const auto source = addNode(fixture, kSolidSourceNodeType);
    const auto loose = addNode(fixture, kLayerOutputNodeType);
    const auto output = addNode(fixture, kCompositionOutputNodeType);
    (void)output;
    connect(fixture, source, NodeInputRef{loose, "image"});
    for (const auto target : {kFirstLayerNodeId, kSecondLayerNodeId}) {
        if (!apply<ConnectPorts>(fixture, OutputPortRef{loose, "image"},
                                 InputPortRef{NodeInputRef{target, "image"}})
                 .changed())
            throw std::logic_error("eligibility consumer");
    }
    (void)apply<SetNodeMuted>(fixture, source, true);
    (void)apply<SetNodeCollapsed>(fixture, loose, true);
    (void)apply<GroupNodes>(fixture, std::set<NodeId>{source, loose},
                            std::string(kDefaultNodeGroupName));
    Checker(test, fixture).run();
    const Checker checker(test, fixture);
    test.expect(canDissolveNode(checker.comp, loose), "connected pass-through dissolves");
    test.expect(!canDissolveNode(checker.comp, source), "a source without image input refuses");
    test.expect(canSetNodeMuted(checker.comp, source, true), "already-muted is NoChange, accepted");
}

void testDrivenParameter(TestContext& test) {
    Fixture fixture;
    const auto scalar = addNode(fixture, kScalarValueNodeType);
    if (!apply<ConnectPorts>(
             fixture, OutputPortRef{scalar, std::string(kValuePortName)},
             InputPortRef{NodeInputRef{kFirstLayerNodeId, std::string(kOpacityParameterRole)}})
             .changed())
        throw std::logic_error("eligibility driver");
    Checker(test, fixture).run();
    const Checker checker(test, fixture);
    test.expect(!canRemoveNodes(checker.comp, {scalar}),
                "removing the driver of a surviving parameter is refused");
    test.expect(canRemoveNodes(checker.comp, {scalar, kFirstLayerNodeId}),
                "removing driver and driven node together is accepted");
}

void testLocked(TestContext& test) {
    Fixture fixture;
    const auto source = addNode(fixture, kSolidSourceNodeType);
    const auto unrelated = addNode(fixture, kSolidSourceNodeType);
    (void)unrelated;
    connect(fixture, source, NodeInputRef{kFirstLayerNodeId, "image"});
    if (!apply<SetLayerLocked>(fixture, kFirstLayerId, true).changed())
        throw std::logic_error("eligibility lock");
    Checker(test, fixture).run();
    const Checker checker(test, fixture);
    test.expect(!canRemoveNodes(checker.comp, {source}), "upstream of a locked layer is protected");
    test.expect(!canRemoveNodes(checker.comp, {kFirstLayerNodeId}), "locked boundary is protected");
    test.expect(!canDissolveNode(checker.comp, source), "locked upstream node cannot dissolve");
    test.expect(canRemoveNodes(checker.comp, {kSecondLayerNodeId}), "unlocked layer still removes");
    test.expect(canRenameLayer(checker.comp, kFirstLayerId, "Locked rename"),
                "rename of a locked layer matches command acceptance");
}

void testNoValidationAndNoDocumentCopy(TestContext& test) {
    Fixture fixture;
    const auto source = addNode(fixture, kSolidSourceNodeType);
    connect(fixture, source, NodeInputRef{kFirstLayerNodeId, "image"});
    const auto snapshot = fixture.document.snapshot();
    const auto& comp = *snapshot.project().findComposition(kCompositionId);
    const auto validations = Project::validationCount();
    const auto lockBuilds = Composition::lockIndexBuilds();
    bool sink = false;
    for (int round = 0; round < 50; ++round)
        for (const auto& node : comp.graph().nodes()) {
            sink ^= canRemoveNodes(comp, {node.id});
            sink ^= canDissolveNode(comp, node.id);
            sink ^= canSetNodeMuted(comp, node.id, true);
            sink ^= canSetNodeCollapsed(comp, node.id, true);
            sink ^= canGroupNodes(comp, snapshot.ids(), {node.id}, kDefaultNodeGroupName);
        }
    for (const auto& layer : comp.graph().layerOutputs())
        sink ^= canRenameLayer(comp, layer.layerId, "Other");
    test.expect(Project::validationCount() == validations,
                "eligibility predicates perform no project validation");
    test.expect(Composition::lockIndexBuilds() == lockBuilds,
                "eligibility predicates answer lock queries from the published index");
    (void)sink;
}
} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testBase(test);
        bloom::commands::test::testLinkedGraph(test);
        bloom::commands::test::testDrivenParameter(test);
        bloom::commands::test::testLocked(test);
        bloom::commands::test::testNoValidationAndNoDocumentCopy(test);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return test.failures() == 0 ? 0 : 1;
}
