#include <bloom/runtime/node_definition_registry.hpp>

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

using bloom::runtime::InputPortDefinition;
using bloom::runtime::LayerSlotInputDefinition;
using bloom::runtime::NodeDefinition;
using bloom::runtime::NodeLoweringKind;
using bloom::runtime::OutputPortDefinition;
using bloom::runtime::ParameterDefinition;
using bloom::runtime::ParameterValueKind;
using bloom::runtime::SocketValueKind;

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
    case NodeLoweringKind::LayerOutput:
        return hasCanonicalKey(definition, kLayerOutputNodeType, kLayerOutputNodeSchemaVersion) &&
               hasImageInput(definition, kLayerOutputContentInputPort) &&
               hasImageOutput(definition, kLayerOutputOutputPort) &&
               definition.parameters.size() == 2 &&
               hasParameter(definition, 0, kPositionParameterRole, kPositionParameterSchemaKey,
                            ParameterValueKind::Vec2d, true) &&
               hasParameter(definition, 1, kOpacityParameterRole, kOpacityParameterSchemaKey,
                            ParameterValueKind::Float64, true) &&
               !definition.layerSlotInput.has_value();
    case NodeLoweringKind::LayerStack:
        return hasCanonicalKey(definition, kLayerStackNodeType, kLayerStackNodeSchemaVersion) &&
               definition.inputs.empty() && hasImageOutput(definition, kLayerStackOutputPort) &&
               definition.parameters.empty() && definition.layerSlotInput.has_value() &&
               definition.layerSlotInput->role == kLayerStackContentInputRole &&
               definition.layerSlotInput->valueKind == SocketValueKind::Image &&
               definition.layerSlotInput->requiredPerSlot;
    case NodeLoweringKind::CompositionOutput:
        return hasCanonicalKey(definition, kCompositionOutputNodeType,
                               kCompositionOutputNodeSchemaVersion) &&
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
              ParameterValueKind::Color4d, true}},
            std::nullopt};
}

[[nodiscard]] NodeDefinition layerOutputDefinition() {
    using namespace bloom::document;
    return {{std::string(kLayerOutputNodeType), kLayerOutputNodeSchemaVersion},
            NodeLoweringKind::LayerOutput,
            {{std::string(kLayerOutputContentInputPort), SocketValueKind::Image, true}},
            {{std::string(kLayerOutputOutputPort), SocketValueKind::Image}},
            {{std::string(kPositionParameterRole), std::string(kPositionParameterSchemaKey),
              ParameterValueKind::Vec2d, true, true},
             {std::string(kOpacityParameterRole), std::string(kOpacityParameterSchemaKey),
              ParameterValueKind::Float64, true, true}},
            std::nullopt};
}

[[nodiscard]] NodeDefinition layerStackDefinition() {
    using namespace bloom::document;
    return {{std::string(kLayerStackNodeType), kLayerStackNodeSchemaVersion},
            NodeLoweringKind::LayerStack,
            {},
            {{std::string(kLayerStackOutputPort), SocketValueKind::Image}},
            {},
            LayerSlotInputDefinition{std::string(kLayerStackContentInputRole),
                                     SocketValueKind::Image, true}};
}

[[nodiscard]] NodeDefinition compositionOutputDefinition() {
    using namespace bloom::document;
    return {{std::string(kCompositionOutputNodeType), kCompositionOutputNodeSchemaVersion},
            NodeLoweringKind::CompositionOutput,
            {{std::string(kCompositionOutputInputPort), SocketValueKind::Image, true}},
            {{std::string(kCompositionOutputOutputPort), SocketValueKind::Image}},
            {},
            std::nullopt};
}

[[nodiscard]] NodeDefinition textDefinition() {
    using namespace bloom::document;
    return {{std::string(kTextSourceNodeType), kTextSourceNodeSchemaVersion},
            NodeLoweringKind::Unsupported,
            {},
            {{std::string(kTextSourceOutputPort), SocketValueKind::Image}},
            {{std::string(kTextParameterRole), std::string(kTextParameterSchemaKey),
              ParameterValueKind::String, true}},
            std::nullopt};
}

} // namespace

namespace bloom::runtime {

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

} // namespace bloom::runtime
