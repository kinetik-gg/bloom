#include <bloom/document/node_definition_registry.hpp>

#include "value_node_definitions.hpp"

#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

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

// The node's image transport input, which is always the FIRST one: task S7 gave every parameter
// role its own linkable socket, so a Layer Output's input list is now the content image followed by
// one operand socket per transform value. The image port's position is what the dissolve gesture,
// the mute bypass and the empty-image propagation all read, so it stays pinned at the front.
[[nodiscard]] bool hasLeadingImageInput(const NodeDefinition& definition,
                                        const std::string_view name,
                                        const bool required = true) noexcept {
    return !definition.inputs.empty() && definition.inputs.front().name == name &&
           definition.inputs.front().valueKind == SocketValueKind::Image &&
           definition.inputs.front().required == required;
}

// Task S7, item 3: every parameter role of a node is ALSO a linkable input socket of its kind.
// Checked here, generically, so no lowering can declare a parameter the editor cannot link or a
// socket that writes nothing -- and so the rule has one statement rather than one per lowering.
// `imageInputCount` is how many leading image transport ports precede the operand block.
[[nodiscard]] bool hasParameterSockets(const NodeDefinition& definition,
                                       const std::size_t imageInputCount) noexcept {
    if (definition.inputs.size() != imageInputCount + definition.parameters.size()) {
        return false;
    }
    for (std::size_t index = 0; index < definition.parameters.size(); ++index) {
        const auto& declared = definition.parameters[index];
        const auto& socket = definition.inputs[imageInputCount + index];
        if (socket.name != declared.role || socket.required ||
            socket.valueKind != socketKindForParameterValueKind(declared.valueKind)) {
            return false;
        }
    }
    return true;
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
        return hasImageOutput(definition, kSolidSourceOutputPort) &&
               definition.parameters.size() == 1 &&
               hasParameter(definition, 0, kSolidColorParameterRole, kSolidColorParameterSchemaKey,
                            ParameterValueKind::Color4d,
                            isAnimatableSchemaKey(kSolidColorParameterSchemaKey)) &&
               hasParameterSockets(definition, 0) && !definition.layerSlotInput.has_value();
    case NodeLoweringKind::Text:
        // Parameter ORDER is part of the shape, like every other lowering here: content, then size,
        // then color. The font is not a parameter -- this lowering has exactly one face
        // (src/render's embedded DejaVu Sans), so a font parameter would promise a selection the
        // renderer cannot honor.
        return hasCanonicalKey(definition, kTextSourceNodeType, kTextSourceNodeSchemaVersion) &&
               hasImageOutput(definition, kTextSourceOutputPort) &&
               definition.parameters.size() == 3 &&
               hasParameter(definition, 0, kTextParameterRole, kTextParameterSchemaKey,
                            ParameterValueKind::String,
                            isAnimatableSchemaKey(kTextParameterSchemaKey)) &&
               hasParameter(definition, 1, kTextSizeParameterRole, kTextSizeParameterSchemaKey,
                            ParameterValueKind::Float64,
                            isAnimatableSchemaKey(kTextSizeParameterSchemaKey)) &&
               hasParameter(definition, 2, kTextColorParameterRole, kTextColorParameterSchemaKey,
                            ParameterValueKind::Color4d,
                            isAnimatableSchemaKey(kTextColorParameterSchemaKey)) &&
               hasParameterSockets(definition, 0) && !definition.layerSlotInput.has_value();
    case NodeLoweringKind::LayerOutput:
        // Parameter ORDER is the authoring order the properties grid, the node card, and the
        // timeline all read: where the layer sits, the point it turns about, how big it is, how far
        // round it is turned, how much of it shows through, then how it combines with what is
        // beneath it. The two appearance values come after the four geometric ones, and the blend
        // mode comes last because it is the only one that is not a continuous value at all.
        return hasCanonicalKey(definition, kLayerOutputNodeType, kLayerOutputNodeSchemaVersion) &&
               hasLeadingImageInput(definition, kLayerOutputContentInputPort, false) &&
               hasImageOutput(definition, kLayerOutputOutputPort) &&
               definition.parameters.size() == 6 &&
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
               hasParameter(definition, 5, kBlendModeParameterRole, kBlendModeParameterSchemaKey,
                            ParameterValueKind::Integer) &&
               hasParameterSockets(definition, 1) && !definition.layerSlotInput.has_value();
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
               hasLeadingImageInput(definition, kCompositionOutputInputPort) &&
               definition.inputs.size() == 1 &&
               hasImageOutput(definition, kCompositionOutputOutputPort) &&
               definition.parameters.empty() && !definition.layerSlotInput.has_value();
    case NodeLoweringKind::Unsupported:
        return true;
    // The value lowerings share ONE shape contract rather than fifteen bespoke ones; see
    // value_node_definitions.cpp for what it requires and why the asymmetry with the five image
    // lowerings above is deliberate.
    case NodeLoweringKind::ValueConstant:
    case NodeLoweringKind::ValueTime:
    case NodeLoweringKind::ValueScalarMath:
    case NodeLoweringKind::ValueVectorMath:
    case NodeLoweringKind::ValueVectorReduce:
    case NodeLoweringKind::ValueMapRange:
    case NodeLoweringKind::ValueClamp:
    case NodeLoweringKind::ValueMix:
    case NodeLoweringKind::ValueColorMix:
    case NodeLoweringKind::ValueCompare:
    case NodeLoweringKind::ValueSwitch:
    case NodeLoweringKind::ValueSeparate:
    case NodeLoweringKind::ValueCombine:
    case NodeLoweringKind::ValueRandom:
    case NodeLoweringKind::ValueReroute:
        return bloom::document::detail::hasValidValueLoweringShape(definition);
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
            // Task S7, item 3: the colour parameter is a linkable socket as well as an inline chip.
            // Optional, because the parameter IS the value when nothing is connected -- an
            // unconnected operand is an authored constant, not a missing input.
            {{std::string(kSolidColorParameterRole), SocketValueKind::Color, false}},
            {{std::string(kSolidSourceOutputPort), SocketValueKind::Image}},
            // Task S5, item 1: supportsAnimation is true now. It is NOT a second opinion about what
            // is animatable -- the shape check below asserts it agrees with
            // document::isColor4AnimatableSchemaKey(), so the schema predicates stay the single
            // authority and a registered definition cannot drift from them.
            {{std::string(kSolidColorParameterRole), std::string(kSolidColorParameterSchemaKey),
              ParameterValueKind::Color4d, true, true, bloom::core::Color4d{1.0, 1.0, 1.0, 1.0}}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Sources};
}

