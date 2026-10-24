#include <bloom/document/render_reachability.hpp>

#include <bloom/document/project.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace bloom::document {
namespace {

[[nodiscard]] NodeId destinationNodeOf(const InputPortRef& destination) noexcept {
    return std::visit(
        [](const auto& reference) -> NodeId {
            using Reference = std::decay_t<decltype(reference)>;
            if constexpr (std::is_same_v<Reference, NodeInputRef>)
                return reference.nodeId;
            else
                return reference.stackNodeId;
        },
        destination);
}

// The read-only graph questions the compiler asks while collecting its reachable set, answered
// over one composition. Kept in one place so the reachability walk and the confinement walk agree
// with each other and with snapshot_compiler_mute.ipp.
class GraphView final {
  public:
    GraphView(const Composition& composition, const NodeDefinitionRegistry& registry)
        : composition_(composition), graph_(composition.graph()), registry_(registry) {
        for (const auto& node : graph_.nodes())
            nodes_.emplace(node.id, &node);
        for (const auto& boundary : graph_.layerOutputs()) {
            boundaries_.emplace(boundary.nodeId, &boundary);
            // First boundary per layer wins, exactly as the compiler's linear search does.
            layers_.try_emplace(boundary.layerId, &boundary);
            anySolo_ = anySolo_ || boundary.solo;
            if (boundary.trackMatte)
                matteSources_.insert(boundary.trackMatte->source);
        }
        // #357: merges and their slots are indexed once, so consuming a slot edge is a lookup
        // rather than a scan of every merge, slot and boundary.
        for (const auto& stack : graph_.merges()) {
            merges_.try_emplace(stack.nodeId(), &stack);
            for (const auto& entry : stack.entries())
                slots_.try_emplace(SlotKey{stack.nodeId(), entry.slotId}, &entry);
        }
    }

    [[nodiscard]] const LayerStackEntry* slot(const NodeId stack,
                                              const LayerSlotId slot) const noexcept {
        const auto found = slots_.find(SlotKey{stack, slot});
        return found == slots_.end() ? nullptr : found->second;
    }
    [[nodiscard]] const LayerStack* merge(const NodeId id) const noexcept {
        const auto found = merges_.find(id);
        return found == merges_.end() ? nullptr : found->second;
    }

    // ADR 0028: a layer that is any layer's track matte contributes nothing to its Merge.
    [[nodiscard]] bool isMatteSource(const LayerId layer) const noexcept {
        return matteSources_.contains(layer);
    }

    [[nodiscard]] const NodeRecord* node(const NodeId id) const noexcept {
        const auto found = nodes_.find(id);
        return found == nodes_.end() ? nullptr : found->second;
    }
    [[nodiscard]] const LayerOutputBoundary* boundary(const NodeId id) const noexcept {
        const auto found = boundaries_.find(id);
        return found == boundaries_.end() ? nullptr : found->second;
    }
    [[nodiscard]] const NodeDefinition* definition(const NodeRecord& node) const noexcept {
        return registry_.find(node.typeId, node.schemaVersion);
    }
    [[nodiscard]] bool isMuted(const NodeId id) const {
        const auto found = composition_.nodeLayout().find(id);
        return found != composition_.nodeLayout().end() && found->second.muted;
    }

    // Driver sources of this node's parameters, in binding order.
    template <typename Visit>
    void forEachDriverSource(const NodeRecord& node, Visit&& visit) const {
        for (const auto& binding : node.parameters) {
            const auto* parameter = composition_.parameters().find(binding.parameterId);
            const auto* driver = parameter == nullptr
                                     ? nullptr
                                     : std::get_if<DriverBindingSource>(&parameter->source);
            if (driver != nullptr)
                visit(driver->sourceNodeId);
        }
    }

