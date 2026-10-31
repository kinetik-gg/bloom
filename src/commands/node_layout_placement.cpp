#include <bloom/commands/node_layout_placement.hpp>

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <variant>
#include <vector>

namespace bloom::commands {
namespace {
using document::NodeCardSize;
using document::NodeCardSizes;
using document::NodeId;
using document::Vec2d;

// The editor's card metrics in document units, frozen here the way kDefaultNodeGroupPadding is:
// Size::NodeTitleBand, Size::PropertyRow, Spacing::S and Size::NodeCardWidth, plus the Merge
// pill's per-slot pitch (Spacing::M). The estimate only has to be an upper bound of what the
// editor draws; the node canvas tests hold every built-in card to it.
constexpr double kTitleBand = 32.0;
constexpr double kRow = 28.0;
constexpr double kPadding = 8.0;
// Size::NodeCardWidth (240) plus the label column an ordinary built-in card can add to it, on the
// grid's half step. A card with a text field, a video's stream readout or a vector operation's
// operator selector runs wider still.
constexpr double kCardWidth = 264.0;
constexpr double kWideCardWidth = 288.0;
constexpr double kSlotPitch = 12.0;

struct Rect final {
    double left = 0.0;
    double top = 0.0;
    double width = 0.0;
    double height = 0.0;
    [[nodiscard]] double right() const noexcept { return left + width; }
    [[nodiscard]] double bottom() const noexcept { return top + height; }
};

[[nodiscard]] bool touches(const Rect& candidate, const Rect& existing, const double gap) noexcept {
    return candidate.left < existing.right() + gap && candidate.right() + gap > existing.left &&
           candidate.top < existing.bottom() + gap && candidate.bottom() + gap > existing.top;
}

[[nodiscard]] double snapDown(const double value) noexcept {
    return std::floor(value / kNodeLayoutGrid) * kNodeLayoutGrid;
}
[[nodiscard]] double snapUp(const double value) noexcept {
    return std::ceil(value / kNodeLayoutGrid) * kNodeLayoutGrid;
}

[[nodiscard]] bool sane(const NodeCardSize size) noexcept {
    return std::isfinite(size.width) && size.width > 0.0 && std::isfinite(size.height) &&
           size.height > 0.0;
}

[[nodiscard]] std::vector<Rect> obstacles(const document::Composition& composition,
                                          const NodeCardSizes& sizes,
                                          const std::set<NodeId>& ignored) {
    std::vector<Rect> result;
    result.reserve(composition.graph().nodes().size());
    for (const auto& node : composition.graph().nodes()) {
        if (ignored.contains(node.id))
            continue;
        const auto position = effectiveNodePosition(composition, node.id);
        const auto found = sizes.find(node.id);
        const auto size = found != sizes.end() && sane(found->second)
                              ? found->second
                              : estimatedNodeCardSize(composition, node.id);
        result.push_back({position.x, position.y, size.width, size.height});
    }
    return result;
}

[[nodiscard]] bool clear(const std::vector<Rect>& rects, const Rect& candidate, const double gap) {
    return std::ranges::none_of(rects,
                                [&](const Rect& rect) { return touches(candidate, rect, gap); });
}

// The upstream closure of `node` (its inputs, their inputs, ...), including `node` itself: the
// cards that make up one layer's row.
[[nodiscard]] std::set<NodeId> upstreamOf(const document::Composition& composition,
                                          const NodeId node) {
    std::set<NodeId> visited{node};
    std::vector<NodeId> pending{node};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        for (const auto& edge : composition.graph().edges()) {
            const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            if (input != nullptr && input->nodeId == id && visited.insert(edge.source.nodeId).second)
                pending.push_back(edge.source.nodeId);
        }
    }
    return visited;
}
} // namespace

