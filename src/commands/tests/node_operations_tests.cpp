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
            a.nodeGroups() != b->nodeGroups() ||
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

void testLayerToggles(TestContext& test) {
    Fixture fixture;
    (void)exercise<SetLayerEnabled>(test, fixture, kFirstLayerId, false);
    auto snapshot = fixture.document.snapshot();
    test.expect(snapshot.project().findComposition(kCompositionId)->nodeLayout().at(kFirstLayerNodeId).muted,
        "visibility synchronizes Layer node mute");
    (void)exercise<SetLayerSolo>(test, fixture, kFirstLayerId, true);
    (void)exercise<SetLayerLocked>(test, fixture, kFirstLayerId, true);
    refuse<SetLayerRange>(test, fixture, OperationIssueCode::InvalidValue, kFirstLayerId, core::RationalTime{}, core::RationalTime::fromInteger(2));
    refuse<MoveLayerBefore>(test, fixture, OperationIssueCode::InvalidValue, kFirstSlotId, std::optional<LayerSlotId>{});
    refuse<SetParameterSource>(test, fixture, OperationIssueCode::InvalidValue, kFirstPositionId, ConstantValueSource{Vec2d{9, 9}});
    (void)exercise<SetLayerLocked>(test, fixture, kFirstLayerId, false);
}

void testLayerRanges(TestContext& test) {
    Fixture fixture;
    const auto in = core::RationalTime::fromInteger(1), out = core::RationalTime::fromInteger(4);
    (void)exercise<SetLayerRange>(test, fixture, kFirstLayerId, in, out);
    refuse<SetLayerRange>(test, fixture, OperationIssueCode::InvalidValue, kFirstLayerId, out, in);
    refuse<SplitLayerAtTime>(test, fixture, OperationIssueCode::InvalidValue, kFirstLayerId, in);
    const auto split = exercise<SplitLayerAtTime>(test, fixture, kFirstLayerId, core::RationalTime::fromInteger(2));
    const auto copy = split.outputId<LayerId>("layer");
    const auto snapshot = fixture.document.snapshot();
    const auto& graph = snapshot.project().findComposition(kCompositionId)->graph();
    test.expect(copy && graph.findLayer(*copy)->inPoint == core::RationalTime::fromInteger(2) &&
        graph.findLayer(*copy)->outPoint == out && graph.findLayer(kFirstLayerId)->outPoint == core::RationalTime::fromInteger(2),
        "split keeps adjacent half-open ranges and exact undo/redo IDs");
}

void testValidityQuery(TestContext& test) {
    Fixture fixture;
    const auto before = fixture.document.snapshot();
    const auto history = fixture.stack.size();
    test.expect(canApplyNodeOperation(
                    before, DuplicateNodes(kCompositionId, {kFirstLayerNodeId}, {24, 24})),
                "menu query accepts a valid duplicate without issuing IDs");
    test.expect(!canApplyNodeOperation(before, RemoveNodes(kCompositionId, {kLayerStackNodeId})),
                "menu query refuses protected removal");
    test.expect(!canApplyNodeOperation(before, DissolveNode(kCompositionId, kFirstLayerNodeId)),
                "menu query refuses participating Layer Output dissolve");
    const auto after = fixture.document.snapshot();
    test.expect(sameTruth(before, after) && before.revision() == after.revision() &&
                    before.ids().highWater() == after.ids().highWater() &&
                    fixture.stack.size() == history,
                "queries leave all live truth, allocator, revision and history untouched");
}