    [[nodiscard]] bool consumesEdge(const NodeRecord& node, const EdgeRecord& edge) const {
        if (const auto* stack = merge(node.id); stack && !stack->enabled())
            return false;
        if (isMuted(node.id)) {
            const auto input = firstImageInput(node);
            if (!input || *input != edge.destination)
                return false;
        }
        if (const auto* slot = std::get_if<LayerStackInputRef>(&edge.destination)) {
            const auto found = slots_.find(SlotKey{slot->stackNodeId, slot->slotId});
            if (found != slots_.end() &&
                (mutedLayer(*found->second) || isMatteSource(found->second->layerId)))
                return false;
        }
        return true;
    }

  private:
    [[nodiscard]] std::optional<InputPortRef> firstImageInput(const NodeRecord& node) const {
        const auto* definition = this->definition(node);
        const bool imageOutput = definition != nullptr &&
                                 std::ranges::any_of(definition->outputs, [](const auto& output) {
                                     return output.valueKind == SocketValueKind::Image;
                                 });
        if (definition == nullptr ||
            (!imageOutput && definition->lowering != NodeLoweringKind::CompositionOutput))
            return std::nullopt;
        for (const auto& input : definition->inputs) {
            if (input.valueKind == SocketValueKind::Image)
                return NodeInputRef{node.id, input.name};
        }
        const auto* stack = merge(node.id);
        const auto entries = stack ? stack->entries() : std::span<const LayerStackEntry>{};
        if (definition->layerSlotInput &&
            definition->layerSlotInput->valueKind == SocketValueKind::Image && !entries.empty())
            return LayerStackInputRef{node.id, entries.front().slotId,
                                      definition->layerSlotInput->role};
        return std::nullopt;
    }

    [[nodiscard]] bool mutedLayer(const LayerStackEntry& entry) const {
        const auto found = layers_.find(entry.layerId);
        if (found == layers_.end())
            return false;
        const auto& boundary = *found->second;
        return !boundary.enabled || (anySolo_ && !boundary.solo) || isMuted(boundary.nodeId);
    }

    struct SlotKey final {
        NodeId stack;
        LayerSlotId slot;
        friend bool operator==(const SlotKey&, const SlotKey&) = default;
    };
    struct SlotKeyHash final {
        [[nodiscard]] std::size_t operator()(const SlotKey& key) const noexcept {
            return std::hash<std::uint64_t>{}(key.stack.value() * 1'000'003ULL ^ key.slot.value());
        }
    };

    const Composition& composition_;
    const CanonicalGraph& graph_;
    const NodeDefinitionRegistry& registry_;
    std::unordered_map<NodeId, const NodeRecord*> nodes_;
    std::unordered_map<NodeId, const LayerOutputBoundary*> boundaries_;
    std::unordered_map<LayerId, const LayerOutputBoundary*> layers_;
    std::unordered_map<NodeId, const LayerStack*> merges_;
    std::unordered_map<SlotKey, const LayerStackEntry*, SlotKeyHash> slots_;
    std::unordered_set<LayerId> matteSources_;
    bool anySolo_ = false;
};

[[nodiscard]] std::optional<CompositionId>
constantCompositionReference(const Composition& owner, const NodeRecord& node, bool& unknown) {
    for (const auto& binding : node.parameters) {
        if (binding.role != "composition")
            continue;
        const auto* parameter = owner.parameters().find(binding.parameterId);
        const auto* constant =
            parameter ? std::get_if<ConstantValueSource>(&parameter->source) : nullptr;
        const auto* value = constant ? std::get_if<std::int64_t>(&constant->value) : nullptr;
        if (value == nullptr) {
            unknown = true;
            return std::nullopt;
        }
        // Zero or negative names nothing; the compiler diagnoses it without reading another
        // composition.
        if (*value <= 0)
            return std::nullopt;
        return CompositionId::fromRaw(static_cast<std::uint64_t>(*value));
    }
    unknown = true;
    return std::nullopt;
}

} // namespace

namespace {
std::atomic<std::uint64_t> reachabilityComputations{0};
} // namespace

