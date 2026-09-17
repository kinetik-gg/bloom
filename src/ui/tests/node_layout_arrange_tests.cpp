#include <bloom/ui/node_layout_arrange.hpp>

#include <bloom/core/rational_time.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace bloom;
using document::CanonicalGraph;
using document::EdgeId;
using document::EdgeRecord;
using document::NodeCardSize;
using document::NodeCardSizes;
using document::NodeId;
using document::NodeInputRef;
using document::NodeLayout;
using document::NodeRecord;
using document::OutputPortRef;
using document::Vec2d;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (condition)
        return;
    ++failures;
    std::cerr << message << '\n';
}

struct Fixture final {
    CanonicalGraph graph{NodeId::fromRaw(9999)};
    NodeLayout layout;
    NodeCardSizes sizes;

    void addNode(const std::uint64_t raw, const double width = 240.0, const double height = 120.0) {
        const auto id = NodeId::fromRaw(raw);
        expect(graph.addNode(NodeRecord{id, "test.node", {}, 1}), "fixture node is accepted");
        layout.emplace(
            id, document::NodeLayoutRecord{
                    {static_cast<double>(raw) * 10.0, static_cast<double>(raw) * 3.0}, width});
        sizes.emplace(id, NodeCardSize{width, height});
    }

    void connect(const std::uint64_t edge, const std::uint64_t source,
                 const std::uint64_t destination, std::string destinationPort = "in") {
        expect(graph.addEdge(EdgeRecord{
                   EdgeId::fromRaw(edge), OutputPortRef{NodeId::fromRaw(source), "out"},
                   NodeInputRef{NodeId::fromRaw(destination), std::move(destinationPort)}}),
               "fixture edge is accepted");
    }
};

[[nodiscard]] bool overlaps(const Vec2d left, const NodeCardSize leftSize, const Vec2d right,
                            const NodeCardSize rightSize) {
    return left.x < right.x + rightSize.width && left.x + leftSize.width > right.x &&
           left.y < right.y + rightSize.height && left.y + leftSize.height > right.y;
}

[[nodiscard]] NodeId destinationNode(const EdgeRecord& edge) {
    if (const auto* destination = std::get_if<NodeInputRef>(&edge.destination))
        return destination->nodeId;
    return std::get_if<document::LayerStackInputRef>(&edge.destination)->stackNodeId;
}

void expectNoOverlap(const Fixture& fixture, const ui::NodePositions& positions,
                     const std::string& name) {
    for (const auto& [left, leftPosition] : positions)
        for (const auto& [right, rightPosition] : positions)
            if (left < right && overlaps(leftPosition, fixture.sizes.at(left), rightPosition,
                                         fixture.sizes.at(right)))
                expect(false, name + " contains overlapping cards");
}

[[nodiscard]] std::map<NodeId, std::size_t> rankOrder(const ui::NodePositions& positions) {
    std::map<double, std::vector<NodeId>> columns;
    for (const auto& [id, position] : positions)
        columns[position.x].push_back(id);
    std::map<NodeId, std::size_t> result;
    std::size_t rank = 0;
    for (auto& [unused, nodes] : columns) {
        std::ranges::sort(nodes, [&](const NodeId left, const NodeId right) {
            if (positions.at(left).y != positions.at(right).y)
                return positions.at(left).y < positions.at(right).y;
            return left < right;
        });
        for (const auto id : nodes)
            result[id] = rank;
        ++rank;
    }
    return result;
}