void testAddAndLayout(TestContext& test) {
    Fixture fixture;
    for (const auto& definition : builtInNodeDefinitions().definitions()) {
        // Task S1, item 5: a OnePerComposition type is refused once the composition already holds
        // one. The fixture's composition carries a Layer Stack from the start, so that one refuses
        // immediately; the composition output is the same rule proved from the other side -- the
        // first is accepted below and a second is refused right after it.
        if (definition.cardinality == NodeCardinality::OnePerComposition &&
            std::ranges::any_of(
                composition(fixture.document.snapshot()).graph().nodes(),
                [&](const auto& existing) { return existing.typeId == definition.key.typeId; })) {
            refuse<AddNode>(test, fixture, OperationIssueCode::Unsupported, definition.key.typeId,
                            Vec2d{17, -31});
            continue;
        }
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
        if (definition.cardinality == NodeCardinality::OnePerComposition) {
            refuse<AddNode>(test, fixture, OperationIssueCode::Unsupported, definition.key.typeId,
                            Vec2d{17, -31});
        }
    }
    // The rule lives on the definition, so it is the definition that says which types are
    // singletons
    // -- the Layer Stack operator and the composition's one evaluation endpoint, and nothing else.
    for (const auto& definition : builtInNodeDefinitions().definitions()) {
        const bool singleton = definition.key.typeId == kLayerStackNodeType ||
                               definition.key.typeId == kCompositionOutputNodeType;
        test.expect((definition.cardinality == NodeCardinality::OnePerComposition) == singleton,
                    "exactly the Layer Stack and the composition output are one per composition: " +
                        definition.key.typeId);
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
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::InvalidValue,
                         OutputPortRef{source, "image"}, slotInput);
    // ADAPTED (task FIX1, item B): detaching a stack slot's content REMOVES the slot, and
    // connecting a Layer output to the sentinel slot creates one. The slot and the link into it are
    // one thing to the artist, so they are one thing here -- which is what makes a stack slot
    // detachable at all.
    exercise<DisconnectInput>(test, fixture, slotInput);
    test.expect(fixture.document.snapshot()
                        .project()
                        .findComposition(kCompositionId)
                        ->graph()
                        .layerStack()
                        .find(kFirstSlotId) == nullptr,
                "disconnecting a stack slot removes the slot");
    const auto before = fixture.document.snapshot()
                            .project()
                            .findComposition(kCompositionId)
                            ->graph()
                            .layerStack()
                            .entries()
                            .size() +
                        1;
    const InputPortRef newSlot = LayerStackInputRef{kLayerStackNodeId, LayerSlotId{}, "content"};
    exercise<ConnectPorts>(test, fixture, OutputPortRef{kFirstLayerNodeId, "image"}, newSlot);
    test.expect(fixture.document.snapshot()
                        .project()
                        .findComposition(kCompositionId)
                        ->graph()
                        .layerStack()
                        .entries()
                        .size() == before,
                "connecting a Layer output to Merge creates its stack slot");
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

// ADAPTED (task FIX1, item B): dissolving a PARTICIPATING Layer Output used to be refused, because
// the stack slot it fed could not be repaired. A slot is created by connecting a Layer output to
// Merge and removed by disconnecting it, so dissolve now simply takes the layer out of the stack --
// which is what the gesture means -- and its own fixture says so rather than one line buried in the
// refusals.
void testDissolveParticipatingLayer(TestContext& test) {
    Fixture fixture;
    const auto source = addSource(fixture);
    if (!(apply<ConnectPorts>(fixture, OutputPortRef{source, "image"},
                              InputPortRef{NodeInputRef{kFirstLayerNodeId, "image"}})
              .changed()))
        throw std::logic_error("connect participating layer input");
    const auto slotsBefore =
        composition(fixture.document.snapshot()).graph().layerStack().entries().size();
    exercise<DissolveNode>(test, fixture, kFirstLayerNodeId);
    const auto& after = composition(fixture.document.snapshot());
    test.expect(after.graph().findNode(kFirstLayerNodeId) == nullptr &&
                    after.graph().layerStack().entries().size() == slotsBefore - 1 &&
                    after.graph().layerOutputs().size() == 1,
                "dissolving a participating Layer Output takes its layer out of the stack");
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
                    // ADAPTED (task S4, then blend modes): a Layer Output node now owns six
                    // parameters, not two, so dissolving one orphans six.
                    dissolved.parameters().records().size() == count - 6 &&
                    std::ranges::count_if(
                        dissolved.graph().edges(),
                        [&](const auto& edge) { return edge.source.nodeId == source; }) == 2,
                "dissolve removes orphan parameters/layout and reconnects every consumer");
    refuse<DissolveNode>(test, fixture, OperationIssueCode::Unsupported, source);
    refuse<DissolveNode>(test, fixture, OperationIssueCode::Unsupported, kLayerStackNodeId);
    refuse<DissolveNode>(test, fixture, OperationIssueCode::InvalidTarget, NodeId::fromRaw(999));
    refuse<RemoveNodes>(test, fixture, OperationIssueCode::Unsupported,
                        std::set<NodeId>{source, kLayerStackNodeId});
    refuse<RemoveNodes>(test, fixture, OperationIssueCode::InvalidTarget,
                        std::set<NodeId>{source, NodeId::fromRaw(999)});
    const auto compositionOutput =
        apply<AddNode>(fixture, std::string(kCompositionOutputNodeType), Vec2d{})
            .outputId<NodeId>(kAddNodeOutput);
    // And a second one is refused, here as well as in testAddAndLayout: this is the fixture that
    // actually holds a composition output, so it is where the refusal is worth restating.
    refuse<AddNode>(test, fixture, OperationIssueCode::Unsupported,
                    std::string(kCompositionOutputNodeType), Vec2d{});
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
void testDuplicationOwnershipEdges(TestContext& test) {
    Fixture fixture;
    if (!apply<CreateAnimationForParameter>(fixture, kFirstPositionId, core::RationalTime{})
             .changed())
        throw std::logic_error("vec2 animation fixture");
    const auto original = fixture.document.snapshot();
    const auto result = exercise<DuplicateNodes>(
        test, fixture, std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId}, Vec2d{});
    const auto copiedPosition =
        result.outputId<ParameterId>("parameter." + std::to_string(kFirstPositionId.value()));
    if (!copiedPosition)
        throw std::logic_error("copied vec2 parameter");
    const auto& comp = composition(fixture.document.snapshot());
    const auto oldCurve = std::get<AnimationCurveSource>(
                              composition(original).parameters().find(kFirstPositionId)->source)
                              .curveId;
    const auto newCurve =
        std::get<AnimationCurveSource>(comp.parameters().find(*copiedPosition)->source).curveId;
    const auto* oldKeys = comp.animationCurves().findVec2(oldCurve);
    const auto* newKeys = comp.animationCurves().findVec2(newCurve);
    test.expect(oldCurve != newCurve && oldKeys && newKeys &&
                    oldKeys->keyframes.front().id != newKeys->keyframes.front().id &&
                    oldKeys->keyframes.front().value == newKeys->keyframes.front().value,
                "vec2 animation copies independently with fresh curve/key identities");
    const auto firstCopy =
        result.outputId<LayerId>("layer." + std::to_string(kFirstLayerId.value()));
    const auto secondCopy =
        result.outputId<LayerId>("layer." + std::to_string(kSecondLayerId.value()));
    const auto entries = comp.graph().layerStack().entries();
    test.expect(firstCopy && secondCopy && entries.size() == 4 &&
                    entries[0].layerId == kFirstLayerId && entries[1].layerId == *firstCopy &&
                    entries[2].layerId == kSecondLayerId && entries[3].layerId == *secondCopy,
                "multiple copied layer slots each follow their own original");
    refuse<DuplicateNodes>(test, fixture, OperationIssueCode::InvalidValue,
                           std::set<NodeId>{kFirstLayerNodeId, kLayerStackNodeId}, Vec2d{});
    if (!apply<MoveNodes>(
             fixture,
             std::map<NodeId, Vec2d>{{kFirstLayerNodeId, {std::numeric_limits<double>::max(), 0}}})
             .changed())
        throw std::logic_error("position overflow fixture");
    refuse<DuplicateNodes>(test, fixture, OperationIssueCode::InvalidValue,
                           std::set<NodeId>{kFirstLayerNodeId},
                           Vec2d{std::numeric_limits<double>::max(), 0});

    // ADAPTED (task S7): a driver binding names a value node's output now, so the fixture adds the
    // Scalar node the binding points at instead of allocating a bare driver id. The contract under
    // test is unchanged -- duplicating a node whose parameter is driven is still refused, because a
    // copy would need a second driver nothing asked for.
    auto before = fixture.document.snapshot();
    auto draft = fixture.document.draft(before);
    const auto valueNodeId = draft.ids().allocateNode();
    const auto valueParameterId = draft.ids().allocateParameter();
    auto* drivenComposition = draft.project().findComposition(kCompositionId);
    if (!valueNodeId || !valueParameterId || drivenComposition == nullptr ||
        !drivenComposition->parameters().insert(
            {*valueParameterId, std::string(bloom::document::kScalarValueParameterSchemaKey),
             ConstantValueSource{0.5}}) ||
        !drivenComposition->graph().addNode(
            {*valueNodeId,
             std::string(bloom::document::kScalarValueNodeType),
             {{std::string(bloom::document::kValueParameterRole), *valueParameterId}},
             bloom::document::kValueNodeSchemaVersion}) ||
        !drivenComposition->parameters().setSource(
            kSecondOpacityId,
            DriverBindingSource{*valueNodeId, std::string(bloom::document::kValuePortName)}) ||
        !fixture.document.commit(before.revision(), std::move(draft)).committed())
        throw std::logic_error("driver fixture");
    fixture.stack.clear();
    refuse<DuplicateNodes>(test, fixture, OperationIssueCode::Unsupported,
                           std::set<NodeId>{kSecondLayerNodeId}, Vec2d{});
}

