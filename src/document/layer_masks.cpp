#include <bloom/document/layer_masks.hpp>

#include <bloom/document/persisted_text.hpp>

#include <cmath>
#include <string>
#include <unordered_set>

namespace bloom::document {
namespace {

[[nodiscard]] bool validMode(const LayerMaskMode mode) noexcept {
    return mode == LayerMaskMode::None || mode == LayerMaskMode::Add ||
           mode == LayerMaskMode::Subtract || mode == LayerMaskMode::Intersect ||
           mode == LayerMaskMode::Difference;
}

[[nodiscard]] bool constantMatches(const std::string_view schemaKey,
                                   const ParameterValue& value) noexcept {
    if (schemaKey == kLayerMaskPathSchemaKey) {
        const auto* path = std::get_if<PathValue>(&value);
        return path != nullptr && path->isValid();
    }
    const auto* number = std::get_if<double>(&value);
    return number != nullptr && std::isfinite(*number) &&
           isScalarWithinSchemaDomain(schemaKey, *number);
}

} // namespace

NodeDefinition maskDefinition() {
    NodeDefinition node{{std::string(kMaskNodeType), 1},
                        NodeLoweringKind::Mask,
                        {},
                        {{std::string(kMaskNodeOutputPort), SocketValueKind::Mask}},
                        {},
                        std::nullopt,
                        NodeCardinality::Many,
                        NodeCategory::Compositing};
    node.parameters.push_back({"path", std::string(kLayerMaskPathSchemaKey),
                               ParameterValueKind::Path, true, true, PathValue{}});
    const auto scalar = [&](std::string role, std::string_view key, const double value) {
        node.parameters.push_back(
            {role, std::string(key), ParameterValueKind::Float64, true, true, value});
        node.inputs.push_back({std::move(role), SocketValueKind::Scalar, false});
    };
    scalar("feather", kLayerMaskFeatherSchemaKey, 0.0);
    scalar("expansion", kLayerMaskExpansionSchemaKey, 0.0);
    scalar("opacity", kLayerMaskOpacitySchemaKey, 1.0);
    node.parameters.push_back({"inverted", std::string(kLayerMaskInvertedSchemaKey),
                               ParameterValueKind::Boolean, true, false, false});
    return node;
}

bool isLayerMaskParameterSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kLayerMaskPathSchemaKey || schemaKey == kLayerMaskFeatherSchemaKey ||
           schemaKey == kLayerMaskExpansionSchemaKey || schemaKey == kLayerMaskOpacitySchemaKey;
}

std::array<std::pair<ParameterId, std::string_view>, 4>
layerMaskParameters(const LayerMask& mask) noexcept {
    return {{{mask.path, kLayerMaskPathSchemaKey},
             {mask.feather, kLayerMaskFeatherSchemaKey},
             {mask.expansion, kLayerMaskExpansionSchemaKey},
             {mask.opacity, kLayerMaskOpacitySchemaKey}}};
}

ValidationResult validateLayerMasks(const CanonicalGraph& graph, const ParameterStore& parameters) {
    ValidationResult result;
    std::unordered_set<ParameterId> nodeBound;
    for (const auto& node : graph.nodes())
        for (const auto& binding : node.parameters)
            nodeBound.insert(binding.parameterId);
    std::unordered_set<LayerMaskId> maskIds;
    std::unordered_set<ParameterId> maskOwned;
    for (const auto& layer : graph.layerOutputs()) {
        const auto layerPath = "graph.layerOutputs[" + std::to_string(layer.layerId.value()) + "]";
        if (layer.masks.size() > kMaximumLayerMasks)
            result.add(ValidationCode::InvalidValue, layerPath + ".masks",
                       "A layer may hold at most 64 masks");
        for (const auto& mask : layer.masks) {
            const auto path = layerPath + ".masks[" + std::to_string(mask.id.value()) + "]";
            if (!mask.id.isValid())
                result.add(ValidationCode::InvalidId, path + ".id", "Mask ID must not be zero");
            else if (!maskIds.insert(mask.id).second)
                result.add(ValidationCode::DuplicateId, path + ".id",
                           "Mask ID is duplicated within the composition");
            validateHumanFacingName(mask.name, path + ".name", "Mask name", result);
            if (!validMode(mask.mode))
                result.add(ValidationCode::InvalidValue, path + ".mode",
                           "Mask mode is not a known value");
            for (const auto& [id, schemaKey] : layerMaskParameters(mask)) {
                const auto parameterPath = path + "." + std::string(schemaKey);
                const auto* record = parameters.find(id);
                if (record == nullptr) {
                    result.add(ValidationCode::MissingReference, parameterPath,
                               "Mask parameter does not resolve to a parameter");
                    continue;
                }
                if (record->schemaKey != schemaKey)
                    result.add(ValidationCode::TypeMismatch, parameterPath,
                               "Mask parameter has the wrong schema");
                if (nodeBound.contains(id) || !maskOwned.insert(id).second)
                    result.add(ValidationCode::SharedReference, parameterPath,
                               "A mask parameter is owned by exactly one mask");
                if (const auto* constant = std::get_if<ConstantValueSource>(&record->source)) {
                    if (!constantMatches(schemaKey, constant->value))
                        result.add(ValidationCode::InvalidValue, parameterPath,
                                   "Mask parameter value is outside its schema domain");
                } else if (!std::holds_alternative<AnimationCurveSource>(record->source)) {
                    result.add(ValidationCode::InvalidValue, parameterPath,
                               "A mask parameter is a constant or an animation curve");
                }
            }
        }
    }
    return result;
}

} // namespace bloom::document
