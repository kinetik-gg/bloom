#include <bloom/document/node_definition_registry.hpp>

#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace {

using bloom::document::InputPortDefinition;
using bloom::document::LayerSlotInputDefinition;
using bloom::document::NodeCardinality;
using bloom::document::NodeCategory;
using bloom::document::NodeDefinition;
using bloom::document::NodeLoweringKind;
using bloom::document::OutputPortDefinition;
using bloom::document::ParameterDefinition;
using bloom::document::ParameterValueKind;
using bloom::document::SocketValueKind;

struct NodeLookupKey {
    std::string_view typeId;
    std::uint32_t schemaVersion = 0;
};

[[nodiscard]] bool definitionPrecedesKey(const NodeDefinition& definition,
                                         const NodeLookupKey key) noexcept {
    const std::string_view definitionType{definition.key.typeId};
    return definitionType < key.typeId ||
           (definitionType == key.typeId && definition.key.schemaVersion < key.schemaVersion);
}

template <typename Definition>
[[nodiscard]] bool hasUniqueNonEmptyNames(const std::vector<Definition>& definitions) {
    std::unordered_set<std::string_view> names;
    for (const auto& definition : definitions) {
        if (definition.name.empty() || !names.insert(definition.name).second) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasValidParameters(const std::vector<ParameterDefinition>& definitions) {
    std::unordered_set<std::string_view> roles;
    for (const auto& definition : definitions) {
        if (definition.role.empty() || definition.schemaKey.empty() ||
            !roles.insert(definition.role).second) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasImageInput(const NodeDefinition& definition,
                                 const std::string_view name) noexcept {
    return definition.inputs.size() == 1 && definition.inputs.front().name == name &&
           definition.inputs.front().valueKind == SocketValueKind::Image &&
           definition.inputs.front().required;
}

[[nodiscard]] bool hasImageOutput(const NodeDefinition& definition,
                                  const std::string_view name) noexcept {
    return definition.outputs.size() == 1 && definition.outputs.front().name == name &&
           definition.outputs.front().valueKind == SocketValueKind::Image;
}

[[nodiscard]] bool hasParameter(const NodeDefinition& definition, const std::size_t index,
                                const std::string_view role, const std::string_view schemaKey,
                                const ParameterValueKind valueKind,
                                const bool supportsAnimation = false) noexcept {
    return definition.parameters.size() > index && definition.parameters[index].role == role &&
           definition.parameters[index].schemaKey == schemaKey &&
           definition.parameters[index].valueKind == valueKind &&
           definition.parameters[index].required &&
           definition.parameters[index].supportsAnimation == supportsAnimation;
}

[[nodiscard]] bool hasCanonicalKey(const NodeDefinition& definition, const std::string_view typeId,
                                   const std::uint32_t schemaVersion) noexcept {
    return definition.key.typeId == typeId && definition.key.schemaVersion == schemaVersion;
}

[[nodiscard]] bool hasValidLoweringShape(const NodeDefinition& definition) noexcept {
    using namespace bloom::document;
    switch (definition.lowering) {
    case NodeLoweringKind::Solid:
        return definition.inputs.empty() && hasImageOutput(definition, kSolidSourceOutputPort) &&
               definition.parameters.size() == 1 &&
               hasParameter(definition, 0, kSolidColorParameterRole, kSolidColorParameterSchemaKey,
                            ParameterValueKind::Color4d) &&
               !definition.layerSlotInput.has_value();
    case NodeLoweringKind::Text:
        // Parameter ORDER is part of the shape, like every other lowering here: content, then size,
        // then color. The font is not a parameter -- this lowering has exactly one face
        // (src/render's embedded DejaVu Sans), so a font parameter would promise a selection the
        // renderer cannot honor.
        return hasCanonicalKey(definition, kTextSourceNodeType, kTextSourceNodeSchemaVersion) &&
               definition.inputs.empty() && hasImageOutput(definition, kTextSourceOutputPort) &&
               definition.parameters.size() == 3 &&
               hasParameter(definition, 0, kTextParameterRole, kTextParameterSchemaKey,
                            ParameterValueKind::String) &&
               hasParameter(definition, 1, kTextSizeParameterRole, kTextSizeParameterSchemaKey,
                            ParameterValueKind::Float64) &&
               hasParameter(definition, 2, kTextColorParameterRole, kTextColorParameterSchemaKey,
                            ParameterValueKind::Color4d) &&
               !definition.layerSlotInput.has_value();
    case NodeLoweringKind::LayerOutput:
        // Parameter ORDER is the authoring order the properties grid, the node card, and the
        // timeline all read: where the layer sits, the point it turns about, how big it is, how far
        // round it is turned, then how much of it shows through. Opacity stays last because it is
        // the only one of the five that is not part of the geometric transform.
        return hasCanonicalKey(definition, kLayerOutputNodeType, kLayerOutputNodeSchemaVersion) &&
               hasImageInput(definition, kLayerOutputContentInputPort) &&
               hasImageOutput(definition, kLayerOutputOutputPort) &&
               definition.parameters.size() == 5 &&
               hasParameter(definition, 0, kPositionParameterRole, kPositionParameterSchemaKey,
                            ParameterValueKind::Vec2d, true) &&
               hasParameter(definition, 1, kAnchorParameterRole, kAnchorParameterSchemaKey,
                            ParameterValueKind::Vec2d, true) &&
               hasParameter(definition, 2, kScaleParameterRole, kScaleParameterSchemaKey,
                            ParameterValueKind::Vec2d, true) &&
               hasParameter(definition, 3, kRotationParameterRole, kRotationParameterSchemaKey,
                            ParameterValueKind::Float64, true) &&
               hasParameter(definition, 4, kOpacityParameterRole, kOpacityParameterSchemaKey,
                            ParameterValueKind::Float64, true) &&
               !definition.layerSlotInput.has_value();
    case NodeLoweringKind::LayerStack:
        return hasCanonicalKey(definition, kLayerStackNodeType, kLayerStackNodeSchemaVersion) &&
               definition.cardinality == NodeCardinality::OnePerComposition &&
               definition.inputs.empty() && hasImageOutput(definition, kLayerStackOutputPort) &&
               definition.parameters.empty() && definition.layerSlotInput.has_value() &&
               definition.layerSlotInput->role == kLayerStackContentInputRole &&
               definition.layerSlotInput->valueKind == SocketValueKind::Image &&
               definition.layerSlotInput->requiredPerSlot;
    case NodeLoweringKind::CompositionOutput:
        return hasCanonicalKey(definition, kCompositionOutputNodeType,
                               kCompositionOutputNodeSchemaVersion) &&
               definition.cardinality == NodeCardinality::OnePerComposition &&
               hasImageInput(definition, kCompositionOutputInputPort) &&
               hasImageOutput(definition, kCompositionOutputOutputPort) &&
               definition.parameters.empty() && !definition.layerSlotInput.has_value();
    case NodeLoweringKind::Unsupported:
        return true;
    }
    return false;
}

[[nodiscard]] bool isValidDefinition(const NodeDefinition& definition) {
    if (definition.key.typeId.empty() || definition.key.schemaVersion == 0 ||
        !hasUniqueNonEmptyNames(definition.inputs) || !hasUniqueNonEmptyNames(definition.outputs) ||
        !hasValidParameters(definition.parameters)) {
        return false;
    }
    if (definition.layerSlotInput.has_value() && definition.layerSlotInput->role.empty()) {
        return false;
    }
    return hasValidLoweringShape(definition);
}

[[nodiscard]] NodeDefinition solidDefinition() {
    using namespace bloom::document;
    return {{std::string(kSolidSourceNodeType), kSolidSourceNodeSchemaVersion},
            NodeLoweringKind::Solid,
            {},
            {{std::string(kSolidSourceOutputPort), SocketValueKind::Image}},
            {{std::string(kSolidColorParameterRole), std::string(kSolidColorParameterSchemaKey),
              ParameterValueKind::Color4d, true, false, bloom::core::Color4d{1.0, 1.0, 1.0, 1.0}}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Sources};
}

[[nodiscard]] NodeDefinition layerOutputDefinition() {
    using namespace bloom::document;
    return {{std::string(kLayerOutputNodeType), kLayerOutputNodeSchemaVersion},
            NodeLoweringKind::LayerOutput,
            {{std::string(kLayerOutputContentInputPort), SocketValueKind::Image, true}},
            {{std::string(kLayerOutputOutputPort), SocketValueKind::Image}},
            {{std::string(kPositionParameterRole), std::string(kPositionParameterSchemaKey),
              ParameterValueKind::Vec2d, true, true, Vec2d{}},
             {std::string(kAnchorParameterRole), std::string(kAnchorParameterSchemaKey),
              ParameterValueKind::Vec2d, true, true, kDefaultAnchor},
             {std::string(kScaleParameterRole), std::string(kScaleParameterSchemaKey),
              ParameterValueKind::Vec2d, true, true, kDefaultScale},
             {std::string(kRotationParameterRole), std::string(kRotationParameterSchemaKey),
              ParameterValueKind::Float64, true, true, kDefaultRotationDegrees},
             {std::string(kOpacityParameterRole), std::string(kOpacityParameterSchemaKey),
              ParameterValueKind::Float64, true, true, 1.0}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Layers};
}

[[nodiscard]] NodeDefinition layerStackDefinition() {
    using namespace bloom::document;
    return {{std::string(kLayerStackNodeType), kLayerStackNodeSchemaVersion},
            NodeLoweringKind::LayerStack,
            {},
            {{std::string(kLayerStackOutputPort), SocketValueKind::Image}},
            {},
            LayerSlotInputDefinition{std::string(kLayerStackContentInputRole),
                                     SocketValueKind::Image, true},
            NodeCardinality::OnePerComposition,
            NodeCategory::Compositing};
}

[[nodiscard]] NodeDefinition compositionOutputDefinition() {
    using namespace bloom::document;
    return {{std::string(kCompositionOutputNodeType), kCompositionOutputNodeSchemaVersion},
            NodeLoweringKind::CompositionOutput,
            {{std::string(kCompositionOutputInputPort), SocketValueKind::Image, true}},
            {{std::string(kCompositionOutputOutputPort), SocketValueKind::Image}},
            {},
            std::nullopt,
            NodeCardinality::OnePerComposition,
            NodeCategory::Output};
}

[[nodiscard]] NodeDefinition textDefinition() {
    using namespace bloom::document;
    return {{std::string(kTextSourceNodeType), kTextSourceNodeSchemaVersion},
            NodeLoweringKind::Text,
            {},
            {{std::string(kTextSourceOutputPort), SocketValueKind::Image}},
            {{std::string(kTextParameterRole), std::string(kTextParameterSchemaKey),
              ParameterValueKind::String, true, false, std::string{}},
             {std::string(kTextSizeParameterRole), std::string(kTextSizeParameterSchemaKey),
              ParameterValueKind::Float64, true, false, kDefaultTextSizePixels},
             {std::string(kTextColorParameterRole), std::string(kTextColorParameterSchemaKey),
              ParameterValueKind::Color4d, true, false, bloom::core::Color4d{1.0, 1.0, 1.0, 1.0}}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Sources};
}

} // namespace

namespace bloom::document {

NodeRegistrationStatus NodeDefinitionRegistry::registerDefinition(NodeDefinition definition) {
    if (frozen_) {
        return NodeRegistrationStatus::Frozen;
    }
    if (!isValidDefinition(definition)) {
        return NodeRegistrationStatus::InvalidDefinition;
    }
    const auto duplicate = std::ranges::find_if(
        definitions_, [&](const auto& existing) { return existing.key == definition.key; });
    if (duplicate != definitions_.end()) {
        return NodeRegistrationStatus::DuplicateDefinition;
    }
    definitions_.push_back(std::move(definition));
    return NodeRegistrationStatus::Registered;
}

void NodeDefinitionRegistry::freeze() {
    if (frozen_) {
        return;
    }
    std::ranges::sort(definitions_, [](const auto& left, const auto& right) {
        if (left.key.typeId != right.key.typeId) {
            return left.key.typeId < right.key.typeId;
        }
        return left.key.schemaVersion < right.key.schemaVersion;
    });
    frozen_ = true;
}

const NodeDefinition*
NodeDefinitionRegistry::find(const std::string_view typeId,
                             const std::uint32_t schemaVersion) const noexcept {
    const auto match =
        frozen_ ? std::lower_bound(definitions_.begin(), definitions_.end(),
                                   NodeLookupKey{typeId, schemaVersion}, definitionPrecedesKey)
                : std::ranges::find_if(definitions_, [&](const auto& definition) {
                      return definition.key.typeId == typeId &&
                             definition.key.schemaVersion == schemaVersion;
                  });
    if (match == definitions_.end() || match->key.typeId != typeId ||
        match->key.schemaVersion != schemaVersion) {
        return nullptr;
    }
    return &*match;
}

bool NodeDefinitionRegistry::containsType(const std::string_view typeId) const noexcept {
    if (!frozen_) {
        return std::ranges::any_of(
            definitions_, [&](const auto& definition) { return definition.key.typeId == typeId; });
    }
    const auto match = std::lower_bound(
        definitions_.begin(), definitions_.end(), typeId,
        [](const NodeDefinition& definition, const std::string_view candidate) noexcept {
            return std::string_view{definition.key.typeId} < candidate;
        });
    return match != definitions_.end() && match->key.typeId == typeId;
}

bool registerBuiltInNodeDefinitions(NodeDefinitionRegistry& registry) {
    std::array definitions{solidDefinition(), layerOutputDefinition(), layerStackDefinition(),
                           compositionOutputDefinition(), textDefinition()};
    if (registry.isFrozen() || std::ranges::any_of(definitions, [&](const auto& definition) {
            return registry.find(definition.key.typeId, definition.key.schemaVersion) != nullptr;
        })) {
        return false;
    }
    for (auto& definition : definitions) {
        if (registry.registerDefinition(std::move(definition)) !=
            NodeRegistrationStatus::Registered) {
            return false;
        }
    }
    return true;
}

const NodeDefinitionRegistry& builtInNodeDefinitions() {
    struct BuiltIns final {
        NodeDefinitionRegistry registry;
        BuiltIns() {
            (void)registerBuiltInNodeDefinitions(registry);
            registry.freeze();
        }
    };
    static const BuiltIns builtIns;
    return builtIns.registry;
}

} // namespace bloom::document