// Node groups: one transaction each, undo/redo pinned by exercise() above, and the one rule that
// spans them all -- a node belongs to exactly one group, and a frame emptied by any of these goes.
// Task S7, items 2 and 3: a link into an operand socket is the parameter's driver binding, and the
// same ConnectPorts/DisconnectInput pair that wires image transport authors it. No second gesture,
// no second command, and undo restores the exact constant the link replaced.
void testParameterSocketDrivers(TestContext& test) {
    Fixture fixture;
    const auto parameterOf = [&fixture](const NodeId nodeId, const std::string_view role) {
        const auto snapshot = fixture.document.snapshot();
        const auto& graph = composition(snapshot).graph();
        const auto* node = graph.findNode(nodeId);
        if (node == nullptr)
            throw std::logic_error("driver fixture node");
        const auto binding = std::ranges::find(node->parameters, role, &ParameterBinding::role);
        if (binding == node->parameters.end())
            throw std::logic_error("driver fixture role");
        const auto* parameter = composition(snapshot).parameters().find(binding->parameterId);
        if (parameter == nullptr)
            throw std::logic_error("driver fixture parameter");
        return *parameter;
    };

    const auto scalarNode = apply<AddNode>(fixture, std::string(kScalarValueNodeType), Vec2d{8, 9})
                                .outputId<NodeId>(kAddNodeOutput);
    if (!scalarNode.has_value())
        throw std::logic_error("scalar value node fixture");

    const InputPortRef opacitySocket =
        NodeInputRef{kFirstLayerNodeId, std::string(kOpacityParameterRole)};
    test.expect(std::holds_alternative<ConstantValueSource>(
                    parameterOf(kFirstLayerNodeId, kOpacityParameterRole).source),
                "an unlinked operand socket's parameter is a constant");

    exercise<ConnectPorts>(test, fixture, OutputPortRef{*scalarNode, std::string(kValuePortName)},
                           opacitySocket);
    const auto linked = parameterOf(kFirstLayerNodeId, kOpacityParameterRole).source;
    test.expect(std::holds_alternative<DriverBindingSource>(linked) &&
                    std::get<DriverBindingSource>(linked) ==
                        DriverBindingSource{*scalarNode, std::string(kValuePortName)},
                "linking an operand socket sets its parameter's driver binding");
    test.expect(
        std::ranges::none_of(composition(fixture.document.snapshot()).graph().edges(),
                             [&](const auto& edge) { return edge.destination == opacitySocket; }),
        "and records no edge: one authored value has one durable source");
    test.expect(apply<ConnectPorts>(
                    fixture, OutputPortRef{*scalarNode, std::string(kValuePortName)}, opacitySocket)
                        .status == CommandStatus::NoChange,
                "relinking the same output is a no-op");

    exercise<DisconnectInput>(test, fixture, opacitySocket);
    test.expect(std::holds_alternative<ConstantValueSource>(
                    parameterOf(kFirstLayerNodeId, kOpacityParameterRole).source),
                "unlinking restores a constant source");
    test.expect(apply<DisconnectInput>(fixture, opacitySocket).status == CommandStatus::NoChange,
                "an already-unlinked operand socket is a no-op");

    // The kind rule is the shared promotion whitelist: an Integer node drives a Scalar operand
    // through an explicit widening, and a String node drives nothing numeric at all.
    const auto integerNode =
        apply<AddNode>(fixture, std::string(kIntegerValueNodeType), Vec2d{1, 2})
            .outputId<NodeId>(kAddNodeOutput);
    const auto stringNode = apply<AddNode>(fixture, std::string(kStringValueNodeType), Vec2d{3, 4})
                                .outputId<NodeId>(kAddNodeOutput);
    if (!integerNode.has_value() || !stringNode.has_value())
        throw std::logic_error("promotion fixture nodes");
    exercise<ConnectPorts>(test, fixture, OutputPortRef{*integerNode, std::string(kValuePortName)},
                           opacitySocket);
    refuse<ConnectPorts>(test, fixture, OperationIssueCode::SocketKindMismatch,
                         OutputPortRef{*stringNode, std::string(kValuePortName)}, opacitySocket);
    // Nothing promotes INTO an Integer socket from a Scalar: the blend mode is a closed
    // enumeration, and a number between two modes is not a mode.
    refuse<ConnectPorts>(
        test, fixture, OperationIssueCode::SocketKindMismatch,
        OutputPortRef{*scalarNode, std::string(kValuePortName)},
        InputPortRef{NodeInputRef{kFirstLayerNodeId, std::string(kBlendModeParameterRole)}});
}