std::shared_ptr<const RenderReachability>
RenderReachability::of(const Composition& composition, const NodeDefinitionRegistry& registry) {
    if (&registry == &builtInNodeDefinitions())
        if (auto cached = composition.frozenRenderReachability())
            return cached;
    return std::make_shared<const RenderReachability>(compute(composition, registry));
}

std::uint64_t RenderReachability::computations() noexcept {
    return reachabilityComputations.load(std::memory_order_relaxed);
}

RenderReachability RenderReachability::compute(const Composition& composition,
                                               const NodeDefinitionRegistry& registry) {
    reachabilityComputations.fetch_add(1, std::memory_order_relaxed);
    RenderReachability result;
    const auto& graph = composition.graph();
    if (!graph.compositionOutput().has_value())
        return result;
    const GraphView view(composition, registry);
    const auto rootId = graph.compositionOutput()->nodeId;
    if (view.node(rootId) == nullptr)
        return result;

    std::unordered_map<NodeId, std::vector<const EdgeRecord*>> incoming;
    std::unordered_map<NodeId, std::vector<NodeId>> driverConsumers;
    std::vector<NodeId> pending{rootId};
    for (const auto& edge : graph.edges()) {
        ++result.work_.edgesVisited;
        incoming[destinationNodeOf(edge.destination)].push_back(&edge);
    }
    for (const auto& node : graph.nodes()) {
        ++result.work_.nodesVisited;
        if (const auto* definition = view.definition(node);
            definition != nullptr && definition->lowering == NodeLoweringKind::ValueBoundsReadout)
            pending.push_back(node.id);
        view.forEachDriverSource(
            node, [&](const NodeId source) { driverConsumers[source].push_back(node.id); });
    }

    bool known = true;
    std::unordered_set<NodeId> visited;
    while (!pending.empty()) {
        const auto nodeId = pending.back();
        pending.pop_back();
        if (!visited.insert(nodeId).second)
            continue;
        const auto* node = view.node(nodeId);
        if (node == nullptr) {
            // The compiler refuses a reachable reference to a missing node outright.
            known = false;
            continue;
        }
        ++result.work_.nodesVisited;
        result.nodes_.insert(nodeId);
        if (view.definition(*node) == nullptr)
            known = false;
        if (const auto* boundary = view.boundary(nodeId); boundary && boundary->parent) {
            if (const auto* parent = graph.findLayer(*boundary->parent))
                pending.push_back(parent->nodeId);
        }
        // ADR 0028: a reached matte target consumes its source layer whatever the source's slot,
        // enabled or solo state.
        if (const auto* boundary = view.boundary(nodeId); boundary && boundary->trackMatte) {
            if (const auto* source = graph.findLayer(boundary->trackMatte->source))
                pending.push_back(source->nodeId);
        }
        if (const auto found = incoming.find(nodeId); found != incoming.end()) {
            for (const auto* edge : found->second) {
                ++result.work_.edgesVisited;
                if (!view.consumesEdge(*node, *edge))
                    continue;
                result.edges_.insert(edge->id);
                pending.push_back(edge->source.nodeId);
            }
        }
        view.forEachDriverSource(*node, [&](const NodeId source) { pending.push_back(source); });
        if (const auto consumers = driverConsumers.find(nodeId); consumers != driverConsumers.end())
            pending.insert(pending.end(), consumers->second.begin(), consumers->second.end());
        if (node->typeId == kCompositionSourceNodeType) {
            if (const auto target =
                    constantCompositionReference(composition, *node, result.nestedUnknown_))
                result.nested_.insert(*target);
        }
    }
    result.known_ = known;
    return result;
}

bool RenderReachability::sameRenderTopology(const RenderReachability& other) const noexcept {
    return known_ && other.known_ && nestedUnknown_ == other.nestedUnknown_ &&
           nodes_ == other.nodes_ && edges_ == other.edges_ && nested_ == other.nested_;
}

