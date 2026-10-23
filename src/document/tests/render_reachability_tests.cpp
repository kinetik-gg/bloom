#include <bloom/document/project.hpp>
#include <bloom/document/render_reachability.hpp>
#include <bloom/document/value_nodes.hpp>

#include <array>
#include <iostream>
#include <string>

namespace {
using namespace bloom::document;
using bloom::core::RationalTime;

int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

[[nodiscard]] RationalTime t(const std::int64_t seconds) {
    return RationalTime::fromInteger(seconds);
}

constexpr auto kStack = NodeId::fromRaw(1);
constexpr auto kOutput = NodeId::fromRaw(2);
constexpr auto kFirstLayer = NodeId::fromRaw(3);
constexpr auto kSecondLayer = NodeId::fromRaw(4);
constexpr auto kFirstSolid = NodeId::fromRaw(5);
constexpr auto kSecondSolid = NodeId::fromRaw(6);
constexpr auto kBlur = NodeId::fromRaw(7);
constexpr auto kValue = NodeId::fromRaw(8);
constexpr auto kFirstLayerId = LayerId::fromRaw(30);
constexpr auto kSecondLayerId = LayerId::fromRaw(31);
constexpr auto kFirstSlot = LayerSlotId::fromRaw(40);
constexpr auto kSecondSlot = LayerSlotId::fromRaw(41);
constexpr auto kRadius = ParameterId::fromRaw(60);
constexpr auto kValueParameter = ParameterId::fromRaw(61);
constexpr auto kMain = CompositionId::fromRaw(10);

void require(const bool condition, const char* message) {
    if (!condition)
        throw std::logic_error(message);
}

// Output <- Stack <- {First Layer <- First Solid, Second Layer <- Second Solid}, plus an
// unconnected Blur whose radius may be driven by an unconnected scalar value node.
struct Fixture {
    CanonicalGraph graph{kStack};
    std::uint64_t nextEdge = 100;

    Fixture() {
        require(graph.addNode(
                    {kStack, std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion}),
                "stack");
        require(graph.addNode({kOutput,
                               std::string(kCompositionOutputNodeType),
                               {},
                               kCompositionOutputNodeSchemaVersion}),
                "output");
        for (const auto [layer, solid, layerId, slot] :
             {std::tuple{kFirstLayer, kFirstSolid, kFirstLayerId, kFirstSlot},
              std::tuple{kSecondLayer, kSecondSolid, kSecondLayerId, kSecondSlot}}) {
            require(
                graph.addNode(
                    {layer, std::string(kLayerOutputNodeType), {}, kLayerOutputNodeSchemaVersion}),
                "layer");
            require(
                graph.addNode(
                    {solid, std::string(kSolidSourceNodeType), {}, kSolidSourceNodeSchemaVersion}),
                "solid");
            require(graph.addLayerOutput({layer, layerId, "Layer", "image"}), "boundary");
            require(graph.layerStack().append({slot, layerId}), "slot");
            connect({solid, "image"}, NodeInputRef{layer, "image"});
            connect({layer, "image"},
                    LayerStackInputRef{kStack, slot, std::string(kLayerStackContentInputRole)});
        }
        connect({kStack, "image"}, NodeInputRef{kOutput, "image"});
        graph.setCompositionOutput({kOutput, "image"});
        require(graph.addNode({kBlur, "bloom.gaussian-blur", {{"radius", kRadius}}, 1}), "blur");
        require(graph.addNode({kValue,
                               std::string(kScalarValueNodeType),
                               {{std::string(kValueParameterRole), kValueParameter}},
                               kValueNodeSchemaVersion}),
                "value");
    }

    EdgeId connect(OutputPortRef source, InputPortRef destination) {
        const auto id = EdgeId::fromRaw(nextEdge++);
        require(graph.addEdge({id, std::move(source), std::move(destination)}), "edge");
        return id;
    }

    void disconnect(const NodeId destination) {
        for (const auto& edge : graph.edges()) {
            const auto* input = std::get_if<NodeInputRef>(&edge.destination);
            if (input != nullptr && input->nodeId == destination) {
                require(graph.eraseEdge(edge.id), "erase");
                return;
            }
        }
    }

