#include <bloom/commands/node_layout_arrange.hpp>

#include <bloom/commands/node_layout_placement.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// Arrange (task N1, item 7) reads a graph the way the layer stack reads: Composition Output on
// the right, each node further left the further upstream it is, and the rows in stack order --
// the top layer's row first, exactly as the Timeline lists it. Columns are the longest path from
// a node to the output side, so every Layer Output shares one column and every source the next.
// Rows come from one spanning tree grown from the output through each node's inputs in their own
// order (Merge's slots top first, an ordinary node's ports in declaration order): each node's
// upstream subtree owns a horizontal band, a node is centred on its band, and the bands of
// siblings never overlap, so no two cards can overlap either. Every position is on the layout
// grid, with kNodeLayoutGap between neighbouring columns and bands.
namespace bloom::commands {
namespace {
using document::NodeId;

// How an input sorts among its node's inputs: Merge's stack slots in stack order, then declared
// ports in declaration order, then driven parameters in binding order, then by name.
using InputKey = std::tuple<int, std::size_t, std::string>;

struct DirectedLink final {
    NodeId source;
    NodeId destination;
    InputKey key;
};

[[nodiscard]] double snapUp(const double value) noexcept {
    return std::ceil(value / kNodeLayoutGrid) * kNodeLayoutGrid;
}
[[nodiscard]] double snapDown(const double value) noexcept {
    return std::floor(value / kNodeLayoutGrid) * kNodeLayoutGrid;
}

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
                    ? std::max(found->second.width, document::kConservativeNodeCardSize.width)
                    : document::kConservativeNodeCardSize.width,
                document::kConservativeNodeCardSize.height};
    }
    return document::kConservativeNodeCardSize;
}

[[nodiscard]] std::size_t inputIndex(const document::NodeDefinition* definition,
                                     const std::string_view port) {
    if (definition != nullptr)
        for (std::size_t index = 0; index < definition->inputs.size(); ++index)
            if (definition->inputs[index].name == port)
                return index;
    return std::numeric_limits<std::size_t>::max();
}

[[nodiscard]] std::vector<DirectedLink>
linksFor(const document::CanonicalGraph& graph,
         const document::ParameterStore* parameters = nullptr) {
    const auto& registry = document::builtInNodeDefinitions();
    const auto definitionOf = [&](const NodeId id) -> const document::NodeDefinition* {
        const auto* node = graph.findNode(id);
        return node == nullptr ? nullptr : registry.find(node->typeId, node->schemaVersion);
    };
    std::vector<DirectedLink> links;
    links.reserve(graph.edges().size());
    for (const auto& edge : graph.edges()) {
        if (const auto* port = std::get_if<document::NodeInputRef>(&edge.destination)) {
            const auto index = inputIndex(definitionOf(port->nodeId), port->port);
            links.push_back({edge.source.nodeId, port->nodeId, InputKey{1, index, port->port}});
            continue;
        }
        const auto& slot = std::get<document::LayerStackInputRef>(edge.destination);
        std::size_t order = std::numeric_limits<std::size_t>::max();
        if (const auto* stack = graph.merge(slot.stackNodeId)) {
            const auto entries = stack->entries();
            for (std::size_t index = 0; index < entries.size(); ++index)
                if (entries[index].slotId == slot.slotId)
                    order = index;
        }
        links.push_back({edge.source.nodeId, slot.stackNodeId, InputKey{0, order, slot.role}});
    }
    if (parameters == nullptr)
        return links;
    for (const auto& node : graph.nodes())
        for (std::size_t binding = 0; binding < node.parameters.size(); ++binding) {
            const auto* parameter = parameters->find(node.parameters[binding].parameterId);
            const auto* driver =
                parameter ? std::get_if<document::DriverBindingSource>(&parameter->source)
                          : nullptr;
            if (driver == nullptr)
                continue;
            const auto& role = node.parameters[binding].role;
            const auto index = inputIndex(definitionOf(node.id), role);
            links.push_back({driver->sourceNodeId, node.id,
                             index != std::numeric_limits<std::size_t>::max()
                                 ? InputKey{1, index, role}
                                 : InputKey{2, binding, role}});
        }
    return links;
}

