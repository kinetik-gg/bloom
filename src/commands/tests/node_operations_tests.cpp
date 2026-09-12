#include "command_test_support.hpp"

#include <bloom/document/animation.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace bloom::commands::test {
namespace {
using namespace document;

bool sameTruth(const Snapshot& left, const Snapshot& right) {
    if (left.project().id() != right.project().id() ||
        left.project().name() != right.project().name() ||
        !std::ranges::equal(left.project().extensionRecords(),
                            right.project().extensionRecords()) ||
        left.project().compositions().size() != right.project().compositions().size())
        return false;
    for (const auto& a : left.project().compositions()) {
        const auto* b = right.project().findComposition(a.id());
        if (!b || a.name() != b->name() || a.duration() != b->duration() ||
            a.format() != b->format() || a.nodeLayout() != b->nodeLayout() ||
            !std::ranges::equal(a.parameters().records(), b->parameters().records()) ||
            !std::ranges::equal(a.animationCurves().records(), b->animationCurves().records()) ||
            !std::ranges::equal(a.graph().nodes(), b->graph().nodes()) ||
            !std::ranges::equal(a.graph().edges(), b->graph().edges()) ||
            !std::ranges::equal(a.graph().layerOutputs(), b->graph().layerOutputs()) ||
            !std::ranges::equal(a.graph().layerStack().entries(),
                                b->graph().layerStack().entries()) ||
            a.graph().layerStack().nodeId() != b->graph().layerStack().nodeId() ||
            a.graph().compositionOutput() != b->graph().compositionOutput())
            return false;
    }
    return true;
}

struct Fixture {
    Document document{makeProject()};
    CommandStack stack{document};
};

template <typename Op, typename... Args> CommandResult apply(Fixture& fixture, Args&&... args) {
    Transaction transaction("Node edit", fixture.document.snapshot().revision());
    transaction.emplace<Op>(kCompositionId, std::forward<Args>(args)...);
    return fixture.stack.execute(std::move(transaction));
}

template <typename Op, typename... Args>
CommandResult exercise(TestContext& test, Fixture& fixture, Args&&... args) {
    const auto before = fixture.document.snapshot();
    const auto historySize = fixture.stack.size();
    const auto result = apply<Op>(fixture, std::forward<Args>(args)...);
    const auto after = fixture.document.snapshot();
    test.expect(result.status == CommandStatus::Succeeded &&
                    fixture.stack.size() == historySize + 1,
                "one authoring operation creates exactly one transaction/history entry");
    if (!result.changed())
        return result;
    test.expect(after.revision().value() == before.revision().value() + 1 &&
                    fixture.stack.trackedRevision() == after.revision(),
                "execute revision parity");
    test.expect(fixture.stack.undo().changed(), "node edit undoes once");
    const auto undone = fixture.document.snapshot();
    test.expect(sameTruth(undone, before) &&
                    undone.revision().value() == after.revision().value() + 1,
                "undo restores every exact record, edge, source, curve, order and layout at a "
                "fresh revision");
    test.expect(undone.ids().highWater() == after.ids().highWater(),
                "undo retains issued IDs under the allocator monotonicity contract");
    test.expect(fixture.stack.redo().changed(), "node edit redoes once");
    test.expect(sameTruth(fixture.document.snapshot(), after) &&
                    fixture.document.snapshot().revision().value() ==
                        undone.revision().value() + 1 &&
                    fixture.document.snapshot().ids().highWater() == after.ids().highWater(),
                "redo restores pinned IDs and exact snapshot truth without rerunning allocation");
    return result;
}

template <typename Op, typename... Args>
void refuse(TestContext& test, Fixture& fixture, const OperationIssueCode code, Args&&... args) {
    const auto before = fixture.document.snapshot();
    const auto history = fixture.stack.size();
    Transaction transaction("Atomic refusal");
    transaction.emplace<SetProjectName>("Must roll back");
    transaction.emplace<Op>(kCompositionId, std::forward<Args>(args)...);
    const auto result = fixture.stack.execute(std::move(transaction));
    test.expect(result.status == CommandStatus::Rejected && !result.operationFailures.empty() &&
                    result.operationFailures.front().operationIndex == 1 &&
                    result.operationFailures.front().issue.code == code,
                "invalid node operation reports its typed refusal");
    const auto after = fixture.document.snapshot();
    test.expect(sameTruth(after, before) && after.revision() == before.revision() &&
                    after.ids().highWater() == before.ids().highWater() &&
                    fixture.stack.size() == history && result.outputs.empty(),
                "a refusal rolls back all draft edits, IDs, revision and history atomically");
}

NodeId addSource(Fixture& fixture) {
    const auto result = apply<AddNode>(fixture, std::string(kSolidSourceNodeType), Vec2d{4, 5});
    const auto id = result.outputId<NodeId>(kAddNodeOutput);
    if (!(id.has_value()))
        throw std::logic_error("source fixture adds");
    return *id;
}

void testAddAndLayout(TestContext& test) {
    Fixture fixture;
    for (const auto& definition : builtInNodeDefinitions().definitions()) {
        const auto beforeSlots =
            composition(fixture.document.snapshot()).graph().layerStack().entries().size();
        const auto result = exercise<AddNode>(test, fixture, definition.key.typeId, Vec2d{17, -31});
        const auto id = result.outputId<NodeId>(kAddNodeOutput);
        if (!(id.has_value()))
            throw std::logic_error("node ID returned");
        const auto& comp = composition(fixture.document.snapshot());
        const auto* node = comp.graph().findNode(*id);
        test.expect(node && node->parameters.size() == definition.parameters.size() &&
                        node->schemaVersion == definition.key.schemaVersion &&
                        comp.nodeLayout().at(*id).position == Vec2d{17, -31} &&
                        comp.graph().layerStack().entries().size() == beforeSlots,
                    "generic AddNode uses schema defaults and never creates a layer or slot");
        for (const auto& parameter : definition.parameters) {
            const auto parameterId = result.outputId<ParameterId>("parameter." + parameter.role);
            const auto* record = parameterId ? comp.parameters().find(*parameterId) : nullptr;
            test.expect(record && record->schemaKey == parameter.schemaKey &&
                            record->source ==
                                ParameterSource{ConstantValueSource{parameter.defaultValue}},
                        "registry default is copied exactly into an independent parameter");
        }
    }
    const auto id = addSource(fixture);
    exercise<MoveNodes>(test, fixture,
                        std::map<NodeId, Vec2d>{{id, {12, 34}}, {kFirstLayerNodeId, {-6, 9}}});
    exercise<SetNodeCollapsed>(test, fixture, id, true);
    exercise<SetNodeMuted>(test, fixture, id, true);
    exercise<SetNodeWidth>(test, fixture, id, 239.25);
    const auto& layout = composition(fixture.document.snapshot()).nodeLayout().at(id);
    test.expect(layout.position == Vec2d{12, 34} && layout.collapsed && layout.muted &&
                    layout.width == 239.25,
                "layout fields change independently");
    const auto before = fixture.document.snapshot();
    test.expect(apply<SetNodeCollapsed>(fixture, id, true).status == CommandStatus::NoChange &&
                    apply<SetNodeMuted>(fixture, id, true).status == CommandStatus::NoChange &&
                    apply<SetNodeWidth>(fixture, id, 239.25).status == CommandStatus::NoChange &&
                    apply<MoveNodes>(fixture, std::map<NodeId, Vec2d>{{id, {12, 34}}}).status ==
                        CommandStatus::NoChange &&
                    fixture.document.snapshot().revision() == before.revision(),
                "identical layout edits do not dirty or advance revision");
    refuse<AddNode>(test, fixture, OperationIssueCode::Unsupported, "test.missing", Vec2d{});
    refuse<AddNode>(test, fixture, OperationIssueCode::InvalidValue,
                    std::string(kSolidSourceNodeType),
                    Vec2d{std::numeric_limits<double>::infinity(), 0});
    const auto missing = NodeId::fromRaw(99999);
    refuse<MoveNodes>(test, fixture, OperationIssueCode::InvalidTarget,
                      std::map<NodeId, Vec2d>{{id, {1, 2}}, {missing, {0, 0}}});
    refuse<MoveNodes>(test, fixture, OperationIssueCode::InvalidValue,
                      std::map<NodeId, Vec2d>{{id, {0, std::numeric_limits<double>::quiet_NaN()}}});
    refuse<SetNodeCollapsed>(test, fixture, OperationIssueCode::InvalidTarget, missing, true);
    refuse<SetNodeMuted>(test, fixture, OperationIssueCode::InvalidTarget, missing, true);
    refuse<SetNodeWidth>(test, fixture, OperationIssueCode::InvalidTarget, missing, 100);
    for (const auto width : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()})
        refuse<SetNodeWidth>(test, fixture, OperationIssueCode::InvalidValue, id, width);
}