void testNodeGroups(TestContext& test) {
    Fixture fixture;
    const auto composition = [&fixture] {
        return fixture.document.snapshot().project().findComposition(kCompositionId);
    };
    const auto groups = [&fixture]() -> const NodeGroups& {
        return fixture.document.snapshot().project().findComposition(kCompositionId)->nodeGroups();
    };

    const auto created = exercise<GroupNodes>(
        test, fixture, std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId});
    const auto first = created.outputId<NodeGroupId>(kGroupNodesOutput);
    test.expect(first.has_value(), "GroupNodes reports the group it created");
    if (!first)
        return;
    test.expect(groups().size() == 1 &&
                    groups().at(*first).members ==
                        std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId} &&
                    groups().at(*first).name == kDefaultNodeGroupName,
                "a new group holds exactly the nodes it was given, under the default name");
    test.expect(groups().at(*first).padding.x == kDefaultNodeGroupPadding &&
                    groups().at(*first).padding.y == kDefaultNodeGroupPadding,
                "a new group takes the frozen default padding");

    // A second group over one of the first group's members takes it away from the first.
    const auto second = exercise<GroupNodes>(test, fixture, std::set<NodeId>{kSecondLayerNodeId},
                                             std::string("Lighting"))
                            .outputId<NodeGroupId>(kGroupNodesOutput);
    test.expect(second.has_value(), "a second group is created");
    if (!second)
        return;
    test.expect(groups().size() == 2 &&
                    groups().at(*first).members == std::set<NodeId>{kFirstLayerNodeId} &&
                    groups().at(*second).members == std::set<NodeId>{kSecondLayerNodeId},
                "members leave their previous group");

    // Renaming: one applies, an identical name is a no-op that writes nothing, and an empty name is
    // refused exactly as a layer name is.
    (void)exercise<RenameGroup>(test, fixture, *second, std::string("Key Light"));
    test.expect(groups().at(*second).name == "Key Light", "RenameGroup renames in place");
    const auto before = fixture.document.snapshot();
    const auto unchanged = apply<RenameGroup>(fixture, *second, std::string("Key Light"));
    test.expect(!unchanged.changed() && fixture.document.snapshot().revision() == before.revision(),
                "renaming a group to its own name changes nothing");
    refuse<RenameGroup>(test, fixture, OperationIssueCode::InvalidValue, *second, std::string{});
    refuse<GroupNodes>(test, fixture, OperationIssueCode::InvalidValue,
                       std::set<NodeId>{kFirstLayerNodeId}, std::string{});
    refuse<GroupNodes>(test, fixture, OperationIssueCode::InvalidValue, std::set<NodeId>{});
    refuse<GroupNodes>(test, fixture, OperationIssueCode::InvalidTarget,
                       std::set<NodeId>{NodeId::fromRaw(9999)});
    refuse<RenameGroup>(test, fixture, OperationIssueCode::InvalidTarget,
                        NodeGroupId::fromRaw(9999), std::string("Nowhere"));
    refuse<UngroupNodes>(test, fixture, OperationIssueCode::InvalidTarget,
                         NodeGroupId::fromRaw(9999));

    // Membership by command: the second frame takes both nodes, which empties the first one.
    (void)exercise<SetGroupMembers>(test, fixture, *second,
                                    std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId});
    test.expect(groups().size() == 1 && !groups().contains(*first) &&
                    groups().at(*second).members ==
                        std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId},
                "a group emptied by another group's membership change is removed");
    refuse<SetGroupMembers>(test, fixture, OperationIssueCode::InvalidTarget, *second,
                            std::set<NodeId>{NodeId::fromRaw(9999)});

    // A move that carries a membership delta is one transaction: exercise() proves the single undo.
    (void)exercise<MoveNodes>(test, fixture,
                              std::map<NodeId, Vec2d>{{kFirstLayerNodeId, Vec2d{111, 222}}},
                              NodeGroupMembershipDelta{{kFirstLayerNodeId, std::nullopt}});
    test.expect(composition()->nodeLayout().at(kFirstLayerNodeId).position == Vec2d{111, 222} &&
                    groups().at(*second).members == std::set<NodeId>{kSecondLayerNodeId},
                "a drag out of a frame moves the card and drops its membership together");
    (void)exercise<MoveNodes>(test, fixture,
                              std::map<NodeId, Vec2d>{{kFirstLayerNodeId, Vec2d{5, 6}}},
                              NodeGroupMembershipDelta{{kFirstLayerNodeId, *second}});
    test.expect(groups().at(*second).members ==
                    std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId},
                "a drop inside a frame adds the card in the same transaction");
    refuse<MoveNodes>(test, fixture, OperationIssueCode::InvalidTarget,
                      std::map<NodeId, Vec2d>{{kFirstLayerNodeId, Vec2d{1, 1}}},
                      NodeGroupMembershipDelta{{kFirstLayerNodeId, NodeGroupId::fromRaw(9999)}});

    // Ungrouping keeps every node and every layout record; only the frame goes.
    const auto layoutBefore = composition()->nodeLayout();
    (void)exercise<UngroupNodes>(test, fixture, *second);
    test.expect(groups().empty() && composition()->nodeLayout() == layoutBefore &&
                    composition()->graph().findNode(kFirstLayerNodeId) != nullptr,
                "ungrouping removes only the frame");

    // Removing a node empties the frame that held it, and the frame goes with it.
    const auto solid = addSource(fixture);
    const auto lone = apply<GroupNodes>(fixture, std::set<NodeId>{solid})
                          .outputId<NodeGroupId>(kGroupNodesOutput);
    test.expect(lone.has_value() && groups().size() == 1, "a one-node group exists");
    (void)exercise<RemoveNodes>(test, fixture, std::set<NodeId>{solid});
    test.expect(groups().empty(), "removing a group's last node removes the group");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testLayerToggles(test);
        bloom::commands::test::testLayerRanges(test);
        bloom::commands::test::testValidityQuery(test);
        bloom::commands::test::testAddAndLayout(test);
        bloom::commands::test::testWiringAndRename(test);
        bloom::commands::test::testRemoveAndDissolve(test);
        bloom::commands::test::testDissolveParticipatingLayer(test);
        bloom::commands::test::testDeepDuplication(test);
        bloom::commands::test::testDuplicationOwnershipEdges(test);
        bloom::commands::test::testParameterSocketDrivers(test);
        bloom::commands::test::testNodeGroups(test);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return test.failures() == 0 ? 0 : 1;
}
