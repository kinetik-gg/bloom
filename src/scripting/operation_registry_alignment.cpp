// ADR 0031: bloom.layer.align-layers and bloom.layer.distribute-layers. Unlike every other
// operation, these need evaluated geometry: the factory measures the layers through the context's
// LayerGeometryProvider (the headless CPU reference query) when the operation is created, before it
// is queued, so the command itself stays evaluation-free. The geometry is the snapshot the
// transaction is created against; layers an earlier operation of the same transaction adds or moves
// are measured as they were before it.

#include <bloom/commands/layer_alignment_operations.hpp>
#include <bloom/scripting/operation_registry.hpp>

#include "operation_values.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bloom::scripting::detail {
namespace {

[[nodiscard]] ArgumentSchema argument(std::string name, const ValueKind kind, const bool required,
                                      const bool inExample, const ContextSource context,
                                      Value example, std::string summary) {
    return {.name = std::move(name),
            .kind = kind,
            .required = required,
            .inExample = inExample,
            .context = context,
            .example = std::move(example),
            .summary = std::move(summary)};
}

[[nodiscard]] std::vector<ArgumentSchema> schema(ArgumentSchema arrangement) {
    return {
        argument("composition", ValueKind::Id, true, false, ContextSource::Composition, StableId{1},
                 "The composition to edit. Defaults to the current composition."),
        std::move(arrangement),
        argument("target", ValueKind::String, false, false, ContextSource::None, "selection",
                 R"(What to line up with: "selection" (the union of the layers' bounds), )"
                 R"("composition" (the composition frame) or "key-object" (the key layer's )"
                 R"(bounds). Defaults to "selection".)"),
        argument("key", ValueKind::Id, false, false, ContextSource::None, StableId{1},
                 R"(The key layer for target "key-object". It never moves.)"),
        argument("layers", ValueKind::Array, true, false, ContextSource::Layers,
                 ValueArray{Value(StableId{1})},
                 "The layers to move. Defaults to the selected layers."),
        argument("time", ValueKind::Time, true, false, ContextSource::Time, std::int64_t{0},
                 "The time whose evaluated bounds are aligned. Defaults to the current playhead "
                 "time."),
    };
}

[[nodiscard]] const Value* find(const Arguments& arguments, const std::string& name) {
    const auto iterator = arguments.find(name);
    return iterator == arguments.end() ? nullptr : &iterator->second;
}

[[nodiscard]] OperationCreateResult create(const std::string& op, const Arguments& args,
                                           const OperationContext& context,
                                           const bool distribution) {
    const auto composition = idOf(*find(args, "composition"));
    if (!composition)
        return argumentFailure(op, "composition", "expects a positive stable id or a live proxy");
    const auto time = rationalOf(*find(args, "time"));
    if (!time)
        return argumentFailure(op, "time",
                               "expects a whole frame or an exact (numerator, denominator) time");

    std::vector<document::LayerId> layers;
    const auto* list = std::get_if<ValueArray>(&find(args, "layers")->storage);
    if (list == nullptr || list->empty() || list->size() > 4096)
        return argumentFailure(op, "layers", "expects 1..4096 stable ids or live proxies");
    for (const auto& entry : *list) {
        const auto layer = idOf(entry);
        if (!layer)
            return argumentFailure(op, "layers", "expects 1..4096 stable ids or live proxies");
        layers.push_back(document::LayerId::fromRaw(*layer));
    }

    std::optional<commands::LayerArrangement> arrangement;
    if (distribution) {
        const auto* axis = std::get_if<std::string>(&find(args, "axis")->storage);
        if (axis != nullptr && *axis == "horizontal")
            arrangement = commands::LayerArrangement::DistributeHorizontalCentres;
        else if (axis != nullptr && *axis == "vertical")
            arrangement = commands::LayerArrangement::DistributeVerticalCentres;
        if (!arrangement)
            return argumentFailure(op, "axis", R"(expects "horizontal" or "vertical")");
    } else {
        const auto* edge = std::get_if<std::string>(&find(args, "edge")->storage);
        arrangement = edge ? commands::layerArrangementFromId(*edge) : std::nullopt;
        if (!arrangement || commands::isDistribution(*arrangement))
            return argumentFailure(op, "edge",
                                   R"(expects "left", "horizontal-centre", "right", "top", )"
                                   R"("vertical-middle" or "bottom")");
    }

    auto target = commands::AlignmentTarget::Selection;
    if (const auto* value = find(args, "target")) {
        const auto* name = std::get_if<std::string>(&value->storage);
        const auto parsed = name ? commands::alignmentTargetFromId(*name) : std::nullopt;
        if (!parsed)
            return argumentFailure(op, "target",
                                   R"(expects "selection", "composition" or "key-object")");
        target = *parsed;
    }
    std::optional<document::LayerId> key;
    if (const auto* value = find(args, "key")) {
        const auto parsed = idOf(*value);
        if (!parsed)
            return argumentFailure(op, "key", "expects a positive stable id or a live proxy");
        key = document::LayerId::fromRaw(*parsed);
    }
    if (target == commands::AlignmentTarget::KeyObject && !key)
        return argumentFailure(op, "key", R"(is required when target is "key-object")");

    commands::LayerAlignmentRequest request{
        .composition = document::CompositionId::fromRaw(*composition),
        .time = *time,
        .layers = std::move(layers),
        .arrangement = *arrangement,
        .target = target,
        .keyLayer = target == commands::AlignmentTarget::KeyObject ? key : std::nullopt,
        .geometry = {}};
    if (context.layerGeometry) {
        auto measured = context.layerGeometry(request.composition, request.time);
        if (!measured.succeeded)
            return OperationCreateResult(
                nullptr, OperationDiagnostic{.code = "bloom.scripting.layer-bounds-unavailable",
                                             .operationId = op,
                                             .argument = {},
                                             .message = "could not measure the layers: " +
                                                        measured.diagnostic});
        request.geometry = std::move(measured.geometry);
    }
    return OperationCreateResult(std::make_unique<commands::AlignLayers>(std::move(request)),
                                 std::nullopt);
}

void add(std::vector<OperationDescriptor>& descriptors, std::string id, ArgumentSchema arrangement,
         const bool distribution) {
    descriptors.push_back(
        {.schema = {.typeId = std::move(id), .arguments = schema(std::move(arrangement))},
         .factory =
             [distribution](const std::string& op, const Arguments& args) {
                 return create(op, args, OperationContext{}, distribution);
             },
         .contextualFactory =
             [distribution](const std::string& op, const Arguments& args,
                            const OperationContext& context) {
                 return create(op, args, context, distribution);
             }});
}

} // namespace

void addLayerAlignmentOperations(std::vector<OperationDescriptor>& descriptors) {
    add(descriptors, "bloom.layer.align-layers",
        argument("edge", ValueKind::String, true, true, ContextSource::None, "left",
                 R"(The edge or centre to line up: "left", "horizontal-centre", "right", "top", )"
                 R"("vertical-middle" or "bottom".)"),
        false);
    add(descriptors, "bloom.layer.distribute-layers",
        argument("axis", ValueKind::String, true, true, ContextSource::None, "horizontal",
                 R"(Space the layers' centres evenly along "horizontal" or "vertical". Needs )"
                 R"(three or more layers.)"),
        true);
}

} // namespace bloom::scripting::detail