void testWiringAndRename(TestContext& test) {
    Fixture fixture;
    const auto source = addSource(fixture);
    const auto replacement = addSource(fixture);
    const InputPortRef firstInput = NodeInputRef{kFirstLayerNodeId, "image"};
    exercise<ConnectPorts>(test, fixture, OutputPortRef{source, "image"}, firstInput);
    const auto initialEdge = composition(fixture.document.snapshot()).graph().edges().back().id;
    exercise<ConnectPorts>(test, fixture, OutputPortRef{replacement, "image"}, firstInput);
    const auto edges = composition(fixture.document.snapshot()).graph().edges();
    test.expect(std::ranges::count(edges, firstInput, &EdgeRecord::destination) == 1 &&
                    std::ranges::any_of(edges,
                                        [&](const auto& edge) {
                                            return edge.id == initialEdge &&
                                                   edge.source.nodeId == replacement;
                                        }),
                "rewire replaces one input edge and retains its stable identity");
    test.expect(
        apply<ConnectPorts>(fixture, OutputPortRef{replacement, "image"}, firstInput).status ==
            CommandStatus::NoChange,
        "identical connect is a no-op");
    exercise<DisconnectInput>(test, fixture, firstInput);
    test.expect(apply<DisconnectInput>(fixture, firstInput).status == CommandStatus::NoChange,
                "already-disconnected input is a no-op");
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::InvalidTarget,
                         OutputPortRef{source, "absent"}, firstInput);
    refuse<DisconnectInput>(test, fixture, OperationIssueCode::InvalidTarget,
                            InputPortRef{NodeInputRef{source, "image"}});
    const InputPortRef slotInput = LayerStackInputRef{kLayerStackNodeId, kFirstSlotId, "content"};
    refuse<DisconnectInput>(test, fixture, OperationIssueCode::InvalidValue, slotInput);
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::InvalidValue,
                         OutputPortRef{source, "image"}, slotInput);
    exercise<ConnectPorts>(test, fixture, OutputPortRef{kFirstLayerNodeId, "image"},
                           InputPortRef{NodeInputRef{kSecondLayerNodeId, "image"}});
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::GraphCycle,
                         OutputPortRef{kSecondLayerNodeId, "image"}, firstInput);
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::GraphCycle,
                         OutputPortRef{kFirstLayerNodeId, "image"}, firstInput);
    NodeDefinitionRegistry registry;
    if (!(registerBuiltInNodeDefinitions(registry)))
        throw std::logic_error("kind registry");
    if (!(registry.registerDefinition({{"test.scalar", 1},
                                       NodeLoweringKind::Unsupported,
                                       {},
                                       {{"value", SocketValueKind::Scalar}},
                                       {},
                                       std::nullopt}) == NodeRegistrationStatus::Registered))
        throw std::logic_error("scalar schema");
    registry.freeze();
    const auto scalar =
        apply<AddNode>(fixture, "test.scalar", Vec2d{}, registry).outputId<NodeId>(kAddNodeOutput);
    if (!(scalar.has_value()))
        throw std::logic_error("scalar source");
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::SocketKindMismatch,
                         OutputPortRef{*scalar, "value"}, firstInput, registry);
    exercise<RenameLayer>(test, fixture, kFirstLayerId, "Hero copy");
    test.expect(composition(fixture.document.snapshot()).graph().layerOutputs().front().name ==
                    "Hero copy",
                "rename changes the owning layer boundary");
    test.expect(apply<RenameLayer>(fixture, kFirstLayerId, "Hero copy").status ==
                    CommandStatus::NoChange,
                "same layer name is a no-op");
    refuse<RenameLayer>(test, fixture, OperationIssueCode::InvalidValue, kFirstLayerId, "");
    refuse<RenameLayer>(test, fixture, OperationIssueCode::InvalidTarget, LayerId::fromRaw(999),
                        "Missing");
}

