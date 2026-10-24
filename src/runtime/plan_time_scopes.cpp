#include "plan_time_scopes.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime::detail {
namespace {

template <class... Ts> struct Visit : Ts... {
    using Ts::operator()...;
};
template <class... Ts> Visit(Ts...) -> Visit<Ts...>;

[[nodiscard]] const CompiledLayerTime* retimed(const CompiledOperation& operation) {
    const auto* layer = std::get_if<CompiledLayerOutput>(&operation);
    return layer != nullptr && layer->time ? &*layer->time : nullptr;
}

// Calls `function(input, clockOfInput)` for every input of `operation` placed on `clock`, where a
// retimed Layer Output feeds its image input from its content clock.
template <typename Function>
void forEachScopedInput(const CompiledOperation& operation, const std::uint32_t clock,
                        const std::uint32_t contentClock, Function&& function) {
    std::visit(Visit{[](const CompiledShape&) {}, [](const CompiledSolid&) {},
                     [](const CompiledChecker&) {}, [](const CompiledText&) {},
                     [](const CompiledImageSource&) {}, [](const CompiledVideoSource&) {},
                     [](const CompiledCompositionSource&) {}, [](const CompiledChannelShuffle&) {},
                     [&](const CompiledImageEffect& effect) {
                         function(effect.input, clock);
                         if (effect.secondaryInput)
                             function(*effect.secondaryInput, clock);
                         if (effect.maskInput)
                             function(*effect.maskInput, clock);
                     },
                     [&](const CompiledLayerOutput& layer) {
                         function(layer.input, layer.time ? contentClock : clock);
                         if (layer.parent)
                             function(*layer.parent, clock);
                     },
                     [&](const CompiledMerge& merge) {
                         for (const auto& entry : merge.entries)
                             function(entry.input, clock);
                     },
                     [&](const CompiledCompositionOutput& output) { function(output.input, clock); }},
               operation);
}

template <typename Function>
void remapInputs(CompiledOperation& operation, Function&& function) {
    std::visit(Visit{[](CompiledShape&) {}, [](CompiledSolid&) {}, [](CompiledChecker&) {},
                     [](CompiledText&) {}, [](CompiledImageSource&) {}, [](CompiledVideoSource&) {},
                     [](CompiledCompositionSource&) {}, [](CompiledChannelShuffle&) {},
                     [&](CompiledImageEffect& effect) {
                         effect.input = function(effect.input, false);
                         if (effect.secondaryInput)
                             effect.secondaryInput = function(*effect.secondaryInput, false);
                         if (effect.maskInput)
                             effect.maskInput = function(*effect.maskInput, false);
                     },
                     [&](CompiledLayerOutput& layer) {
                         layer.input = function(layer.input, true);
                         if (layer.parent)
                             layer.parent = function(*layer.parent, false);
                     },
                     [&](CompiledMerge& merge) {
                         for (auto& entry : merge.entries)
                             entry.input = function(entry.input, false);
                     },
                     [&](CompiledCompositionOutput& output) {
                         output.input = function(output.input, false);
                     }},
               operation);
}

} // namespace

