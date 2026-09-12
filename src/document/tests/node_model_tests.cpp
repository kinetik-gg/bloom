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
    constexpr std::array kinds{SocketValueKind::Image, SocketValueKind::Color,
                               SocketValueKind::Scalar, SocketValueKind::Vector2,
                               SocketValueKind::String};
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
            expect(graph.addEdge(edge, registry) == (sourceKind == destinationKind),
                   "adder kind check");
            if (sourceKind != destinationKind) {
                expect(graph.edges().empty(), "rejected edge leaves graph untouched");
                expect(graph.addEdge(edge), "unknown schemas remain preservable");
            }
            const auto validation = graph.validate(ParameterStore{}, registry);
            expect(std::ranges::any_of(validation.issues(),
                                       [](const auto& issue) {
                                           return issue.code == ValidationCode::SocketKindMismatch;
                                       }) == (sourceKind != destinationKind),
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
} // namespace

int main() {
    socketKinds();
    layoutRecords();
    return failures == 0 ? 0 : 1;
}