void testRemoveAndDissolve(TestContext& test) {
    Fixture fixture;
    const auto source = addSource(fixture);
    const auto loose = apply<AddNode>(fixture, std::string(kLayerOutputNodeType), Vec2d{})
                           .outputId<NodeId>(kAddNodeOutput);
    if (!(loose.has_value()))
        throw std::logic_error("loose boundary node");
    if (!(apply<ConnectPorts>(fixture, OutputPortRef{source, "image"},
                              InputPortRef{NodeInputRef{*loose, "image"}})
              .changed()))
        throw std::logic_error("connect dissolve input");
    for (const auto target : {kFirstLayerNodeId, kSecondLayerNodeId})
        if (!(apply<ConnectPorts>(fixture, OutputPortRef{*loose, "image"},
                                  InputPortRef{NodeInputRef{target, "image"}})
                  .changed()))
            throw std::logic_error("connect dissolve consumer");
    const auto count = composition(fixture.document.snapshot()).parameters().records().size();
    exercise<DissolveNode>(test, fixture, *loose);
    const auto& dissolved = composition(fixture.document.snapshot());
    test.expect(!dissolved.graph().findNode(*loose) && !dissolved.nodeLayout().contains(*loose) &&
                    dissolved.parameters().records().size() == count - 2 &&
                    std::ranges::count_if(
                        dissolved.graph().edges(),
                        [&](const auto& edge) { return edge.source.nodeId == source; }) == 2,
                "dissolve removes orphan parameters/layout and reconnects every consumer");
    refuse<DissolveNode>(test, fixture, OperationIssueCode::Unsupported, source);
    refuse<DissolveNode>(test, fixture, OperationIssueCode::Unsupported, kFirstLayerNodeId);
    refuse<DissolveNode>(test, fixture, OperationIssueCode::Unsupported, kLayerStackNodeId);
    refuse<DissolveNode>(test, fixture, OperationIssueCode::InvalidTarget, NodeId::fromRaw(999));
    refuse<RemoveNodes>(test, fixture, OperationIssueCode::Unsupported,
                        std::set<NodeId>{source, kLayerStackNodeId});
    refuse<RemoveNodes>(test, fixture, OperationIssueCode::InvalidTarget,
                        std::set<NodeId>{source, NodeId::fromRaw(999)});
    const auto compositionOutput =
        apply<AddNode>(fixture, std::string(kCompositionOutputNodeType), Vec2d{})
            .outputId<NodeId>(kAddNodeOutput);
    if (!(compositionOutput.has_value()))
        throw std::logic_error("output fixture");
    refuse<RemoveNodes>(test, fixture, OperationIssueCode::Unsupported,
                        std::set<NodeId>{*compositionOutput});
    exercise<RemoveNodes>(test, fixture, std::set<NodeId>{kFirstLayerNodeId});
    const auto& removed = composition(fixture.document.snapshot());
    test.expect(
        removed.graph().findNode(source) && removed.graph().layerStack().entries().size() == 1 &&
            removed.graph().layerStack().entries().front().slotId == kSecondSlotId &&
            removed.graph().layerOutputs().size() == 1,
        "removing a Layer Output drops its boundary/slot and preserves shared upstream nodes");
    exercise<RemoveNodes>(test, fixture, std::set<NodeId>{source});
    test.expect(apply<RemoveNodes>(fixture, std::set<NodeId>{}).status == CommandStatus::NoChange,
                "empty removal is a no-op");
}

