#include <bloom/ui/node_layout_arrange.hpp>

#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

namespace bloom::ui {
namespace {
using document::NodeId;

struct DirectedLink final {
    NodeId source;
    NodeId destination;
};

struct LinkIndex final {
    std::map<NodeId, std::vector<NodeId>> incoming;
    std::map<NodeId, std::vector<NodeId>> outgoing;
};

struct Component final {
    std::vector<NodeId> nodes;
    std::map<NodeId, int> ranks;
    std::map<int, std::vector<NodeId>> ordered;
};

[[nodiscard]] bool validSize(const NodeCardSize size) noexcept {
    return std::isfinite(size.width) && size.width > 0.0 && std::isfinite(size.height) &&
           size.height > 0.0;
}

[[nodiscard]] NodeCardSize sizeFor(const document::NodeLayout& layout, const NodeCardSizes& sizes,
                                   const NodeId id) noexcept {
    if (const auto found = sizes.find(id); found != sizes.end() && validSize(found->second))
        return found->second;
    if (const auto found = layout.find(id); found != layout.end()) {
        return {std::isfinite(found->second.width) && found->second.width > 0.0
                    ? found->second.width
                    : document::kConservativeNodeCardSize.width,
                document::kConservativeNodeCardSize.height};
    }
    return document::kConservativeNodeCardSize;
}

[[nodiscard]] std::vector<DirectedLink>
linksFor(const document::CanonicalGraph& graph,
         const document::ParameterStore* parameters = nullptr) {
    std::vector<DirectedLink> links;
    links.reserve(graph.edges().size());
    for (const auto& edge : graph.edges()) {
        const auto destination = std::visit(
            [](const auto& port) {
                if constexpr (std::is_same_v<std::decay_t<decltype(port)>, document::NodeInputRef>)
                    return port.nodeId;
                else
                    return port.stackNodeId;
            },
            edge.destination);
        links.push_back({edge.source.nodeId, destination});
    }
    if (parameters == nullptr)
        return links;
    for (const auto& node : graph.nodes())
        for (const auto& binding : node.parameters)
            if (const auto* parameter = parameters->find(binding.parameterId))
                if (const auto* driver =
                        std::get_if<document::DriverBindingSource>(&parameter->source))
                    links.push_back({driver->sourceNodeId, node.id});
    return links;
}

[[nodiscard]] LinkIndex indexLinks(const std::vector<NodeId>& nodes,
                                   const std::vector<DirectedLink>& links) {
    LinkIndex index;
    for (const auto id : nodes) {
        index.incoming[id];
        index.outgoing[id];
    }
    for (const auto& link : links) {
        if (!index.incoming.contains(link.source) || !index.incoming.contains(link.destination))
            continue;
        index.outgoing.at(link.source).push_back(link.destination);
        index.incoming.at(link.destination).push_back(link.source);
    }
    for (auto& [unused, neighbours] : index.incoming)
        std::ranges::sort(neighbours);
    for (auto& [unused, neighbours] : index.outgoing)
        std::ranges::sort(neighbours);
    return index;
}

[[nodiscard]] std::map<NodeId, document::NodeGroupId>
groupOwners(const document::NodeGroups& groups, const std::set<NodeId>& selected) {
    std::map<NodeId, document::NodeGroupId> owners;
    for (const auto& [groupId, group] : groups)
        for (const auto id : group.members)
            if (selected.contains(id))
                owners.emplace(id, groupId);
    return owners;
}

[[nodiscard]] bool contains(const std::set<NodeId>& selection, const NodeId id) {
    return selection.empty() || selection.contains(id);
}

[[nodiscard]] std::vector<Component> makeComponents(const std::vector<NodeId>& nodes,
                                                    const std::vector<DirectedLink>& links,
                                                    const std::set<NodeId>& selected,
                                                    const std::optional<NodeId> compositionOutput) {
    std::map<NodeId, std::vector<NodeId>> neighbours;
    for (const auto id : nodes)
        neighbours[id];
    for (const auto& link : links) {
        if (!contains(selected, link.source) || !contains(selected, link.destination))
            continue;
        neighbours[link.source].push_back(link.destination);
        neighbours[link.destination].push_back(link.source);
    }

    std::set<NodeId> unvisited;
    for (const auto id : nodes)
        if (contains(selected, id))
            unvisited.insert(id);
    std::vector<Component> components;
    while (!unvisited.empty()) {
        Component component;
        std::vector<NodeId> pending{*unvisited.begin()};
        unvisited.erase(unvisited.begin());
        while (!pending.empty()) {
            const auto id = pending.back();
            pending.pop_back();
            component.nodes.push_back(id);
            auto adjacent = neighbours[id];
            std::ranges::sort(adjacent);
            for (const auto neighbour : adjacent)
                if (unvisited.erase(neighbour) != 0)
                    pending.push_back(neighbour);
        }
        std::ranges::sort(component.nodes);
        components.push_back(std::move(component));
    }
    std::ranges::sort(components, [&](const Component& left, const Component& right) {
        const auto leftHasOutput =
            compositionOutput && std::ranges::binary_search(left.nodes, *compositionOutput);
        const auto rightHasOutput =
            compositionOutput && std::ranges::binary_search(right.nodes, *compositionOutput);
        if (leftHasOutput != rightHasOutput)
            return leftHasOutput;
        return left.nodes.front() < right.nodes.front();
    });
    return components;
}

void rankComponent(Component& component, const LinkIndex& index) {
    std::map<NodeId, std::vector<NodeId>> outgoing;
    std::map<NodeId, std::size_t> indegree;
    for (const auto id : component.nodes) {
        outgoing[id];
        indegree[id] = 0;
    }
    const std::set<NodeId> members(component.nodes.begin(), component.nodes.end());
    for (const auto source : component.nodes)
        for (const auto destination : index.outgoing.at(source))
            if (members.contains(destination)) {
                outgoing[source].push_back(destination);
                ++indegree[destination];
            }

    std::set<NodeId> ready;
    for (const auto& [id, degree] : indegree)
        if (degree == 0)
            ready.insert(id);
    std::vector<NodeId> topological;
    while (!ready.empty()) {
        const auto id = *ready.begin();
        ready.erase(ready.begin());
        topological.push_back(id);
        for (const auto destination : outgoing[id])
            if (--indegree[destination] == 0)
                ready.insert(destination);
    }
    // A composition with a cycle is not publishable, but the layout function is also used by
    // diagnostics. Put any malformed remainder into a stable rank instead of looping forever.
    const std::set<NodeId> scheduled(topological.begin(), topological.end());
    for (const auto id : component.nodes)
        if (!scheduled.contains(id))
            topological.push_back(id);

    for (const auto id : component.nodes)
        component.ranks[id] = 0;
    for (const auto source : topological) {
        const auto sourceRank = component.ranks[source];
        for (const auto destination : outgoing[source])
            component.ranks[destination] = std::max(component.ranks[destination], sourceRank + 1);
    }
    for (const auto id : component.nodes)
        component.ordered[component.ranks[id]].push_back(id);
    for (auto& [unused, rank] : component.ordered)
        std::ranges::sort(rank);
}

[[nodiscard]] std::map<NodeId, std::size_t>
rankPositions(const std::map<int, std::vector<NodeId>>& ranks) {
    std::map<NodeId, std::size_t> positions;
    for (const auto& [unused, nodes] : ranks)
        for (std::size_t index = 0; index < nodes.size(); ++index)
            positions[nodes[index]] = index;
    return positions;
}

[[nodiscard]] double barycenter(const NodeId id, const int rank, const bool preceding,
                                const Component& component, const LinkIndex& links,
                                const std::map<NodeId, std::size_t>& positions) {
    double total = 0.0;
    std::size_t count = 0;
    const auto& neighbours = preceding ? links.incoming.at(id) : links.outgoing.at(id);
    for (const auto neighbour : neighbours) {
        const auto neighbourRank = component.ranks.find(neighbour);
        if (neighbourRank == component.ranks.end() ||
            neighbourRank->second != rank + (preceding ? -1 : 1))
            continue;
        const auto position = positions.find(neighbour);
        if (position == positions.end())
            continue;
        total += static_cast<double>(position->second);
        ++count;
    }
    if (count == 0) {
        const auto current = positions.find(id);
        return current == positions.end() ? 0.0 : static_cast<double>(current->second);
    }
    return total / static_cast<double>(count);
}

struct RankBlock final {
    std::optional<document::NodeGroupId> group;
    std::vector<NodeId> nodes;
    double barycenter = 0.0;
    std::uint64_t tie = 0;
};

void sweep(Component& component, const LinkIndex& links,
           const std::map<NodeId, std::uint64_t>& groupKeys, const bool leftToRight) {
    if (component.ordered.empty())
        return;
    std::vector<int> ranks;
    ranks.reserve(component.ordered.size());
    for (const auto& [rank, unused] : component.ordered)
        ranks.push_back(rank);
    if (!leftToRight)
        std::ranges::reverse(ranks);

    auto positions = rankPositions(component.ordered);
    for (const int rank : ranks) {
        const auto& current = component.ordered.at(rank);
        std::map<document::NodeGroupId, std::vector<NodeId>> grouped;
        std::vector<NodeId> ungrouped;
        for (const auto id : current) {
            const auto group = groupKeys.find(id);
            if (group == groupKeys.end())
                ungrouped.push_back(id);
            else
                grouped[document::NodeGroupId::fromRaw(group->second)].push_back(id);
        }

        std::vector<RankBlock> blocks;
        blocks.reserve(grouped.size() + ungrouped.size());
        for (auto& [group, members] : grouped) {
            RankBlock block{group, std::move(members), 0.0, group.value()};
            for (const auto id : block.nodes)
                block.barycenter += barycenter(id, rank, leftToRight, component, links, positions);
            block.barycenter /= static_cast<double>(block.nodes.size());
            blocks.push_back(std::move(block));
        }
        for (const auto id : ungrouped)
            blocks.push_back({std::nullopt,
                              {id},
                              barycenter(id, rank, leftToRight, component, links, positions),
                              id.value()});
        std::ranges::sort(blocks, [](const RankBlock& left, const RankBlock& right) {
            if (left.barycenter != right.barycenter)
                return left.barycenter < right.barycenter;
            return left.tie < right.tie;
        });
        std::vector<NodeId> result;
        for (auto& block : blocks) {
            std::ranges::sort(block.nodes, [&](const NodeId left, const NodeId right) {
                const auto leftBary =
                    barycenter(left, rank, leftToRight, component, links, positions);
                const auto rightBary =
                    barycenter(right, rank, leftToRight, component, links, positions);
                if (leftBary != rightBary)
                    return leftBary < rightBary;
                return left < right;
            });
            result.insert(result.end(), block.nodes.begin(), block.nodes.end());
        }
        component.ordered[rank] = std::move(result);
        for (std::size_t index = 0; index < component.ordered.at(rank).size(); ++index)
            positions[component.ordered.at(rank)[index]] = index;
    }
}

class Fenwick final {
  public:
    explicit Fenwick(const std::size_t size) : tree_(size + 1) {}
    void add(std::size_t index) {
        for (++index; index < tree_.size(); index += index & (~index + 1))
            ++tree_[index];
    }
    [[nodiscard]] std::size_t sum(std::size_t index) const {
        std::size_t result = 0;
        for (++index; index != 0; index &= index - 1)
            result += tree_[index];
        return result;
    }