std::vector<RenderTimeSpan> normalizeRenderTimeSpans(std::vector<RenderTimeSpan> spans,
                                                     const core::RationalTime duration) {
    std::vector<RenderTimeSpan> clipped;
    clipped.reserve(spans.size());
    for (auto span : spans) {
        span.start = std::max(span.start, core::RationalTime{});
        span.end = std::min(span.end, duration);
        if (span.start < span.end)
            clipped.push_back(span);
    }
    std::ranges::sort(clipped, {}, &RenderTimeSpan::start);
    std::vector<RenderTimeSpan> merged;
    for (const auto& span : clipped) {
        if (!merged.empty() && span.start <= merged.back().end)
            merged.back().end = std::max(merged.back().end, span.end);
        else
            merged.push_back(span);
    }
    return merged;
}

RenderConfinement confineNodeChange(const Composition& composition,
                                    const RenderReachability& reachability,
                                    const std::span<const NodeId> changed,
                                    const NodeDefinitionRegistry& registry,
                                    RenderReachabilityWork* work) {
    RenderReachabilityWork local;
    auto& counters = work != nullptr ? *work : local;
    if (!reachability.known())
        return {};
    std::vector<NodeId> pending;
    for (const auto id : changed)
        if (reachability.reaches(id))
            pending.push_back(id);
    if (pending.empty())
        return {.kind = RenderConfinementKind::Unaffected, .spans = {}};

    const auto& graph = composition.graph();
    const GraphView view(composition, registry);
    // Forward adjacency restricted to reachable consumers. Following an edge the compiler would not
    // consume (a muted node's secondary input, say) only makes the walk more conservative.
    std::unordered_map<NodeId, std::vector<const EdgeRecord*>> outgoing;
    for (const auto& edge : graph.edges()) {
        ++counters.edgesVisited;
        if (reachability.reaches(destinationNodeOf(edge.destination)))
            outgoing[edge.source.nodeId].push_back(&edge);
    }
    std::unordered_map<NodeId, std::vector<NodeId>> drives;
    for (const auto id : reachability.nodes()) {
        ++counters.nodesVisited;
        if (const auto* node = view.node(id))
            view.forEachDriverSource(*node,
                                     [&](const NodeId source) { drives[source].push_back(id); });
    }
    std::unordered_map<LayerId, std::vector<NodeId>> children;
    // ADR 0028: a matte source reaches output only through its targets, inside their spans.
    std::unordered_map<LayerId, std::vector<NodeId>> matteTargets;
    for (const auto& boundary : graph.layerOutputs()) {
        if (boundary.parent && reachability.reaches(boundary.nodeId))
            children[*boundary.parent].push_back(boundary.nodeId);
        if (boundary.trackMatte && reachability.reaches(boundary.nodeId))
            matteTargets[boundary.trackMatte->source].push_back(boundary.nodeId);
    }

    // A boundary gates its own output when every consumer is an enabled, unmuted layer-stack slot
    // that belongs to this layer, and nothing reads it as a driver.
    const auto gates = [&](const LayerOutputBoundary& boundary) {
        if (drives.contains(boundary.nodeId))
            return false;
        const auto found = outgoing.find(boundary.nodeId);
        if (found == outgoing.end())
            return true;
        return std::ranges::all_of(found->second, [&](const EdgeRecord* edge) {
            const auto* slot = std::get_if<LayerStackInputRef>(&edge->destination);
            if (slot == nullptr)
                return false;
            const auto* stack = view.merge(slot->stackNodeId);
            const auto* entry = stack ? view.slot(slot->stackNodeId, slot->slotId) : nullptr;
            return entry != nullptr && entry->layerId == boundary.layerId && stack->enabled() &&
                   !view.isMuted(stack->nodeId());
        });
    };

    const auto rootId = graph.compositionOutput()->nodeId;
    const auto duration = composition.duration();
    bool reachedRoot = false;
    std::vector<RenderTimeSpan> spans;
    std::unordered_set<NodeId> visited;
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (!visited.insert(id).second)
            continue;
        ++counters.nodesVisited;
        if (id == rootId) {
            reachedRoot = true;
            continue;
        }
        const auto* node = view.node(id);
        const auto* definition = node ? view.definition(*node) : nullptr;
        if (definition == nullptr || definition->lowering == NodeLoweringKind::ValueBoundsReadout)
            return {};
        bool hasSuccessor = false;
        if (const auto* boundary = view.boundary(id)) {
            if (const auto found = children.find(boundary->layerId); found != children.end()) {
                hasSuccessor = true;
                pending.insert(pending.end(), found->second.begin(), found->second.end());
            }
            if (view.isMatteSource(boundary->layerId)) {
                // Hidden from its Merge: its own slot is not a consumer, its targets are. It is
                // never gated by its own span.
                if (const auto found = matteTargets.find(boundary->layerId);
                    found != matteTargets.end()) {
                    hasSuccessor = true;
                    pending.insert(pending.end(), found->second.begin(), found->second.end());
                }
            } else if (gates(*boundary)) {
                spans.push_back({boundary->inPoint, boundary->endPoint(duration)});
                continue;
            }
        }
        const auto* hiddenSource = view.boundary(id);
        if (hiddenSource != nullptr && !view.isMatteSource(hiddenSource->layerId))
            hiddenSource = nullptr;
        if (const auto found = outgoing.find(id); found != outgoing.end()) {
            for (const auto* edge : found->second) {
                ++counters.edgesVisited;
                if (hiddenSource != nullptr) {
                    // A matte source's own slot is hidden from its Merge.
                    const auto* slot = std::get_if<LayerStackInputRef>(&edge->destination);
                    const auto* stack = slot ? graph.merge(slot->stackNodeId) : nullptr;
                    const auto* entry = stack ? stack->find(slot->slotId) : nullptr;
                    if (entry != nullptr && entry->layerId == hiddenSource->layerId)
                        continue;
                }
                hasSuccessor = true;
                pending.push_back(destinationNodeOf(edge->destination));
            }
        }
        if (const auto found = drives.find(id); found != drives.end()) {
            hasSuccessor = true;
            pending.insert(pending.end(), found->second.begin(), found->second.end());
        }
        // A reachable node that feeds nothing reachable is a readback root's dependency, not a
        // pixel dependency this walk can bound.
        if (!hasSuccessor)
            return {};
    }
    if (reachedRoot)
        return {.kind = RenderConfinementKind::WholeComposition, .spans = {}};
    return {.kind = RenderConfinementKind::LayerSpans,
            .spans = normalizeRenderTimeSpans(std::move(spans), duration)};
}