std::optional<TimeScopeFailure> scopeLayerTime(CompiledCompositionPlanDefinition& definition) {
    const auto& operations = definition.operations;
    if (std::ranges::none_of(operations, [](const auto& item) { return retimed(item) != nullptr; }))
        return std::nullopt;

    // Clock 0 is the plan clock. Layer and Content clocks are keyed by (parent clock, layer op).
    std::vector<CompiledTimeClock> clocks{CompiledTimeClock{}};
    std::map<std::tuple<std::uint32_t, std::size_t, CompiledClockKind>, std::uint32_t> clockIds;
    const auto clockFor = [&](const std::uint32_t parent, const std::size_t layer,
                              const CompiledClockKind kind) -> std::optional<std::uint32_t> {
        const auto key = std::tuple{parent, layer, kind};
        if (const auto found = clockIds.find(key); found != clockIds.end())
            return found->second;
        if (clocks.size() >= kMaximumTimeClocks)
            return std::nullopt;
        const auto id = static_cast<std::uint32_t>(clocks.size());
        // `layer` is rewritten to the emitted copy below.
        clocks.push_back({kind, parent, OperationIndex::fromRaw(layer)});
        clockIds.emplace(key, id);
        return id;
    };

    // Which clocks each original operation is needed on, discovered from the output backwards.
    // Bounds readouts address image operations directly and need them on the plan clock.
    std::vector<std::set<std::uint32_t>> needed(operations.size());
    std::vector<std::pair<std::size_t, std::uint32_t>> work{{definition.output.value(), 0}};
    for (const auto& value : definition.valueOperations)
        if (const auto* bounds = std::get_if<CompiledBoundsReadout>(&value.kernel);
            bounds && bounds->operationIndex.value() < operations.size())
            work.emplace_back(bounds->operationIndex.value(), 0);
    // For each (layer op, placement clock): its (layer clock, content clock).
    std::map<std::pair<std::size_t, std::uint32_t>, std::pair<std::uint32_t, std::uint32_t>>
        layerClocks;
    while (!work.empty()) {
        const auto [index, clock] = work.back();
        work.pop_back();
        if (index >= operations.size() || !needed[index].insert(clock).second)
            continue;
        std::uint32_t content = clock;
        if (retimed(operations[index]) != nullptr) {
            const auto layerClock = clockFor(clock, index, CompiledClockKind::Layer);
            const auto contentClock =
                layerClock ? clockFor(*layerClock, index, CompiledClockKind::Content)
                           : std::nullopt;
            if (!contentClock)
                return TimeScopeFailure{"Layer time scopes exceed the supported clock count"};
            layerClocks[{index, clock}] = {*layerClock, *contentClock};
            content = *contentClock;
        }
        forEachScopedInput(operations[index], clock, content,
                           [&](const OperationIndex input, const std::uint32_t inputClock) {
                               work.emplace_back(input.value(), inputClock);
                           });
    }

    std::size_t total = 0;
    for (const auto& clocksOfOperation : needed)
        total += clocksOfOperation.size();
    if (total > kMaximumScopedOperations)
        return TimeScopeFailure{"Layer time scopes exceed the supported operation count"};

    // Emit in the original topological order, one copy per clock, remapping inputs.
    std::map<std::pair<std::size_t, std::uint32_t>, std::size_t> emitted;
    std::vector<CompiledOperation> scoped;
    std::vector<std::uint32_t> operationClocks;
    std::vector<std::uint32_t> placementClocks;
    scoped.reserve(total);
    operationClocks.reserve(total);
    placementClocks.reserve(total);
    for (std::size_t index = 0; index < operations.size(); ++index) {
        for (const auto clock : needed[index]) {
            auto copy = operations[index];
            std::uint32_t operandClock = clock;
            std::uint32_t contentClock = clock;
            if (const auto found = layerClocks.find({index, clock}); found != layerClocks.end()) {
                operandClock = found->second.first;
                contentClock = found->second.second;
            }
            remapInputs(copy, [&](const OperationIndex input, const bool imageInputOfLayer) {
                const auto inputClock =
                    imageInputOfLayer && retimed(operations[index]) ? contentClock : clock;
                return OperationIndex::fromRaw(emitted.at({input.value(), inputClock}));
            });
            emitted[{index, clock}] = scoped.size();
            scoped.push_back(std::move(copy));
            operationClocks.push_back(operandClock);
            placementClocks.push_back(clock);
        }
    }
    // Clocks name the emitted placement copy of the Layer Output that defines them.
    for (auto& entry : clocks) {
        if (entry.kind == CompiledClockKind::Plan)
            continue;
        const auto placement = entry.kind == CompiledClockKind::Layer
                                   ? entry.parent
                                   : clocks[entry.parent].parent;
        entry.layer = OperationIndex::fromRaw(emitted.at({entry.layer.value(), placement}));
    }
    for (auto& value : definition.valueOperations)
        if (auto* bounds = std::get_if<CompiledBoundsReadout>(&value.kernel))
            if (const auto found = emitted.find({bounds->operationIndex.value(), 0});
                found != emitted.end())
                bounds->operationIndex = OperationIndex::fromRaw(found->second);
    definition.output = OperationIndex::fromRaw(emitted.at({definition.output.value(), 0}));
    definition.operations = std::move(scoped);
    definition.clocks = std::move(clocks);
    definition.operationClocks = std::move(operationClocks);
    definition.placementClocks = std::move(placementClocks);
    return std::nullopt;
}

} // namespace bloom::runtime::detail