void testDeepDuplication(TestContext& test) {
    Fixture fixture;
    const auto source = addSource(fixture);
    if (!(apply<ConnectPorts>(fixture, OutputPortRef{source, "image"},
                              InputPortRef{NodeInputRef{kFirstLayerNodeId, "image"}})
              .changed()))
        throw std::logic_error("duplicate internal edge");
    if (!(apply<CreateAnimationForParameter>(fixture, kOpacityId, bloom::core::RationalTime{})
              .changed()))
        throw std::logic_error("animated duplicate fixture");
    if (!(apply<SetNodeWidth>(fixture, source, 250.0).changed() &&
          apply<SetNodeCollapsed>(fixture, source, true).changed() &&
          apply<SetNodeMuted>(fixture, source, true).changed()))
        throw std::logic_error("duplicate layout fixture");
    const auto original = fixture.document.snapshot();
    const auto result = exercise<DuplicateNodes>(
        test, fixture, std::set<NodeId>{source, kFirstLayerNodeId}, Vec2d{20, -10});
    const auto copySource = result.outputId<NodeId>("node." + std::to_string(source.value()));
    const auto copyLayer =
        result.outputId<LayerId>("layer." + std::to_string(kFirstLayerId.value()));
    const auto copyBoundary =
        result.outputId<NodeId>("node." + std::to_string(kFirstLayerNodeId.value()));
    const auto copyOpacity =
        result.outputId<ParameterId>("parameter." + std::to_string(kOpacityId.value()));
    if (!(copySource && copyLayer && copyBoundary && copyOpacity))
        throw std::logic_error("duplicate returns original-to-copy identities");
    const auto& comp = composition(fixture.document.snapshot());
    const auto slots = comp.graph().layerStack().entries();
    test.expect(slots.size() == 3 && slots[0].layerId == kFirstLayerId &&
                    slots[1].layerId == *copyLayer && slots[2].layerId == kSecondLayerId,
                "duplicate slot follows original and preserves surrounding order");
    const auto boundaries = comp.graph().layerOutputs();
    test.expect(std::ranges::any_of(boundaries,
                                    [&](const auto& boundary) {
                                        return boundary.layerId == *copyLayer &&
                                               boundary.name == "First copy";
                                    }),
                "duplicate layer name and LayerId");
    auto expectedLayout = composition(original).nodeLayout().at(source);
    expectedLayout.position.x += 20;
    expectedLayout.position.y -= 10;
    test.expect(comp.nodeLayout().at(*copySource) == expectedLayout,
                "duplicate copies width/collapse/mute and applies position offset");
    const auto* oldOpacity = composition(original).parameters().find(kOpacityId);
    const auto* newOpacity = comp.parameters().find(*copyOpacity);
    if (!(oldOpacity && newOpacity))
        throw std::logic_error("opacity records");
    const auto oldCurve = std::get<AnimationCurveSource>(oldOpacity->source).curveId;
    const auto newCurve = std::get<AnimationCurveSource>(newOpacity->source).curveId;
    const auto* oldKeys = comp.animationCurves().findScalar(oldCurve);
    const auto* newKeys = comp.animationCurves().findScalar(newCurve);
    test.expect(oldCurve != newCurve && oldKeys && newKeys &&
                    oldKeys->keyframes.size() == newKeys->keyframes.size() &&
                    oldKeys->keyframes.front().id != newKeys->keyframes.front().id &&
                    oldKeys->keyframes.front().value == newKeys->keyframes.front().value,
                "curves and keyframes are deeply copied with new identities");
    test.expect(std::ranges::any_of(comp.graph().edges(),
                                    [&](const auto& edge) {
                                        return edge.source.nodeId == *copySource &&
                                               edge.destination == InputPortRef{NodeInputRef{
                                                                       *copyBoundary, "image"}};
                                    }),
                "internal edge joins duplicated nodes");
    const auto isolated = exercise<DuplicateNodes>(test, fixture, std::set<NodeId>{source}, Vec2d{})
                              .outputId<NodeId>("node." + std::to_string(source.value()));
    if (!(isolated.has_value()))
        throw std::logic_error("isolated copy");
    test.expect(
        std::ranges::none_of(composition(fixture.document.snapshot()).graph().edges(),
                             [&](const auto& edge) { return edge.source.nodeId == *isolated; }),
        "edges to unselected nodes are not copied");
    exercise<RemoveNodes>(test, fixture, std::set<NodeId>{*copyBoundary});
    test.expect(!composition(fixture.document.snapshot()).animationCurves().find(newCurve) &&
                    composition(fixture.document.snapshot()).animationCurves().find(oldCurve),
                "removing copied animation owner cleans only its orphan curve");
    refuse<DuplicateNodes>(test, fixture, OperationIssueCode::InvalidTarget,
                           std::set<NodeId>{NodeId::fromRaw(999)}, Vec2d{});
    refuse<DuplicateNodes>(test, fixture, OperationIssueCode::InvalidValue,
                           std::set<NodeId>{source},
                           Vec2d{std::numeric_limits<double>::infinity(), 0});
    test.expect(apply<DuplicateNodes>(fixture, std::set<NodeId>{}, Vec2d{}).status ==
                    CommandStatus::NoChange,
                "empty duplication is a no-op");
}
} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testAddAndLayout(test);
        bloom::commands::test::testWiringAndRename(test);
        bloom::commands::test::testRemoveAndDissolve(test);
        bloom::commands::test::testDeepDuplication(test);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return test.failures() == 0 ? 0 : 1;
}