NodeCardSize estimatedNodeCardSize(const document::Composition& composition, const NodeId id) {
    const auto* node = composition.graph().findNode(id);
    const auto layout = composition.nodeLayout().find(id);
    const double width =
        std::max(kCardWidth, layout != composition.nodeLayout().end() &&
                                     std::isfinite(layout->second.width)
                                 ? layout->second.width
                                 : 0.0);
    if (node == nullptr)
        return document::kConservativeNodeCardSize;
    if (document::isRerouteNodeType(node->typeId))
        return {kCardWidth, kTitleBand};
    if (layout != composition.nodeLayout().end() && layout->second.collapsed)
        return {width, kTitleBand * 2.0};
    const auto* definition =
        document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
    if (definition == nullptr)
        return document::kConservativeNodeCardSize;
    const bool wide =
        node->typeId == "bloom.video-source" || node->typeId == document::kVector2MathNodeType ||
        node->typeId == document::kVector3MathNodeType ||
        std::ranges::any_of(definition->parameters,
                            [](const auto& parameter) {
                                return parameter.valueKind == document::ParameterValueKind::String;
                            }) ||
        std::ranges::any_of(definition->inputs,
                            [](const auto& port) {
                                return port.valueKind == document::SocketValueKind::String;
                            }) ||
        std::ranges::any_of(definition->outputs, [](const auto& port) {
            return port.valueKind == document::SocketValueKind::String;
        });
    // A vector parameter takes one row per component (they share the first one's socket).
    double rows = 0.0;
    for (const auto& binding : node->parameters) {
        const auto parameter = std::ranges::find(definition->parameters, binding.role,
                                                 &document::ParameterDefinition::role);
        const auto kind = parameter == definition->parameters.end()
                              ? document::ParameterValueKind::Float64
                              : parameter->valueKind;
        // A colour is its chip plus one row per channel.
        rows += kind == document::ParameterValueKind::Vec2d     ? 2.0
                : kind == document::ParameterValueKind::Vec3d   ? 3.0
                : kind == document::ParameterValueKind::Color4d ? 4.0
                                                                : 1.0;
    }
    double sockets = static_cast<double>(definition->outputs.size());
    for (const auto& port : definition->inputs)
        if (std::ranges::none_of(node->parameters,
                                 [&](const auto& binding) { return binding.role == port.name; }))
            sockets += 1.0;
    double pills = 0.0;
    if (const auto* stack = composition.graph().merge(id)) {
        const double slots = static_cast<double>(stack->entries().size());
        const double pill = std::max(kRow, slots * kSlotPitch);
        pills += definition->layerSlotInput ? pill : 0.0;
        pills += definition->audioLayerSlotInput ? pill : 0.0;
    }
    // Sources carry a dimensions/range readout row beside their parameters and generators a
    // second closing row; a bounds readout shows each value it reads out on its own row.
    rows += 2.0;
    if (definition->lowering == document::NodeLoweringKind::ValueBoundsReadout)
        rows += static_cast<double>(definition->outputs.size());
    return {wide ? std::max(width, kWideCardWidth) : width,
            kTitleBand + sockets * kRow + pills + rows * kRow + kPadding};
}

NodeCardSizes nodeCardSizes(const document::Composition& composition,
                            const NodeCardSizes& measured) {
    NodeCardSizes sizes;
    for (const auto& node : composition.graph().nodes()) {
        const auto found = measured.find(node.id);
        sizes.emplace(node.id, found != measured.end() && sane(found->second)
                                   ? found->second
                                   : estimatedNodeCardSize(composition, node.id));
    }
    return sizes;
}

Vec2d effectiveNodePosition(const document::Composition& composition, const NodeId node) {
    if (const auto found = composition.nodeLayout().find(node);
        found != composition.nodeLayout().end())
        return found->second.position;
    const auto defaults = document::defaultNodeLayout(composition.graph().nodes());
    const auto found = defaults.find(node);
    return found == defaults.end() ? Vec2d{} : found->second.position;
}

NodeCardSizes nodeCardFloorSizes(const document::Composition& composition,
                                 const NodeCardSizes& measured) {
    auto sizes = nodeCardSizes(composition, measured);
    for (auto& [id, size] : sizes) {
        if (measured.contains(id))
            continue;
        const auto layout = composition.nodeLayout().find(id);
        size.width = std::max(240.0, layout != composition.nodeLayout().end() &&
                                             std::isfinite(layout->second.width)
                                         ? layout->second.width
                                         : 0.0);
    }
    return sizes;
}

Vec2d placeNodeCard(const document::Composition& composition, NodeCardSize size, Vec2d preferred,
                    const NodeCardSizes& sizes, const std::set<NodeId>& ignored,
                    const double acceptGap, const NodeCardSizes* acceptSizes,
                    const std::optional<NodeCardSize> acceptSize) {
    if (!sane(size))
        size = document::kConservativeNodeCardSize;
    if (!std::isfinite(preferred.x) || !std::isfinite(preferred.y))
        preferred = {};
    const auto rects = obstacles(composition, sizes, ignored);
    const auto at = [&](const double x, const double y) {
        return Rect{x, y, size.width, size.height};
    };
    const auto accepting = acceptSize && sane(*acceptSize) ? *acceptSize : size;
    if (clear(acceptSizes != nullptr ? obstacles(composition, *acceptSizes, ignored) : rects,
              Rect{preferred.x, preferred.y, accepting.width, accepting.height},
              std::max(0.0, acceptGap)))
        return preferred;

    // Candidates are the positions just past each neighbour on either axis, on the grid and a full
    // gap away, plus the preferred coordinate itself. Single-axis moves come first: the nearest
    // clear one wins, and the card below every other card in its column is always clear, so this
    // search always ends.
    std::vector<double> xs{preferred.x};
    std::vector<double> ys{preferred.y};
    for (const auto& rect : rects) {
        xs.push_back(snapUp(rect.right() + kNodeLayoutGap));
        xs.push_back(snapDown(rect.left - kNodeLayoutGap - size.width));
        ys.push_back(snapUp(rect.bottom() + kNodeLayoutGap));
        ys.push_back(snapDown(rect.top - kNodeLayoutGap - size.height));
    }
    std::optional<std::tuple<double, double, double>> best; // distance, y, x
    const auto consider = [&](const double x, const double y) {
        const double dx = x - preferred.x;
        const double dy = y - preferred.y;
        const std::tuple candidate{dx * dx + dy * dy, y, x};
        if (best && !(candidate < *best))
            return;
        if (clear(rects, at(x, y), kNodeLayoutGap))
            best = candidate;
    };
    for (const auto y : ys)
        consider(preferred.x, y);
    for (const auto x : xs)
        consider(x, preferred.y);
    if (best)
        return {std::get<2>(*best), std::get<1>(*best)};
    return document::findNearestFreeNodePosition(composition.nodeLayout(), sizes, size, preferred,
                                                 kNodeLayoutGap);
}