[[nodiscard]] bool contains(const std::set<NodeId>& selection, const NodeId id) {
    return selection.empty() || selection.contains(id);
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
    const std::set<NodeId> members(nodes.begin(), nodes.end());

    // Each node's inputs in their own order (deduplicated: Merge's content and audio slots of one
    // layer are one upstream), and the nodes each one feeds.
    auto links = linksFor(graph, parameters);
    std::ranges::stable_sort(links, [](const DirectedLink& left, const DirectedLink& right) {
        return std::tie(left.destination, left.key, left.source) <
               std::tie(right.destination, right.key, right.source);
    });
    std::map<NodeId, std::vector<NodeId>> inputs;
    std::map<NodeId, std::vector<NodeId>> outputs;
    for (const auto id : nodes) {
        inputs[id];
        outputs[id];
    }
    for (const auto& link : links) {
        if (!members.contains(link.source) || !members.contains(link.destination) ||
            link.source == link.destination)
            continue;
        auto& upstream = inputs.at(link.destination);
        if (std::ranges::find(upstream, link.source) == upstream.end())
            upstream.push_back(link.source);
        auto& downstream = outputs.at(link.source);
        if (std::ranges::find(downstream, link.destination) == downstream.end())
            downstream.push_back(link.destination);
    }

    // Columns from the right: the longest path to a node that feeds nothing here. A malformed
    // cyclic remainder (not publishable, but diagnostics still draw it) gets a stable column of
    // its own instead of looping.
    std::map<NodeId, int> column;
    std::map<NodeId, std::size_t> pending;
    std::vector<NodeId> ready;
    for (const auto id : nodes) {
        column[id] = 0;
        pending[id] = outputs.at(id).size();
        if (pending[id] == 0)
            ready.push_back(id);
    }
    std::set<NodeId> ranked;
    while (!ready.empty()) {
        const auto id = ready.back();
        ready.pop_back();
        ranked.insert(id);
        for (const auto source : inputs.at(id)) {
            column[source] = std::max(column[source], column[id] + 1);
            if (--pending[source] == 0)
                ready.push_back(source);
        }
    }
    int deepest = 0;
    for (const auto& [id, value] : column)
        deepest = std::max(deepest, value);
    for (const auto id : nodes)
        if (!ranked.contains(id))
            column[id] = deepest + 1;

    // Group members stay together among their siblings (a frame is drawn around its members).
    std::map<NodeId, document::NodeGroupId> owner;
    for (const auto& [groupId, group] : groups)
        for (const auto id : group.members)
            if (members.contains(id))
                owner.emplace(id, groupId);
    const auto keepGroupsTogether = [&](const std::vector<NodeId>& siblings) {
        std::vector<NodeId> ordered;
        std::set<NodeId> taken;
        for (std::size_t index = 0; index < siblings.size(); ++index) {
            const auto id = siblings[index];
            if (!taken.insert(id).second)
                continue;
            ordered.push_back(id);
            const auto group = owner.find(id);
            if (group == owner.end())
                continue;
            for (std::size_t later = index + 1; later < siblings.size(); ++later) {
                const auto mate = owner.find(siblings[later]);
                if (mate != owner.end() && mate->second == group->second &&
                    taken.insert(siblings[later]).second)
                    ordered.push_back(siblings[later]);
            }
        }
        return ordered;
    };

    // Roots: Composition Output first, then every other node that feeds nothing, by id; then
    // whatever a cycle left unreached.
    std::vector<NodeId> roots;
    const auto output = graph.compositionOutput();
    if (output && members.contains(output->nodeId) && outputs.at(output->nodeId).empty())
        roots.push_back(output->nodeId);
    for (const auto id : nodes)
        if (outputs.at(id).empty() && std::ranges::find(roots, id) == roots.end())
            roots.push_back(id);

    // The spanning tree: a node joins the subtree of the first consumer that reaches it.
    std::map<NodeId, std::vector<NodeId>> children;
    std::set<NodeId> visited;
    std::vector<NodeId> preorder;
    const auto grow = [&](const NodeId root) {
        if (!visited.insert(root).second)
            return;
        std::vector<std::pair<NodeId, std::size_t>> stack{{root, 0}};
        preorder.push_back(root);
        while (!stack.empty()) {
            const auto id = stack.back().first;
            const auto next = stack.back().second++;
            const auto& upstream = inputs.at(id);
            if (next >= upstream.size()) {
                children[id] = keepGroupsTogether(children[id]);
                stack.pop_back();
                continue;
            }
            const auto source = upstream[next];
            if (!visited.insert(source).second)
                continue;
            children[id].push_back(source);
            // A tree child always sits at least one column left of its parent, which is what
            // keeps a band's cards apart horizontally even in a malformed cycle.
            column[source] = std::max(column[source], column[id] + 1);
            preorder.push_back(source);
            stack.emplace_back(source, 0);
        }
    };
    for (const auto root : roots)
        grow(root);
    for (const auto id : nodes)
        if (!visited.contains(id)) {
            roots.push_back(id);
            grow(id);
        }

    // Bands, bottom-up: each node's band holds its own card and its children's bands one gap
    // apart, the smaller of the two centred on the larger (on the grid).
    std::map<NodeId, double> band;
    std::map<NodeId, double> nodeOffset;
    std::map<NodeId, double> childOffset;
    for (auto at = preorder.rbegin(); at != preorder.rend(); ++at) {
        const auto id = *at;
        const auto height = sizeFor(layout, sizes, id).height;
        double extent = 0.0;
        bool first = true;
        for (const auto child : children[id]) {
            if (!first)
                extent = snapUp(extent + kNodeLayoutGap);
            childOffset[child] = extent;
            extent += band.at(child);
            first = false;
        }
        double shift = 0.0;
        if (height >= extent) {
            nodeOffset[id] = 0.0;
            shift = snapDown((height - extent) / 2.0);
            band[id] = height;
        } else {
            nodeOffset[id] = snapDown((extent - height) / 2.0);
            band[id] = extent;
        }
        for (const auto child : children[id])
            childOffset[child] += shift;
    }

    // Column x, right to left: the rightmost column is the output side.
    int columns = 0;
    for (const auto& [id, value] : column)
        columns = std::max(columns, value + 1);
    std::vector<double> widths(static_cast<std::size_t>(columns), 0.0);
    for (const auto id : nodes) {
        auto& width = widths[static_cast<std::size_t>(column.at(id))];
        width = std::max(width, sizeFor(layout, sizes, id).width);
    }
    std::vector<double> columnX(static_cast<std::size_t>(columns), 0.0);
    for (int index = columns - 2; index >= 0; --index)
        columnX[static_cast<std::size_t>(index)] =
            snapUp(columnX[static_cast<std::size_t>(index + 1)] +
                   widths[static_cast<std::size_t>(index + 1)] + kNodeLayoutGap);

    // Tops, top-down: roots stacked one gap apart, every child band inside its parent's.
    NodePositions result;
    std::map<NodeId, double> bandTop;
    double nextRoot = 0.0;
    for (const auto root : roots) {
        bandTop[root] = nextRoot;
        nextRoot = snapUp(nextRoot + band.at(root) + kNodeLayoutGap);
    }
    for (const auto id : preorder) {
        const auto top = bandTop.at(id);
        result[id] = {columnX[static_cast<std::size_t>(column.at(id))], top + nodeOffset.at(id)};
        for (const auto child : children[id])
            bandTop[child] = top + childOffset.at(child);
    }
    return result;
}

} // namespace

