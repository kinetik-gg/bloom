#include <bloom/document/node_layout.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <string>

namespace {
using namespace bloom::document;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

void socketKinds() {
    // All eight kinds after task S7, and the expectation is no longer "equal kinds connect" but the
    // shared isAcceptedSocketConnection() predicate -- equal kinds, or one of the five whitelisted
    // promotions. Stated through the predicate rather than a second copy of the table so this test
    // cannot drift from the rule the editor and the compiler both ask.
    constexpr std::array kinds{SocketValueKind::Image,   SocketValueKind::Color,
                               SocketValueKind::Scalar,  SocketValueKind::Vector2,
                               SocketValueKind::String,  SocketValueKind::Integer,
                               SocketValueKind::Boolean, SocketValueKind::Vector3};
    for (const auto sourceKind : kinds) {
        for (const auto destinationKind : kinds) {
            NodeDefinitionRegistry registry;
            expect(registry.registerDefinition({{"test.source", 1},
                                                NodeLoweringKind::Unsupported,
                                                {},
                                                {{"out", sourceKind}},
                                                {},
                                                std::nullopt}) ==
                       NodeRegistrationStatus::Registered,
                   "register source kind");
            expect(registry.registerDefinition({{"test.sink", 1},
                                                NodeLoweringKind::Unsupported,
                                                {{"in", destinationKind, true}},
                                                {},
                                                {},
                                                std::nullopt}) ==
                       NodeRegistrationStatus::Registered,
                   "register destination kind");
            registry.freeze();
            CanonicalGraph graph(NodeId::fromRaw(9));
            expect(graph.addNode({NodeId::fromRaw(1), "test.source", {}, 1}), "source node");
            expect(graph.addNode({NodeId::fromRaw(2), "test.sink", {}, 1}), "sink node");
            const EdgeRecord edge{EdgeId::fromRaw(1),
                                  {NodeId::fromRaw(1), "out"},
                                  NodeInputRef{NodeId::fromRaw(2), "in"}};
            const bool accepted = isAcceptedSocketConnection(sourceKind, destinationKind);
            expect(graph.addEdge(edge, registry) == accepted, "adder kind check");
            if (!accepted) {
                expect(graph.edges().empty(), "rejected edge leaves graph untouched");
                expect(graph.addEdge(edge), "unknown schemas remain preservable");
            }
            const auto validation = graph.validate(ParameterStore{}, registry);
            expect(std::ranges::any_of(validation.issues(),
                                       [](const auto& issue) {
                                           return issue.code == ValidationCode::SocketKindMismatch;
                                       }) == !accepted,
                   "validation reports typed mismatch");
        }
    }
}

void layoutRecords() {
    CanonicalGraph graph(NodeId::fromRaw(3));
    const std::array types{"bloom.solid-source", "bloom.layer-output", "bloom.layer-stack",
                           "bloom.composition-output", "bloom.text-source"};
    for (std::size_t index = 0; index < types.size(); ++index) {
        expect(graph.addNode({NodeId::fromRaw(index + 1), types[index], {}, 1}), "layout node");
    }
    auto layout = defaultNodeLayout(graph.nodes());
    expect(layout.at(NodeId::fromRaw(1)).position == Vec2d{32, 32}, "source origin");
    expect(layout.at(NodeId::fromRaw(2)).position == Vec2d{288, 32}, "boundary column");
    expect(layout.at(NodeId::fromRaw(3)).position == Vec2d{544, 32}, "stack column");
    expect(layout.at(NodeId::fromRaw(4)).position == Vec2d{800, 32}, "output column");
    expect(layout.at(NodeId::fromRaw(5)).position == Vec2d{32, 212}, "second source row");
    layout[NodeId::fromRaw(999)] = {};
    const auto warnings = validateNodeLayout(layout, graph);
    expect(warnings.ok() && warnings.issues().size() == 1 &&
               warnings.issues().front().severity == ValidationSeverity::Warning,
           "unknown node layout is only a diagnostic");
    ValidationResult parent;
    parent.append("composition", warnings);
    expect(parent.ok(), "warning severity survives nesting");
    layout.at(NodeId::fromRaw(1)).width = 0;
    expect(!validateNodeLayout(layout, graph).ok(), "zero width refused");
    layout.at(NodeId::fromRaw(1)).width = std::numeric_limits<double>::infinity();
    expect(!validateNodeLayout(layout, graph).ok(), "infinite width refused");
}
void nodeGroupRecords() {
    CanonicalGraph graph(NodeId::fromRaw(3));
    const std::array types{"bloom.solid-source", "bloom.layer-output", "bloom.layer-stack",
                           "bloom.composition-output", "bloom.text-source"};
    for (std::size_t index = 0; index < types.size(); ++index) {
        expect(graph.addNode({NodeId::fromRaw(index + 1), types[index], {}, 1}), "group node");
    }
    NodeGroups groups;
    const auto first = NodeGroupId::fromRaw(1);
    groups[first] = {first, "Group", {NodeId::fromRaw(1), NodeId::fromRaw(2)}, {}};
    expect(NodeGroupRecord{}.padding == Vec2d{kDefaultNodeGroupPadding, kDefaultNodeGroupPadding},
           "a fresh record carries the frozen default padding");
    expect(validateNodeGroups(groups, graph).ok(), "a group over live nodes validates");
    expect(findNodeGroupOf(groups, NodeId::fromRaw(2)) == &groups.at(first), "member finds group");
    expect(findNodeGroupOf(groups, NodeId::fromRaw(4)) == nullptr, "nonmember finds nothing");

    groups.at(first).members.insert(NodeId::fromRaw(999));
    const auto missing = validateNodeGroups(groups, graph);
    expect(missing.ok() && missing.issues().size() == 1 &&
               missing.issues().front().severity == ValidationSeverity::Warning,
           "an unknown member is only a diagnostic");
    groups.at(first).members.erase(NodeId::fromRaw(999));

    const auto second = NodeGroupId::fromRaw(2);
    groups[second] = {second, "Second", {NodeId::fromRaw(2)}, {}};
    expect(!validateNodeGroups(groups, graph).ok(), "a node belongs to at most one group");
    groups.at(second).members = {NodeId::fromRaw(4)};
    expect(validateNodeGroups(groups, graph).ok(), "disjoint groups validate");

    groups.at(second).name.clear();
    expect(!validateNodeGroups(groups, graph).ok(), "an empty group name is refused");
    groups.at(second).name = "Second";
    groups.at(second).padding = {-1.0, 0.0};
    expect(!validateNodeGroups(groups, graph).ok(), "negative padding is refused");
    groups.at(second).padding = {std::numeric_limits<double>::quiet_NaN(), 0.0};
    expect(!validateNodeGroups(groups, graph).ok(), "nonfinite padding is refused");
    groups.at(second).padding = {};

    // A group is layout, so the composition carries it beside nodeLayout and validates it there.
    // The bare graph this fixture builds is not a wired composition, so the assertion is about
    // which issues mention nodeGroups rather than about overall validity.
    const auto groupIssues = [](const Composition& composition) {
        const auto validation = composition.validate();
        return std::ranges::count_if(validation.issues(), [](const auto& issue) {
            return issue.path.find("nodeGroups") != std::string::npos;
        });
    };
    Composition composition(CompositionId::fromRaw(1), "Main",
                            bloom::core::RationalTime::fromInteger(1), std::move(graph));
    composition.nodeGroups() = groups;
    expect(groupIssues(composition) == 0, "composition validates its groups");
    composition.nodeGroups().at(second).members.insert(NodeId::fromRaw(2));
    expect(groupIssues(composition) == 1, "composition reports overlapping membership");
}
} // namespace

int main() {
    socketKinds();
    layoutRecords();
    nodeGroupRecords();
    return failures == 0 ? 0 : 1;
}