void placeLayerCards(document::Composition& composition, const NodeId layerNode,
                     const NodeId sourceNode, const NodeCardSizes& measured) {
    const auto& graph = composition.graph();
    const auto sizes = nodeCardSizes(composition, measured);
    const auto sizeOf = [&](const NodeId id) {
        const auto found = sizes.find(id);
        return found == sizes.end() ? document::kConservativeNodeCardSize : found->second;
    };
    // The stack this layer feeds, and its place in that stack's order (entry zero is the top).
    std::optional<document::LayerStackInputRef> slot;
    for (const auto& edge : graph.edges())
        if (const auto* input = std::get_if<document::LayerStackInputRef>(&edge.destination);
            input != nullptr && edge.source.nodeId == layerNode)
            slot = *input;
    std::optional<NodeId> above;
    std::optional<NodeId> below;
    std::optional<NodeId> stackNode;
    if (slot) {
        stackNode = slot->stackNodeId;
        if (const auto* stack = graph.merge(slot->stackNodeId)) {
            const auto entries = stack->entries();
            const auto boundaryNode = [&](const document::LayerId layer) -> std::optional<NodeId> {
                for (const auto& boundary : graph.layerOutputs())
                    if (boundary.layerId == layer && boundary.nodeId != layerNode)
                        return boundary.nodeId;
                return std::nullopt;
            };
            for (std::size_t index = 0; index < entries.size(); ++index) {
                if (entries[index].slotId != slot->slotId)
                    continue;
                for (std::size_t up = index; up-- > 0 && !above;)
                    above = boundaryNode(entries[up].layerId);
                for (std::size_t down = index + 1; down < entries.size() && !below; ++down)
                    below = boundaryNode(entries[down].layerId);
            }
        }
    }
    const std::set<NodeId> placed{layerNode, sourceNode};
    const auto layerSize = sizeOf(layerNode);
    const auto sourceSize = sizeOf(sourceNode);
    const double rowHeight = std::max(layerSize.height, sourceSize.height);
    // One neighbouring layer's row: the band its Layer Output and everything upstream of it fill.
    const auto rowBand = [&](const NodeId neighbour) {
        double top = std::numeric_limits<double>::infinity();
        double bottom = -std::numeric_limits<double>::infinity();
        for (const auto id : upstreamOf(composition, neighbour)) {
            if (placed.contains(id))
                continue;
            const auto position = effectiveNodePosition(composition, id);
            top = std::min(top, position.y);
            bottom = std::max(bottom, position.y + sizeOf(id).height);
        }
        return std::pair{top, bottom};
    };
    Vec2d layerPreferred;
    if (above || below) {
        const auto neighbour = above ? *above : *below;
        layerPreferred.x = effectiveNodePosition(composition, neighbour).x;
        if (above)
            layerPreferred.y = snapUp(rowBand(*above).second + kNodeLayoutGap);
        else
            layerPreferred.y = snapDown(rowBand(*below).first - kNodeLayoutGap - rowHeight);
    } else if (stackNode) {
        const auto merge = effectiveNodePosition(composition, *stackNode);
        layerPreferred = {snapDown(merge.x - kNodeLayoutGap - layerSize.width), merge.y};
    }
    const auto layerPosition =
        placeNodeCard(composition, layerSize, layerPreferred, sizes, placed, kNodeLayoutGap);
    composition.nodeLayout()[layerNode] = {layerPosition, 128.0, false, false};
    const Vec2d sourcePreferred{snapDown(layerPosition.x - kNodeLayoutGap - sourceSize.width),
                                layerPosition.y};
    const auto sourcePosition =
        placeNodeCard(composition, sourceSize, sourcePreferred, sizes, {sourceNode}, kNodeLayoutGap);
    composition.nodeLayout()[sourceNode] = {sourcePosition, 128.0, false, false};
}

} // namespace bloom::commands