[[nodiscard]] NodeDefinition layerOutputDefinition() {
    using namespace bloom::document;
    return {{std::string(kLayerOutputNodeType), kLayerOutputNodeSchemaVersion},
            NodeLoweringKind::LayerOutput,
            // The content image first -- its position is what the dissolve gesture and the mute
            // bypass read -- then one operand socket per transform parameter, in the registered
            // authoring order (task S7, item 3). The blend mode's socket is Integer, and Integer
            // only: there is no meaningful value between Multiply and Screen, so no other kind
            // promotes into it.
            // The content image is OPTIONAL (task FIX1, item B): an artist wires a Layer node up by
            // hand now, so a Layer that has been added but not yet fed is a real, selectable node
            // that simply draws nothing -- not a composition the compiler refuses to compile. The
            // port is still the FIRST one, which is what the dissolve gesture, the mute bypass and
            // the empty-image propagation all read.
            {{std::string(kLayerOutputContentInputPort), SocketValueKind::Image, false},
             {std::string(kPositionParameterRole), SocketValueKind::Vector2, false},
             {std::string(kAnchorParameterRole), SocketValueKind::Vector2, false},
             {std::string(kScaleParameterRole), SocketValueKind::Vector2, false},
             {std::string(kRotationParameterRole), SocketValueKind::Scalar, false},
             {std::string(kOpacityParameterRole), SocketValueKind::Scalar, false},
             {std::string(kBlendModeParameterRole), SocketValueKind::Integer, false}},
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
              ParameterValueKind::Float64, true, true, 1.0},
             // supportsAnimation is false, and deliberately so: there is no meaningful value
             // between Multiply and Screen, so a curve over this parameter could only hold or jump,
             // which an enable/disable keyframe model would express and an interpolated curve
             // would not.
             {std::string(kBlendModeParameterRole), std::string(kBlendModeParameterSchemaKey),
              ParameterValueKind::Integer, true, false, kDefaultBlendModeValue}},
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
            // One operand socket per parameter, in the same order (task S7, item 3). The content
            // port is a String socket: a text layer whose words come from a String node is the
            // whole point of having a String kind at all.
            {{std::string(kTextParameterRole), SocketValueKind::String, false},
             {std::string(kTextSizeParameterRole), SocketValueKind::Scalar, false},
             {std::string(kTextColorParameterRole), SocketValueKind::Color, false}},
            {{std::string(kTextSourceOutputPort), SocketValueKind::Image}},
            // Content stays constant-only (a String has no interpolation); size and colour became
            // animatable in task S5, which the shape check below cross-checks against the schema
            // predicates rather than restating.
            {{std::string(kTextParameterRole), std::string(kTextParameterSchemaKey),
              ParameterValueKind::String, true, false, std::string{}},
             {std::string(kTextSizeParameterRole), std::string(kTextSizeParameterSchemaKey),
              ParameterValueKind::Float64, true, true, kDefaultTextSizePixels},
             {std::string(kTextColorParameterRole), std::string(kTextColorParameterSchemaKey),
              ParameterValueKind::Color4d, true, true, bloom::core::Color4d{1.0, 1.0, 1.0, 1.0}}},
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
    std::vector<NodeDefinition> definitions{solidDefinition(), layerOutputDefinition(),
                                            layerStackDefinition(), compositionOutputDefinition(),
                                            textDefinition()};
    // The value library is appended, not interleaved: the five above are the structural node types
    // a composition is built out of, and reading them first in one place is what makes the
    // registry's own contract legible.
    for (auto& definition : detail::valueNodeDefinitions()) {
        definitions.push_back(std::move(definition));
    }
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