[[nodiscard]] std::size_t crossingCount(const CanonicalGraph& graph,
                                        const ui::NodePositions& positions) {
    const auto ranks = rankOrder(positions);
    std::map<std::size_t, std::vector<std::pair<std::size_t, std::size_t>>> byRank;
    std::map<std::size_t, std::map<NodeId, std::size_t>> order;
    for (const auto& [id, position] : positions)
        order[rankOrder(positions).at(id)][id] = 0;
    for (auto& [rank, nodes] : order) {
        std::vector<NodeId> sorted;
        for (const auto& [id, unused] : nodes)
            sorted.push_back(id);
        std::ranges::sort(sorted, [&](const NodeId left, const NodeId right) {
            if (positions.at(left).y != positions.at(right).y)
                return positions.at(left).y < positions.at(right).y;
            return left < right;
        });
        for (std::size_t index = 0; index < sorted.size(); ++index)
            nodes[sorted[index]] = index;
    }
    for (const auto& edge : graph.edges()) {
        const auto sourceRank = ranks.at(edge.source.nodeId);
        const auto destination = destinationNode(edge);
        const auto destinationRank = ranks.at(destination);
        if (destinationRank == sourceRank + 1)
            byRank[sourceRank].emplace_back(order[sourceRank].at(edge.source.nodeId),
                                            order[destinationRank].at(destination));
    }
    std::size_t result = 0;
    for (auto& [unused, edges] : byRank) {
        std::ranges::sort(edges);
        for (std::size_t left = 0; left < edges.size(); ++left)
            for (std::size_t right = left + 1; right < edges.size(); ++right)
                if (edges[left].second > edges[right].second)
                    ++result;
    }
    return result;
}

void testChainAndStability() {
    Fixture fixture;
    for (std::uint64_t id = 1; id <= 4; ++id)
        fixture.addNode(id);
    fixture.connect(10, 1, 2);
    fixture.connect(11, 2, 3);
    fixture.connect(12, 3, 4);

    const auto arranged = ui::arrangeNodes(fixture.graph, fixture.layout, fixture.sizes);
    expect(arranged.size() == 4, "chain arranges every node");
    expectNoOverlap(fixture, arranged, "chain");
    for (const auto& edge : fixture.graph.edges())
        expect(arranged.at(edge.source.nodeId).x < arranged.at(destinationNode(edge)).x,
               "chain edges point left to right");
    expect(arranged == ui::arrangeNodes(fixture.graph, fixture.layout, fixture.sizes),
           "arrangement is stable for identical input");
}

void testSixLayerFanInReducesCrossings() {
    Fixture fixture;
    constexpr std::uint64_t kBranches = 6;
    constexpr std::uint64_t kDepth = 6;
    for (std::uint64_t depth = 0; depth < kDepth; ++depth)
        for (std::uint64_t branch = 0; branch < kBranches; ++branch)
            fixture.addNode(depth * kBranches + branch + 1, 220.0, 80.0);
    fixture.addNode(1000, 260.0, 100.0);

    std::uint64_t edge = 1;
    for (std::uint64_t depth = 0; depth + 1 < kDepth; ++depth)
        for (std::uint64_t branch = 0; branch < kBranches; ++branch) {
            const auto nextBranch = depth % 2 == 0 ? kBranches - branch - 1 : branch;
            fixture.connect(edge++, depth * kBranches + branch + 1,
                            (depth + 1) * kBranches + nextBranch + 1);
        }
    for (std::uint64_t branch = 0; branch < kBranches; ++branch)
        fixture.connect(edge++, (kDepth - 1) * kBranches + branch + 1, 1000,
                        "in" + std::to_string(branch));

    const auto arranged = ui::arrangeNodes(fixture.graph, fixture.layout, fixture.sizes);
    expectNoOverlap(fixture, arranged, "six-layer fan-in");
    for (const auto& edgeRecord : fixture.graph.edges())
        expect(arranged.at(edgeRecord.source.nodeId).x < arranged.at(destinationNode(edgeRecord)).x,
               "fan-in ranks are monotone");

    ui::NodePositions naive;
    for (std::uint64_t depth = 0; depth < kDepth; ++depth)
        for (std::uint64_t branch = 0; branch < kBranches; ++branch)
            naive[NodeId::fromRaw(depth * kBranches + branch + 1)] = {
                static_cast<double>(depth * 1000), static_cast<double>(branch * 100)};
    naive[NodeId::fromRaw(1000)] = {static_cast<double>(kDepth * 1000), 250.0};
    expect(crossingCount(fixture.graph, arranged) <= crossingCount(fixture.graph, naive),
           "barycenter sweeps do not increase naive crossings");
}