  private:
    std::vector<std::size_t> tree_;
};

[[nodiscard]] std::size_t crossingCount(const Component& component, const LinkIndex& links) {
    const auto positions = rankPositions(component.ordered);
    std::size_t crossings = 0;
    for (const auto& [rank, nodes] : component.ordered) {
        if (nodes.empty())
            continue;
        std::vector<std::pair<std::size_t, std::size_t>> edges;
        std::size_t nextRankSize = 0;
        if (const auto next = component.ordered.find(rank + 1); next != component.ordered.end())
            nextRankSize = next->second.size();
        for (const auto source : nodes)
            for (const auto destination : links.outgoing.at(source))
                if (const auto destinationRank = component.ranks.find(destination);
                    destinationRank != component.ranks.end() && destinationRank->second == rank + 1)
                    edges.emplace_back(positions.at(source), positions.at(destination));
        if (edges.empty() || nextRankSize == 0)
            continue;
        std::ranges::sort(edges);
        Fenwick tree(nextRankSize);
        std::size_t seen = 0;
        for (const auto& [unused, destination] : edges) {
            crossings += seen - tree.sum(destination);
            tree.add(destination);
            ++seen;
        }
    }
    return crossings;
}

[[nodiscard]] NodePositions
arrangeSelected(const document::CanonicalGraph& graph, const document::NodeLayout& layout,
                const NodeCardSizes& sizes, const std::set<NodeId>& selection,
                const document::NodeGroups& groups, const document::ParameterStore* parameters) {
    std::vector<NodeId> nodes;
    nodes.reserve(graph.nodes().size());
    for (const auto& node : graph.nodes())
        if (contains(selection, node.id))
            nodes.push_back(node.id);
    std::ranges::sort(nodes);
    if (nodes.empty())
        return {};

    const auto links = linksFor(graph, parameters);
    const auto linkIndex = indexLinks(nodes, links);
    std::optional<NodeId> output;
    if (graph.compositionOutput())
        output = graph.compositionOutput()->nodeId;
    auto components = makeComponents(nodes, links, selection, output);
    const auto owners = groupOwners(groups, std::set<NodeId>(nodes.begin(), nodes.end()));
    std::map<NodeId, std::uint64_t> groupKeys;
    for (const auto& [id, group] : owners)
        groupKeys.emplace(id, group.value());

    NodePositions result;
    double componentTop = 0.0;
    for (auto& component : components) {
        rankComponent(component, linkIndex);
        const auto baseline = crossingCount(component, linkIndex);
        auto best = component.ordered;
        auto bestCrossings = baseline;
        for (int pass = 0; pass < 4; ++pass) {
            sweep(component, linkIndex, groupKeys, true);
            sweep(component, linkIndex, groupKeys, false);
            const auto currentCrossings = crossingCount(component, linkIndex);
            if (currentCrossings < bestCrossings) {
                bestCrossings = currentCrossings;
                best = component.ordered;
            }
        }
        component.ordered = best;

        std::map<int, double> rankHeights;
        double componentHeight = 0.0;
        for (const auto& [rank, rankNodes] : component.ordered) {
            double height = 0.0;
            for (const auto id : rankNodes)
                height += sizeFor(layout, sizes, id).height;
            if (rankNodes.size() > 1)
                height += static_cast<double>(rankNodes.size() - 1) * document::kNodeLayoutSpacing;
            rankHeights[rank] = height;
            componentHeight = std::max(componentHeight, height);
        }
        std::map<int, double> rankX;
        double x = 0.0;
        for (const auto& [rank, rankNodes] : component.ordered) {
            rankX[rank] = x;
            double width = 0.0;
            for (const auto id : rankNodes)
                width = std::max(width, sizeFor(layout, sizes, id).width);
            x += width + document::kNodeLayoutSpacing;
        }
        for (const auto& [rank, rankNodes] : component.ordered) {
            double y = componentTop + (componentHeight - rankHeights.at(rank)) / 2.0;
            for (const auto id : rankNodes) {
                result[id] = {rankX.at(rank), y};
                y += sizeFor(layout, sizes, id).height + document::kNodeLayoutSpacing;
            }
        }
        componentTop += componentHeight + document::kNodeLayoutSpacing;
    }
    return result;
}

} // namespace

NodePositions arrangeNodes(const document::CanonicalGraph& graph,
                           const document::NodeLayout& layout, const NodeCardSizes& sizes) {
    return arrangeSelected(graph, layout, sizes, {}, {}, nullptr);
}

NodePositions arrangeNodes(const document::Composition& composition, const NodeCardSizes& sizes) {
    return arrangeSelected(composition.graph(), composition.nodeLayout(), sizes, {},
                           composition.nodeGroups(), &composition.parameters());
}

NodePositions arrangeNodes(const document::CanonicalGraph& graph,
                           const document::NodeLayout& layout, const NodeCardSizes& sizes,
                           const std::set<document::NodeId>& selection) {
    return arrangeSelected(graph, layout, sizes, selection, {}, nullptr);
}

NodePositions arrangeNodes(const document::Composition& composition, const NodeCardSizes& sizes,
                           const std::set<document::NodeId>& selection) {
    return arrangeSelected(composition.graph(), composition.nodeLayout(), sizes, selection,
                           composition.nodeGroups(), &composition.parameters());
}

} // namespace bloom::ui