NodePositions arrangeNodes(const document::CanonicalGraph& graph,
                           const document::NodeLayout& layout, const NodeCardSizes& sizes) {
    return arrangeSelected(graph, layout, sizes, {}, {}, nullptr);
}

// The composition forms fill every card the caller did not measure with the command-side
// estimate, so a script or an agent arranging with no scene gets the same tight layout.
NodePositions arrangeNodes(const document::Composition& composition, const NodeCardSizes& sizes) {
    return arrangeSelected(composition.graph(), composition.nodeLayout(),
                           nodeCardSizes(composition, sizes), {}, composition.nodeGroups(),
                           &composition.parameters());
}

NodePositions arrangeNodes(const document::CanonicalGraph& graph,
                           const document::NodeLayout& layout, const NodeCardSizes& sizes,
                           const std::set<document::NodeId>& selection) {
    return arrangeSelected(graph, layout, sizes, selection, {}, nullptr);
}

NodePositions arrangeNodes(const document::Composition& composition, const NodeCardSizes& sizes,
                           const std::set<document::NodeId>& selection) {
    return arrangeSelected(composition.graph(), composition.nodeLayout(),
                           nodeCardSizes(composition, sizes), selection, composition.nodeGroups(),
                           &composition.parameters());
}

OperationResult ArrangeNodes::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "The composition does not exist");
    for (const auto id : selection_)
        if (composition->graph().findNode(id) == nullptr)
            return OperationResult::rejected(OperationIssueCode::MissingReference,
                                             "Node " + std::to_string(id.value()) +
                                                 " is not in the composition");
    if (selection_.empty()) {
        auto positions = arrangeNodes(*composition, sizes_);
        if (positions.empty())
            return OperationResult::noChange();
        return MoveNodes(composition_, std::move(positions)).apply(draft);
    }
    const auto relative = arrangeNodes(*composition, sizes_, selection_);
    if (relative.empty())
        return OperationResult::noChange();
    // The arrangement is local to the selection. Shift it back to the selected cards' current
    // bounding-box top-left (on the grid), so arranging never drags the selection across the
    // canvas.
    double oldLeft = std::numeric_limits<double>::infinity();
    double oldTop = std::numeric_limits<double>::infinity();
    double newLeft = std::numeric_limits<double>::infinity();
    double newTop = std::numeric_limits<double>::infinity();
    for (const auto id : selection_) {
        const auto current = effectiveNodePosition(*composition, id);
        oldLeft = std::min(oldLeft, current.x);
        oldTop = std::min(oldTop, current.y);
        const auto arranged = relative.at(id);
        newLeft = std::min(newLeft, arranged.x);
        newTop = std::min(newTop, arranged.y);
    }
    const document::Vec2d offset{snapDown(oldLeft) - newLeft, snapDown(oldTop) - newTop};
    std::map<document::NodeId, document::Vec2d> positions;
    for (const auto& [id, position] : relative)
        positions.emplace(id, document::Vec2d{position.x + offset.x, position.y + offset.y});
    return MoveNodes(composition_, std::move(positions)).apply(draft);
}

} // namespace bloom::commands