void testComponentsAndGroups() {
    Fixture fixture;
    fixture.addNode(1);
    fixture.addNode(2);
    fixture.addNode(10);
    fixture.addNode(11);
    fixture.connect(1, 1, 2);
    fixture.connect(2, 10, 11);
    const auto arranged = ui::arrangeNodes(fixture.graph, fixture.layout, fixture.sizes);
    expectNoOverlap(fixture, arranged, "components");
    expect(arranged.at(NodeId::fromRaw(10)).y > arranged.at(NodeId::fromRaw(1)).y,
           "disconnected components stack below one another");

    CanonicalGraph groupedGraph{NodeId::fromRaw(99)};
    for (const auto id : {NodeId::fromRaw(1), NodeId::fromRaw(2), NodeId::fromRaw(3)})
        expect(groupedGraph.addNode(NodeRecord{id, "test.node", {}, 1}), "group node is accepted");
    expect(groupedGraph.addEdge({EdgeId::fromRaw(1),
                                 {NodeId::fromRaw(1), "out"},
                                 NodeInputRef{NodeId::fromRaw(3), "in"}}),
           "first group edge is accepted");
    expect(groupedGraph.addEdge({EdgeId::fromRaw(2),
                                 {NodeId::fromRaw(2), "out"},
                                 NodeInputRef{NodeId::fromRaw(3), "in2"}}),
           "second group edge is accepted");
    groupedGraph.setCompositionOutput({NodeId::fromRaw(3), "out"});
    document::Composition composition{document::CompositionId::fromRaw(1), "Main",
                                      core::RationalTime::fromInteger(10), std::move(groupedGraph)};
    const auto groupId = document::NodeGroupId::fromRaw(1);
    composition.nodeGroups()[groupId] = {
        groupId, "Sources", {NodeId::fromRaw(1), NodeId::fromRaw(2)}, {}};
    const NodeCardSizes groupedSizes{{NodeId::fromRaw(1), {240.0, 120.0}},
                                     {NodeId::fromRaw(2), {240.0, 120.0}},
                                     {NodeId::fromRaw(3), {240.0, 120.0}}};
    const auto grouped = ui::arrangeNodes(composition, groupedSizes);
    expect(grouped.at(NodeId::fromRaw(1)).x == grouped.at(NodeId::fromRaw(2)).x,
           "group members remain in one rank");
    expect(std::abs(grouped.at(NodeId::fromRaw(1)).y - grouped.at(NodeId::fromRaw(2)).y) == 136.0,
           "group members remain contiguous with the card gap");
}

void testValueDriverChain() {
    CanonicalGraph graph{NodeId::fromRaw(9)};
    const auto source = NodeId::fromRaw(1);
    const auto driven = NodeId::fromRaw(2);
    const auto parameter = document::ParameterId::fromRaw(3);
    expect(graph.addNode(NodeRecord{source, "test.value", {}, 1}), "driver source is accepted");
    expect(graph.addNode(NodeRecord{driven, "test.consumer", {{"input", parameter}}, 1}),
           "driver destination is accepted");
    document::Composition composition{document::CompositionId::fromRaw(1), "Drivers",
                                      core::RationalTime::fromInteger(10), std::move(graph)};
    expect(composition.parameters().insert(
               {parameter, "test.scalar", document::DriverBindingSource{source, "value"}}),
           "driver binding is stored");
    const auto arranged = ui::arrangeNodes(composition, {});
    expect(arranged.at(source).x < arranged.at(driven).x,
           "value-driver links rank the driver before its consumer");
}

} // namespace

int main() {
    testChainAndStability();
    testSixLayerFanInReducesCrossings();
    testComponentsAndGroups();
    testValueDriverChain();
    return failures == 0 ? 0 : 1;
}