std::set<CompositionId> compositionsDependingOn(const Project& project, const CompositionId target,
                                                const NodeDefinitionRegistry& registry) {
    std::set<CompositionId> result{target};
    std::map<CompositionId, std::vector<CompositionId>> referencedBy;
    std::vector<CompositionId> dependsOnEverything;
    for (const auto& composition : project.compositions()) {
        const auto reachability = RenderReachability::of(composition, registry);
        if (!reachability->known() || reachability->nestedReferenceUnknown()) {
            dependsOnEverything.push_back(composition.id());
            continue;
        }
        for (const auto nested : reachability->nestedCompositions())
            referencedBy[nested].push_back(composition.id());
    }
    std::vector<CompositionId> pending{target};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (const auto found = referencedBy.find(id); found != referencedBy.end())
            for (const auto parent : found->second)
                if (result.insert(parent).second)
                    pending.push_back(parent);
    }
    // A composition that may name anything depends on the target and on everything nesting it.
    for (const auto id : dependsOnEverything) {
        if (!result.insert(id).second)
            continue;
        pending.push_back(id);
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            if (const auto found = referencedBy.find(current); found != referencedBy.end())
                for (const auto parent : found->second)
                    if (result.insert(parent).second)
                        pending.push_back(parent);
        }
    }
    return result;
}

} // namespace bloom::document