    [[nodiscard]] Composition build(const bool driveRadius = false) const {
        Composition composition(kMain, "Main", t(10), graph);
        require(composition.parameters().insert({kValueParameter,
                                                 std::string(kScalarValueParameterSchemaKey),
                                                 ConstantValueSource{2.0}}),
                "value parameter");
        ParameterSource radius = ConstantValueSource{0.0};
        if (driveRadius)
            radius = DriverBindingSource{kValue, std::string(kValuePortName)};
        require(composition.parameters().insert({kRadius, "bloom.blur.radius", radius}), "radius");
        return composition;
    }
};

[[nodiscard]] RenderConfinement confine(const Composition& composition, const NodeId node) {
    const auto reachability = RenderReachability::compute(composition);
    const std::array changed{node};
    return confineNodeChange(composition, reachability, changed);
}

void testReachableSet() {
    Fixture fixture;
    const auto composition = fixture.build();
    const auto reachability = RenderReachability::compute(composition);
    expect(reachability.known(), "an ordinary graph has a known reachable set");
    for (const auto node : {kOutput, kStack, kFirstLayer, kSecondLayer, kFirstSolid, kSecondSolid})
        expect(reachability.reaches(node), "the stacked chain is reachable");
    expect(!reachability.reaches(kBlur) && !reachability.reaches(kValue),
           "unconnected nodes are not reachable");
    expect(reachability.edges().size() == 5, "every consumed edge is recorded");
    expect(confine(composition, kBlur).kind == RenderConfinementKind::Unaffected,
           "an unconnected node edit cannot change output");
    expect(confine(composition, kValue).kind == RenderConfinementKind::Unaffected,
           "an unconnected value edit cannot change output");

    Composition noOutput(kMain, "Main", t(10), CanonicalGraph{kStack});
    expect(!RenderReachability::compute(noOutput).known(),
           "a composition without an output is unproven, never empty");
}

void testLayerGate() {
    Fixture fixture;
    auto composition = fixture.build();
    auto* first = composition.graph().findLayer(kFirstLayerId);
    first->inPoint = t(2);
    first->outPoint = t(5);
    const auto content = confine(composition, kFirstSolid);
    expect(content.kind == RenderConfinementKind::LayerSpans && content.spans.size() == 1 &&
               content.spans.front() == RenderTimeSpan{t(2), t(5)},
           "a layer's content edit is confined to its active span");
    const auto own = confine(composition, kFirstLayer);
    expect(own.kind == RenderConfinementKind::LayerSpans && own.spans.size() == 1 &&
               own.spans.front() == RenderTimeSpan{t(2), t(5)},
           "a layer boundary's own parameter edit is confined to its span");
    expect(confine(composition, kStack).kind == RenderConfinementKind::WholeComposition,
           "an edit past every layer gate reaches the output at every time");

    // A post-stack effect is still time-local: the layer gate holds.
    Fixture post;
    post.disconnect(kOutput);
    post.connect({kStack, "image"}, NodeInputRef{kBlur, "input"});
    post.connect({kBlur, "image"}, NodeInputRef{kOutput, "image"});
    auto postComposition = post.build(true);
    postComposition.graph().findLayer(kFirstLayerId)->outPoint = t(4);
    const auto behind = confine(postComposition, kFirstSolid);
    expect(behind.kind == RenderConfinementKind::LayerSpans &&
               behind.spans.front() == RenderTimeSpan{t(0), t(4)},
           "a post-stack effect does not widen a gated layer span");
    expect(confine(postComposition, kValue).kind == RenderConfinementKind::WholeComposition,
           "a driver of a post-stack effect changes every time");
    expect(RenderReachability::compute(postComposition).reaches(kValue),
           "a driving value node is reachable through its parameter");
}

void testDriversInsideALayer() {
    Fixture fixture;
    fixture.disconnect(kFirstLayer);
    fixture.connect({kFirstSolid, "image"}, NodeInputRef{kBlur, "input"});
    fixture.connect({kBlur, "image"}, NodeInputRef{kFirstLayer, "image"});
    auto composition = fixture.build(true);
    composition.graph().findLayer(kFirstLayerId)->inPoint = t(3);
    const auto driven = confine(composition, kValue);
    expect(driven.kind == RenderConfinementKind::LayerSpans &&
               driven.spans.front() == RenderTimeSpan{t(3), t(10)},
           "a driver inside one layer is confined to that layer");

    // The same layer also read as a whole output (not through its own slot) is no longer a gate.
    composition.graph().findLayer(kFirstLayerId)->inPoint = t(0);
    auto direct = Fixture{};
    direct.disconnect(kOutput);
    direct.connect({kFirstLayer, "image"}, NodeInputRef{kOutput, "image"});
    expect(confine(direct.build(), kFirstSolid).kind == RenderConfinementKind::WholeComposition,
           "a layer output consumed directly is not gated by its span");
}

void testSoloMuteAndDisabledStacks() {
    {
        Fixture fixture;
        auto composition = fixture.build();
        composition.graph().findLayer(kSecondLayerId)->solo = true;
        const auto reachability = RenderReachability::compute(composition);
        expect(!reachability.reaches(kFirstLayer) && !reachability.reaches(kFirstSolid),
               "a solo elsewhere excludes this layer from the reachable set");
        expect(confine(composition, kFirstSolid).kind == RenderConfinementKind::Unaffected,
               "a solo-excluded layer edit cannot change output");
    }
    {
        Fixture fixture;
        auto composition = fixture.build();
        composition.nodeLayout()[kStack].muted = true;
        expect(confine(composition, kFirstSolid).kind == RenderConfinementKind::WholeComposition,
               "a muted stack passes its first slot through ungated");
        expect(!RenderReachability::compute(composition).reaches(kSecondLayer),
               "a muted stack consumes only its first slot");
    }
    {
        Fixture fixture;
        auto composition = fixture.build();
        composition.graph().merge(kStack)->setEnabled(false);
        const auto reachability = RenderReachability::compute(composition);
        expect(reachability.reaches(kStack) && !reachability.reaches(kFirstLayer),
               "a disabled merge consumes no slot");
    }
    {
        Fixture fixture;
        auto composition = fixture.build();
        composition.graph().findLayer(kFirstLayerId)->enabled = false;
        expect(confine(composition, kFirstSolid).kind == RenderConfinementKind::Unaffected,
               "a disabled layer edit cannot change output");
    }
}

void testParentsAndReadouts() {
    {
        Fixture fixture;
        auto composition = fixture.build();
        auto* first = composition.graph().findLayer(kFirstLayerId);
        auto* second = composition.graph().findLayer(kSecondLayerId);
        first->outPoint = t(3);
        second->inPoint = t(6);
        second->parent = kFirstLayerId;
        const auto parent = confine(composition, kFirstLayer);
        expect(parent.kind == RenderConfinementKind::LayerSpans && parent.spans.size() == 2 &&
                   parent.spans[0] == RenderTimeSpan{t(0), t(3)} &&
                   parent.spans[1] == RenderTimeSpan{t(6), t(10)},
               "a parent edit also reaches every child's span");
    }
    {
        Fixture fixture;
        const auto readout = NodeId::fromRaw(9);
        require(fixture.graph.addNode(
                    {readout, std::string(kLayerBoundsNodeType), {}, kValueNodeSchemaVersion}),
                "readout");
        fixture.connect({kFirstLayer, "image"},
                        NodeInputRef{readout, std::string(kLayerBoundsImagePortName)});
        const auto composition = fixture.build();
        expect(RenderReachability::compute(composition).reaches(readout),
               "a readout is a reachability root");
        expect(confine(composition, kFirstSolid).kind == RenderConfinementKind::Unproven,
               "a change reaching a readout is unproven");
    }
}

[[nodiscard]] Composition withId(const Composition& base, const CompositionId id) {
    Composition composition(id, "Copy", t(10), base.graph());
    for (const auto& record : base.parameters().records())
        require(composition.parameters().insert(record), "copy parameter");
    return composition;
}

[[nodiscard]] Composition nestingComposition(const CompositionId id, const CompositionId target,
                                             const bool connected, const bool constant = true) {
    Fixture fixture;
    const auto source = NodeId::fromRaw(9);
    const auto reference = ParameterId::fromRaw(62);
    require(fixture.graph.addNode({source,
                                   std::string(kCompositionSourceNodeType),
                                   {{"composition", reference}},
                                   kCompositionSourceNodeSchemaVersion}),
            "composition source");
    if (connected) {
        fixture.disconnect(kFirstLayer);
        fixture.connect({source, "image"}, NodeInputRef{kFirstLayer, "image"});
    }
    auto composition = withId(fixture.build(), id);
    ParameterSource value = ConstantValueSource{static_cast<std::int64_t>(target.value())};
    if (!constant)
        value = DriverBindingSource{kValue, std::string(kValuePortName)};
    require(
        composition.parameters().insert({reference, "bloom.composition-source.composition", value}),
        "reference");
    return composition;
}

void testNestedDependencies() {
    const auto inner = CompositionId::fromRaw(20);
    const auto outer = CompositionId::fromRaw(21);
    const auto unrelated = CompositionId::fromRaw(22);
    const auto top = CompositionId::fromRaw(23);
    {
        Project project(ProjectId::fromRaw(1), "Nest");
        Fixture fixture;
        require(project.addComposition(withId(fixture.build(), inner)), "inner");
        require(project.addComposition(nestingComposition(outer, inner, true)), "outer");
        require(project.addComposition(nestingComposition(unrelated, inner, false)), "unrelated");
        require(project.addComposition(nestingComposition(top, outer, true)), "top");
        const auto depending = compositionsDependingOn(project, inner);
        expect(depending == std::set<CompositionId>{inner, outer, top},
               "reachable nesting is transitive and an unconnected source is not a dependency");
        expect(compositionsDependingOn(project, unrelated) == std::set<CompositionId>{unrelated},
               "nothing nests an unrelated composition");
        const auto* outerComposition = project.findComposition(outer);
        expect(RenderReachability::compute(*outerComposition).nestedCompositions() ==
                   std::set<CompositionId>{inner},
               "a reachable source records its constant reference");
    }
    {
        Project project(ProjectId::fromRaw(1), "Nest");
        Fixture fixture;
        require(project.addComposition(withId(fixture.build(), inner)), "inner");
        require(project.addComposition(nestingComposition(outer, unrelated, true, false)), "outer");
        expect(compositionsDependingOn(project, inner).contains(outer),
               "a non-constant reference may name any composition");
    }
}

void testNormalizationAndWork() {
    const auto spans =
        normalizeRenderTimeSpans({{t(6), t(12)}, {t(-1), t(2)}, {t(2), t(3)}, {t(4), t(4)}}, t(10));
    expect(spans == std::vector<RenderTimeSpan>{{t(0), t(3)}, {t(6), t(10)}},
           "spans are clipped, merged when touching and emptied of zero-length entries");

    // The walk is linear in the composition it reads: doubling unrelated nodes doubles only the
    // node scan, never the reachable traversal.
    Fixture small;
    Fixture large;
    for (std::uint64_t index = 0; index < 64; ++index)
        require(large.graph.addNode({NodeId::fromRaw(1000 + index),
                                     std::string(kSolidSourceNodeType),
                                     {},
                                     kSolidSourceNodeSchemaVersion}),
                "filler");
    const auto smallWork = RenderReachability::compute(small.build()).work();
    const auto largeWork = RenderReachability::compute(large.build()).work();
    expect(largeWork.nodesVisited == smallWork.nodesVisited + 64,
           "unreachable nodes cost one scan step each and are never traversed");
    expect(largeWork.edgesVisited == smallWork.edgesVisited, "unreachable nodes add no edge work");
}

} // namespace

int main() {
    try {
        testReachableSet();
        testLayerGate();
        testDriversInsideALayer();
        testSoloMuteAndDisabledStacks();
        testParentsAndReadouts();
        testNestedDependencies();
        testNormalizationAndWork();
    } catch (const std::exception& error) {
        std::cerr << "fixture failure: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " render reachability expectation(s) failed\n";
        return 1;
    }
    return 0;
}
