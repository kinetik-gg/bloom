#include "value_node_definitions.hpp"

#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_operations.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace {

using bloom::document::InputPortDefinition;
using bloom::document::NodeCardinality;
using bloom::document::NodeCategory;
using bloom::document::NodeDefinition;
using bloom::document::NodeLoweringKind;
using bloom::document::OutputPortDefinition;
using bloom::document::ParameterDefinition;
using bloom::document::ParameterValue;
using bloom::document::ParameterValueKind;
using bloom::document::SocketValueKind;

// An operand socket: never required, because the parameter behind it always carries a value. An
// unconnected operand is not a missing input, it is an authored constant -- which is the whole of
// what "parameters as sockets" means.
[[nodiscard]] InputPortDefinition operand(const std::string_view name, const SocketValueKind kind) {
    return {std::string(name), kind, false};
}

[[nodiscard]] OutputPortDefinition result(const std::string_view name, const SocketValueKind kind) {
    return {std::string(name), kind};
}

// An operand's backing parameter. supportsAnimation is read from the schema predicates rather than
// spelled here, so a definition cannot drift from document::isAnimatableSchemaKey() -- which after
// task FIX1, item G accepts the Scalar, Vector 2 and Colour LITERALS and nothing else in this
// library. Every generic operand schema is still constant-or-driven.
[[nodiscard]] ParameterDefinition parameter(const std::string_view role,
                                            const std::string_view schemaKey,
                                            const ParameterValueKind kind, ParameterValue value) {
    return {std::string(role),
            std::string(schemaKey),
            kind,
            true,
            bloom::document::isAnimatableSchemaKey(schemaKey),
            std::move(value)};
}

struct KindVocabulary final {
    SocketValueKind socket;
    ParameterValueKind parameter;
    std::string_view operandSchemaKey;
    ParameterValue defaultValue;
};

[[nodiscard]] KindVocabulary scalarKind() {
    using namespace bloom::document;
    return {SocketValueKind::Scalar, ParameterValueKind::Float64, kScalarOperandParameterSchemaKey,
            0.0};
}

[[nodiscard]] KindVocabulary vectorKind(const std::uint8_t components) {
    using namespace bloom::document;
    if (components == 2) {
        return {SocketValueKind::Vector2, ParameterValueKind::Vec2d,
                kVector2OperandParameterSchemaKey, Vec2d{}};
    }
    return {SocketValueKind::Vector3, ParameterValueKind::Vec3d, kVector3OperandParameterSchemaKey,
            Vec3d{}};
}

// ---------------------------------------------------------------------------------------------
// Values: the literal sources
// ---------------------------------------------------------------------------------------------

// A literal has NO input socket for its own value, and that is deliberate rather than an omission
// of item 3's parameters-as-sockets rule. A literal is where a value comes FROM; a node whose value
// arrives from elsewhere is a Reroute, and Reroute exists for exactly that. Giving a Scalar node a
// Scalar input would make the two node types the same node with two names.
[[nodiscard]] NodeDefinition valueConstantDefinition(const std::string_view typeId,
                                                     const std::string_view schemaKey,
                                                     const SocketValueKind socket,
                                                     const ParameterValueKind kind,
                                                     ParameterValue defaultValue) {
    using namespace bloom::document;
    return {{std::string(typeId), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueConstant,
            {},
            {result(kValuePortName, socket)},
            {parameter(kValueParameterRole, schemaKey, kind, std::move(defaultValue))},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Values};
}

[[nodiscard]] NodeDefinition timeDefinition() {
    using namespace bloom::document;
    // No inputs and no parameters at all: a Time node's value is the frame being rendered, which
    // belongs to the evaluation request and not to the document. Both outputs name the SAME instant
    // in the two units an artist actually wires -- seconds for anything continuous, the frame
    // number for anything that counts or seeds.
    return {{std::string(kTimeValueNodeType), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueTime,
            {},
            {result(kTimeSecondsPortName, SocketValueKind::Scalar),
             result(kTimeFramePortName, SocketValueKind::Integer)},
            {},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Values};
}

// ---------------------------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------------------------

[[nodiscard]] NodeDefinition scalarMathDefinition() {
    using namespace bloom::document;
    // Five operand sockets, one definition, 24 operations. The widest operation (Remap) reads five,
    // so five is what the node declares; the card hides the rows the live operation does not read
    // rather than showing operands it would silently ignore.
    std::vector<InputPortDefinition> inputs;
    std::vector<ParameterDefinition> parameters;
    inputs.reserve(kScalarOperandPortNames.size());
    parameters.reserve(kScalarOperandPortNames.size() + 2);
    for (const auto name : kScalarOperandPortNames) {
        inputs.push_back(operand(name, SocketValueKind::Scalar));
        parameters.push_back(
            parameter(name, kScalarOperandParameterSchemaKey, ParameterValueKind::Float64, 0.0));
    }
    parameters.push_back(parameter(kOperationParameterRole, kScalarOperationParameterSchemaKey,
                                   ParameterValueKind::Integer,
                                   scalarOperationStoredValue(kDefaultScalarOperation)));
    parameters.push_back(parameter(kClampResultParameterRole, kClampResultParameterSchemaKey,
                                   ParameterValueKind::Boolean, false));
    return {{std::string(kScalarMathNodeType), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueScalarMath,
            std::move(inputs),
            {result(kResultPortName, SocketValueKind::Scalar)},
            std::move(parameters),
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Math};
}

[[nodiscard]] NodeDefinition vectorMathDefinition(const std::string_view typeId,
                                                  const std::uint8_t components) {
    using namespace bloom::document;
    const auto vocabulary = vectorKind(components);
    return {{std::string(typeId), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueVectorMath,
            {operand(kFirstOperandPortName, vocabulary.socket),
             operand(kSecondOperandPortName, vocabulary.socket),
             operand(kScaleFactorPortName, SocketValueKind::Scalar)},
            {result(kResultPortName, vocabulary.socket)},
            {parameter(kFirstOperandPortName, vocabulary.operandSchemaKey, vocabulary.parameter,
                       vocabulary.defaultValue),
             parameter(kSecondOperandPortName, vocabulary.operandSchemaKey, vocabulary.parameter,
                       vocabulary.defaultValue),
             // One, not zero: Scale is the operation this operand exists for, and scaling by zero
             // as a default would make a freshly added node collapse its input to nothing.
             parameter(kScaleFactorPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, 1.0),
             parameter(kOperationParameterRole, kVectorOperationParameterSchemaKey,
                       ParameterValueKind::Integer,
                       vectorOperationStoredValue(kDefaultVectorOperation))},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Math};
}

// The reductions are a separate node type from Vector Math, not a mode of it, because their result
// is a SCALAR: a socket's kind is fixed by its definition, so a single node whose output retyped
// itself per operation would be a socket that lies about what it carries.
[[nodiscard]] NodeDefinition vectorReduceDefinition(const std::string_view typeId,
                                                    const std::uint8_t components) {
    using namespace bloom::document;
    const auto vocabulary = vectorKind(components);
    return {{std::string(typeId), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueVectorReduce,
            {operand(kFirstOperandPortName, vocabulary.socket),
             operand(kSecondOperandPortName, vocabulary.socket)},
            {result(kResultPortName, SocketValueKind::Scalar)},
            {parameter(kFirstOperandPortName, vocabulary.operandSchemaKey, vocabulary.parameter,
                       vocabulary.defaultValue),
             parameter(kSecondOperandPortName, vocabulary.operandSchemaKey, vocabulary.parameter,
                       vocabulary.defaultValue),
             parameter(kOperationParameterRole, kVectorReductionParameterSchemaKey,
                       ParameterValueKind::Integer,
                       vectorReductionStoredValue(kDefaultVectorReduction))},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Math};
}

[[nodiscard]] NodeDefinition mapRangeDefinition() {
    using namespace bloom::document;
    return {{std::string(kMapRangeNodeType), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueMapRange,
            {operand(kMapRangeValuePortName, SocketValueKind::Scalar),
             operand(kMapRangeFromMinimumPortName, SocketValueKind::Scalar),
             operand(kMapRangeFromMaximumPortName, SocketValueKind::Scalar),
             operand(kMapRangeToMinimumPortName, SocketValueKind::Scalar),
             operand(kMapRangeToMaximumPortName, SocketValueKind::Scalar)},
            {result(kResultPortName, SocketValueKind::Scalar)},
            {parameter(kMapRangeValuePortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, kDefaultRangeMinimum),
             parameter(kMapRangeFromMinimumPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, kDefaultRangeMinimum),
             parameter(kMapRangeFromMaximumPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, kDefaultRangeMaximum),
             parameter(kMapRangeToMinimumPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, kDefaultRangeMinimum),
             parameter(kMapRangeToMaximumPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, kDefaultRangeMaximum),
             parameter(kInterpolationParameterRole, kRangeInterpolationParameterSchemaKey,
                       ParameterValueKind::Integer,
                       rangeInterpolationStoredValue(kDefaultRangeInterpolation)),
             parameter(kClampResultParameterRole, kClampResultParameterSchemaKey,
                       ParameterValueKind::Boolean, false)},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Math};
}

[[nodiscard]] NodeDefinition clampDefinition() {
    using namespace bloom::document;
    return {{std::string(kClampNodeType), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueClamp,
            {operand(kClampValuePortName, SocketValueKind::Scalar),
             operand(kClampMinimumPortName, SocketValueKind::Scalar),
             operand(kClampMaximumPortName, SocketValueKind::Scalar)},
            {result(kResultPortName, SocketValueKind::Scalar)},
            {parameter(kClampValuePortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, kDefaultRangeMinimum),
             parameter(kClampMinimumPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, kDefaultRangeMinimum),
             parameter(kClampMaximumPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, kDefaultRangeMaximum)},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Math};
}

[[nodiscard]] NodeDefinition mixDefinition() {
    using namespace bloom::document;
    return {{std::string(kMixNodeType), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueMix,
            {operand(kMixFactorPortName, SocketValueKind::Scalar),
             operand(kFirstOperandPortName, SocketValueKind::Scalar),
             operand(kSecondOperandPortName, SocketValueKind::Scalar)},
            {result(kResultPortName, SocketValueKind::Scalar)},
            {parameter(kMixFactorPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, kDefaultMixFactor),
             parameter(kFirstOperandPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, 0.0),
             parameter(kSecondOperandPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, 0.0)},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Math};
}

// A separate node type from Mix, not a data-type mode of it, for the reason
// docs/architecture/evaluation-primitives.md's Type Binding rule gives: a Color is not a Vec4. Its
// four channels are mixed independently as straight authoring values, with no gamut or OCIO step --
// the same straight-value reading the Solid colour schema already has.
[[nodiscard]] NodeDefinition colorMixDefinition() {
    using namespace bloom::document;
    return {{std::string(kColorMixNodeType), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueColorMix,
            {operand(kMixFactorPortName, SocketValueKind::Scalar),
             operand(kFirstOperandPortName, SocketValueKind::Color),
             operand(kSecondOperandPortName, SocketValueKind::Color)},
            {result(kResultPortName, SocketValueKind::Color)},
            {parameter(kMixFactorPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, kDefaultMixFactor),
             parameter(kFirstOperandPortName, kColorOperandParameterSchemaKey,
                       ParameterValueKind::Color4d, kDefaultValueColor),
             parameter(kSecondOperandPortName, kColorOperandParameterSchemaKey,
                       ParameterValueKind::Color4d, kDefaultValueColor)},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Math};
}

[[nodiscard]] NodeDefinition compareDefinition() {
    using namespace bloom::document;
    return {{std::string(kCompareNodeType), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueCompare,
            {operand(kFirstOperandPortName, SocketValueKind::Scalar),
             operand(kSecondOperandPortName, SocketValueKind::Scalar),
             operand(kEpsilonParameterRole, SocketValueKind::Scalar)},
            {result(kResultPortName, SocketValueKind::Boolean)},
            {parameter(kFirstOperandPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, 0.0),
             parameter(kSecondOperandPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, 0.0),
             parameter(kEpsilonParameterRole, kCompareEpsilonParameterSchemaKey,
                       ParameterValueKind::Float64, kDefaultCompareEpsilon),
             parameter(kOperationParameterRole, kCompareOperationParameterSchemaKey,
                       ParameterValueKind::Integer,
                       compareOperationStoredValue(kDefaultCompareOperation))},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Math};
}

// One definition per socket kind, because Bloom has no polymorphic port: a Switch whose two branch
// sockets retyped themselves would be a socket that does not know what it carries, and
// ConnectPorts's kind rule would have nothing to check. Seven small definitions, one lowering.
[[nodiscard]] NodeDefinition switchDefinition(const std::string_view typeId,
                                              const SocketValueKind socket,
                                              const ParameterValueKind kind,
                                              const std::string_view operandSchemaKey,
                                              ParameterValue defaultValue) {
    using namespace bloom::document;
    return {{std::string(typeId), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueSwitch,
            {operand(kSwitchConditionPortName, SocketValueKind::Boolean),
             operand(kSwitchFalsePortName, socket), operand(kSwitchTruePortName, socket)},
            {result(kResultPortName, socket)},
            {parameter(kSwitchConditionPortName, kBooleanOperandParameterSchemaKey,
                       ParameterValueKind::Boolean, false),
             parameter(kSwitchFalsePortName, operandSchemaKey, kind, defaultValue),
             parameter(kSwitchTruePortName, operandSchemaKey, kind, std::move(defaultValue))},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Utilities};
}

// ---------------------------------------------------------------------------------------------
// Convert
// ---------------------------------------------------------------------------------------------

[[nodiscard]] NodeDefinition separateVectorDefinition(const std::string_view typeId,
                                                      const std::uint8_t components) {
    using namespace bloom::document;
    const auto vocabulary = vectorKind(components);
    std::vector<OutputPortDefinition> outputs;
    outputs.reserve(components);
    for (std::uint8_t component = 0; component < components; ++component) {
        outputs.push_back(result(kVectorComponentPortNames[component], SocketValueKind::Scalar));
    }
    return {{std::string(typeId), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueSeparate,
            {operand(kVectorPortName, vocabulary.socket)},
            std::move(outputs),
            {parameter(kVectorPortName, vocabulary.operandSchemaKey, vocabulary.parameter,
                       vocabulary.defaultValue)},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Utilities};
}

[[nodiscard]] NodeDefinition combineVectorDefinition(const std::string_view typeId,
                                                     const std::uint8_t components) {
    using namespace bloom::document;
    const auto vocabulary = vectorKind(components);
    std::vector<InputPortDefinition> inputs;
    std::vector<ParameterDefinition> parameters;
    inputs.reserve(components);
    parameters.reserve(components);
    for (std::uint8_t component = 0; component < components; ++component) {
        const auto name = kVectorComponentPortNames[component];
        inputs.push_back(operand(name, SocketValueKind::Scalar));
        parameters.push_back(
            parameter(name, kScalarOperandParameterSchemaKey, ParameterValueKind::Float64, 0.0));
    }
    return {{std::string(typeId), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueCombine,
            std::move(inputs),
            {result(kVectorPortName, vocabulary.socket)},
            std::move(parameters),
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Utilities};
}

// Channel extraction only, and an explicit node rather than an implicit conversion anywhere: the
// repo's "no implicit Color to Vec4" rule means the only way to get a colour's channels as numbers
// is to ask for them by name.
[[nodiscard]] NodeDefinition separateColorDefinition() {
    using namespace bloom::document;
    std::vector<OutputPortDefinition> outputs;
    outputs.reserve(kColorChannelPortNames.size());
    for (const auto channel : kColorChannelPortNames) {
        outputs.push_back(result(channel, SocketValueKind::Scalar));
    }
    return {{std::string(kSeparateRgbaNodeType), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueSeparate,
            {operand(kColorPortName, SocketValueKind::Color)},
            std::move(outputs),
            {parameter(kColorPortName, kColorOperandParameterSchemaKey, ParameterValueKind::Color4d,
                       kDefaultValueColor)},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Utilities};
}

[[nodiscard]] NodeDefinition combineColorDefinition() {
    using namespace bloom::document;
    std::vector<InputPortDefinition> inputs;
    std::vector<ParameterDefinition> parameters;
    inputs.reserve(kColorChannelPortNames.size());
    parameters.reserve(kColorChannelPortNames.size());
    for (const auto channel : kColorChannelPortNames) {
        inputs.push_back(operand(channel, SocketValueKind::Scalar));
        // One in every channel, alpha included: the default is opaque white, which is what the
        // default colour in every other schema already is.
        parameters.push_back(
            parameter(channel, kScalarOperandParameterSchemaKey, ParameterValueKind::Float64, 1.0));
    }
    return {{std::string(kCombineRgbaNodeType), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueCombine,
            std::move(inputs),
            {result(kColorPortName, SocketValueKind::Color)},
            std::move(parameters),
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Utilities};
}

// ---------------------------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// Task UTIL-1's table-driven library
// ---------------------------------------------------------------------------------------------

// The inverse of socketKindForParameterValueKind(). Image has no authoring kind at all, which is
// why it answers Float64 and the caller's own descriptor may never name it: a value-graph node
// carries numbers, and the shape check refuses an Image socket on one.
[[nodiscard]] ParameterValueKind parameterKindForSocketKind(const SocketValueKind kind) noexcept {
    switch (kind) {
    case SocketValueKind::Color:
        return ParameterValueKind::Color4d;
    case SocketValueKind::Vector2:
        return ParameterValueKind::Vec2d;
    case SocketValueKind::Vector3:
        return ParameterValueKind::Vec3d;
    case SocketValueKind::String:
        return ParameterValueKind::String;
    case SocketValueKind::Integer:
        return ParameterValueKind::Integer;
    case SocketValueKind::Boolean:
        return ParameterValueKind::Boolean;
    case SocketValueKind::Image:
    case SocketValueKind::Audio:
    case SocketValueKind::Scalar:
        break;
    }
    return ParameterValueKind::Float64;
}

// The generic operand schema for a kind. One schema per KIND rather than one per node-and-role, for
// the reason parameter.hpp's own comment gives: a Substring's `start` and a Repeat's `count` are
// both an unconstrained integer, and two schema keys saying so would be two spellings of one
// domain.
[[nodiscard]] std::string_view operandSchemaKeyForSocketKind(const SocketValueKind kind) noexcept {
    using namespace bloom::document;
    switch (kind) {
    case SocketValueKind::Color:
        return kColorOperandParameterSchemaKey;
    case SocketValueKind::Vector2:
        return kVector2OperandParameterSchemaKey;
    case SocketValueKind::Vector3:
        return kVector3OperandParameterSchemaKey;
    case SocketValueKind::String:
        return kStringOperandParameterSchemaKey;
    case SocketValueKind::Integer:
        return kIntegerOperandParameterSchemaKey;
    case SocketValueKind::Boolean:
        return kBooleanOperandParameterSchemaKey;
    case SocketValueKind::Image:
    case SocketValueKind::Audio:
    case SocketValueKind::Scalar:
        break;
    }
    return kScalarOperandParameterSchemaKey;
}

[[nodiscard]] ParameterValue
descriptorDefaultValue(const bloom::document::ValueUtilityOperand& operand) {
    using namespace bloom::document;
    switch (operand.kind) {
    case SocketValueKind::Color:
        return kDefaultValueColor;
    case SocketValueKind::Vector2:
        return Vec2d{operand.number, operand.y};
    case SocketValueKind::Vector3:
        return Vec3d{operand.number, operand.y, operand.z};
    case SocketValueKind::String:
        return std::string(operand.text);
    case SocketValueKind::Integer:
        return operand.integer;
    case SocketValueKind::Boolean:
        return operand.flag;
    case SocketValueKind::Image:
    case SocketValueKind::Audio:
    case SocketValueKind::Scalar:
        break;
    }
    return operand.number;
}

// One descriptor as a registered definition. Operand sockets come first and in the descriptor's own
// order, then the inline selectors as parameters with no socket -- which is exactly the order
// hasValidValueLoweringShape() reads them back in.
[[nodiscard]] NodeDefinition
valueUtilityDefinition(const bloom::document::ValueUtilityDescriptor& descriptor) {
    using namespace bloom::document;
    std::vector<InputPortDefinition> inputs;
    std::vector<ParameterDefinition> parameters;
    std::vector<OutputPortDefinition> outputs;
    inputs.reserve(descriptor.operands.size());
    parameters.reserve(descriptor.operands.size() + descriptor.selectors.size());
    outputs.reserve(descriptor.outputs.size());
    for (const auto& declared : descriptor.operands) {
        inputs.push_back(operand(declared.role, declared.kind));
        parameters.push_back(parameter(declared.role, operandSchemaKeyForSocketKind(declared.kind),
                                       parameterKindForSocketKind(declared.kind),
                                       descriptorDefaultValue(declared)));
    }
    for (const auto& selector : descriptor.selectors) {
        parameters.push_back(parameter(selector.role, selector.schemaKey,
                                       ParameterValueKind::Integer, selector.defaultValue));
    }
    for (const auto& declared : descriptor.outputs) {
        outputs.push_back(result(declared.name, declared.kind));
    }
    return {{std::string(descriptor.typeId), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueUtility,
            std::move(inputs),
            std::move(outputs),
            std::move(parameters),
            std::nullopt,
            NodeCardinality::Many,
            descriptor.category};
}

[[nodiscard]] NodeDefinition randomDefinition() {
    using namespace bloom::document;
    // Seeded and nothing else. The same seed is the same value on every machine and in every
    // process, so the only way this value changes over time is if something wires a changing number
    // -- a Time node's frame -- into the seed. That is deliberate: a render must be reproducible.
    return {{std::string(kRandomNodeType), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueRandom,
            {operand(kRandomSeedPortName, SocketValueKind::Integer),
             operand(kRandomMinimumPortName, SocketValueKind::Scalar),
             operand(kRandomMaximumPortName, SocketValueKind::Scalar)},
            {result(kValuePortName, SocketValueKind::Scalar)},
            {parameter(kRandomSeedPortName, kIntegerOperandParameterSchemaKey,
                       ParameterValueKind::Integer, std::int64_t{0}),
             parameter(kRandomMinimumPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, kDefaultRangeMinimum),
             parameter(kRandomMaximumPortName, kScalarOperandParameterSchemaKey,
                       ParameterValueKind::Float64, kDefaultRangeMaximum)},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Math};
}

// The one value lowering with a REQUIRED input and no parameter: a Reroute has no value of its own
// at all, so an unconnected one genuinely cannot produce anything and the existing missing-input
// diagnostic is the right report. It also spans Image, unlike every other value lowering, because
// tidying an image wire is the same gesture as tidying a scalar one.
[[nodiscard]] NodeDefinition rerouteDefinition() {
    using namespace bloom::document;
    return {{std::string(kRerouteNodeType), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueReroute,
            {{std::string(kValuePortName), SocketValueKind::Image, true}},
            {result(kValuePortName, SocketValueKind::Image)},
            {},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Utilities};
}

[[nodiscard]] NodeDefinition layerBoundsDefinition() {
    using namespace bloom::document;
    return {{std::string(kLayerBoundsNodeType), kValueNodeSchemaVersion},
            NodeLoweringKind::ValueBoundsReadout,
            {{std::string(kLayerBoundsImagePortName), SocketValueKind::Image, true}},
            {result(kLayerBoundsSizePortName, SocketValueKind::Vector2),
             result(kLayerBoundsOriginPortName, SocketValueKind::Vector2),
             result(kLayerBoundsAnchorPortName, SocketValueKind::Vector2),
             result(kLayerBoundsCenterPortName, SocketValueKind::Vector2)},
            {},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Values};
}

} // namespace

namespace bloom::document::detail {

bool isInlineSelectorSchemaKey(const std::string_view schemaKey) noexcept {
    if (schemaKey == kRoundingModeParameterSchemaKey ||
        schemaKey == kNumberRadixParameterSchemaKey || schemaKey == kStringCaseParameterSchemaKey ||
        schemaKey == kStringPadSideParameterSchemaKey ||
        schemaKey == kIntegerOperationParameterSchemaKey ||
        schemaKey == kBooleanOperationParameterSchemaKey) {
        return true;
    }
    return schemaKey == kScalarOperationParameterSchemaKey ||
           schemaKey == kVectorOperationParameterSchemaKey ||
           schemaKey == kVectorReductionParameterSchemaKey ||
           schemaKey == kRangeInterpolationParameterSchemaKey ||
           schemaKey == kCompareOperationParameterSchemaKey ||
           schemaKey == kClampResultParameterSchemaKey;
}

// ONE generic contract for all fifteen value lowerings, where each of the five image lowerings gets
// a bespoke one. That asymmetry is deliberate: the image lowerings each have a fixed, load-bearing
// shape the evaluator depends on by position (a Layer Output's six transform parameters in
// authoring order), while the value lowerings share one invariant that is the actual contract --
// every operand socket is backed by a parameter of the matching kind, and every parameter is either
// such an operand or an inline selector. Fifteen copies of that sentence would be fifteen places it
// could be written slightly differently.
bool hasValidValueLoweringShape(const NodeDefinition& definition) noexcept {
    if (!isValueLowering(definition.lowering)) {
        return false;
    }
    // Value nodes are never structural: no layer slot, never a singleton, and always offered under
    // one of the two sections the Add surface lists them in.
    if (definition.layerSlotInput.has_value() || definition.cardinality != NodeCardinality::Many ||
        (definition.category != NodeCategory::Values && definition.category != NodeCategory::Math &&
         definition.category != NodeCategory::Utilities)) {
        return false;
    }
    if (definition.outputs.empty()) {
        return false;
    }
    const bool carriesImages = definition.lowering == NodeLoweringKind::ValueReroute &&
                               isRerouteNodeType(definition.key.typeId);
    const bool hasImageTransportInput =
        carriesImages || definition.lowering == NodeLoweringKind::ValueBoundsReadout;
    for (const auto& output : definition.outputs) {
        if ((output.valueKind == SocketValueKind::Image) != carriesImages) {
            return false;
        }
    }
    const auto findParameter = [&definition](const std::string_view role) {
        return std::ranges::find(definition.parameters, role, &ParameterDefinition::role);
    };
    for (const auto& input : definition.inputs) {
        const bool imageInput = input.valueKind == SocketValueKind::Image;
        if (imageInput != hasImageTransportInput ||
            (definition.lowering == NodeLoweringKind::ValueBoundsReadout &&
             (&input != &definition.inputs.front()))) {
            return false;
        }
        const auto backing = findParameter(input.name);
        if (backing == definition.parameters.end()) {
            // The only socket with no parameter behind it is a Reroute's pass-through, which is
            // required precisely because nothing else can supply it.
            if ((definition.lowering != NodeLoweringKind::ValueReroute &&
                 definition.lowering != NodeLoweringKind::ValueBoundsReadout) ||
                !input.required) {
                return false;
            }
            continue;
        }
        // An operand socket is never required: the parameter behind it is the value when nothing is
        // connected.
        if (input.required ||
            input.valueKind != socketKindForParameterValueKind(backing->valueKind)) {
            return false;
        }
    }
    for (const auto& declared : definition.parameters) {
        // Animatability is the schema predicates' answer, never a second opinion: a definition that
        // claimed a curve kind the document does not have would promise an editor gesture that
        // cannot be carried out.
        if (declared.supportsAnimation != isAnimatableSchemaKey(declared.schemaKey)) {
            return false;
        }
        const auto socket =
            std::ranges::find(definition.inputs, declared.role, &InputPortDefinition::name);
        if (socket != definition.inputs.end()) {
            continue;
        }
        // A parameter with no socket is either an inline selector, or the single authored value of
        // a literal Value node -- which has no inputs at all, because a literal is where a value
        // comes from rather than somewhere one arrives.
        const bool literal = definition.lowering == NodeLoweringKind::ValueConstant &&
                             definition.inputs.empty() && definition.parameters.size() == 1 &&
                             declared.role == kValueParameterRole;
        if (!literal && !isInlineSelectorSchemaKey(declared.schemaKey)) {
            return false;
        }
    }
    // Per-lowering structure: how many operands and results each shape is, where the generic rules
    // above cannot say it. A lowering whose operand count is wrong would reach a kernel with the
    // wrong arity, which the kernel would reject at every frame rather than at registration.
    const auto inputs = definition.inputs.size();
    const auto outputs = definition.outputs.size();
    switch (definition.lowering) {
    case NodeLoweringKind::ValueConstant:
        return inputs == 0 && outputs == 1 && definition.parameters.size() == 1;
    case NodeLoweringKind::ValueTime:
        return inputs == 0 && outputs == 2 && definition.parameters.empty() &&
               definition.outputs[0].name == kTimeSecondsPortName &&
               definition.outputs[0].valueKind == SocketValueKind::Scalar &&
               definition.outputs[1].name == kTimeFramePortName &&
               definition.outputs[1].valueKind == SocketValueKind::Integer;
    case NodeLoweringKind::ValueScalarMath:
        return inputs == kMaximumScalarOperationOperands && outputs == 1 &&
               definition.parameters.size() == kMaximumScalarOperationOperands + 2;
    case NodeLoweringKind::ValueVectorMath:
        return inputs == 3 && outputs == 1 && definition.parameters.size() == 4;
    case NodeLoweringKind::ValueVectorReduce:
        return inputs == 2 && outputs == 1 && definition.parameters.size() == 3 &&
               definition.outputs.front().valueKind == SocketValueKind::Scalar;
    case NodeLoweringKind::ValueMapRange:
        return inputs == 5 && outputs == 1 && definition.parameters.size() == 7;
    case NodeLoweringKind::ValueClamp:
        return inputs == 3 && outputs == 1 && definition.parameters.size() == 3;
    case NodeLoweringKind::ValueMix:
    case NodeLoweringKind::ValueColorMix:
        return inputs == 3 && outputs == 1 && definition.parameters.size() == 3 &&
               definition.inputs.front().name == kMixFactorPortName &&
               definition.inputs.front().valueKind == SocketValueKind::Scalar;
    case NodeLoweringKind::ValueCompare:
        return inputs == 3 && outputs == 1 && definition.parameters.size() == 4 &&
               definition.outputs.front().valueKind == SocketValueKind::Boolean;
    case NodeLoweringKind::ValueSwitch:
        return inputs == 3 && outputs == 1 && definition.parameters.size() == 3 &&
               definition.inputs.front().name == kSwitchConditionPortName &&
               definition.inputs.front().valueKind == SocketValueKind::Boolean &&
               definition.inputs[1].valueKind == definition.outputs.front().valueKind &&
               definition.inputs[2].valueKind == definition.outputs.front().valueKind;
    case NodeLoweringKind::ValueSeparate:
        return inputs == 1 && definition.parameters.size() == 1 && outputs >= 2 && outputs <= 4;
    case NodeLoweringKind::ValueCombine:
        return outputs == 1 && inputs >= 2 && inputs <= 4 && definition.parameters.size() == inputs;
    case NodeLoweringKind::ValueRandom:
        return inputs == 3 && outputs == 1 && definition.parameters.size() == 3 &&
               definition.outputs.front().valueKind == SocketValueKind::Scalar;
    case NodeLoweringKind::ValueUtility: {
        // Checked against the DESCRIPTOR rather than against a hand-written arity, because the
        // descriptor is what the compiler and the kernel read: a definition that disagreed with it
        // would lower operands the kernel does not expect, at every frame rather than at startup.
        const auto* descriptor = findValueUtilityDescriptor(definition.key.typeId);
        if (descriptor == nullptr || inputs != descriptor->operands.size() ||
            outputs != descriptor->outputs.size() ||
            definition.parameters.size() !=
                descriptor->operands.size() + descriptor->selectors.size() ||
            definition.category != descriptor->category) {
            return false;
        }
        for (std::size_t index = 0; index < descriptor->operands.size(); ++index) {
            if (definition.inputs[index].name != descriptor->operands[index].role ||
                definition.inputs[index].valueKind != descriptor->operands[index].kind) {
                return false;
            }
        }
        for (std::size_t index = 0; index < descriptor->selectors.size(); ++index) {
            const auto& declared = definition.parameters[descriptor->operands.size() + index];
            if (declared.role != descriptor->selectors[index].role ||
                declared.schemaKey != descriptor->selectors[index].schemaKey ||
                declared.valueKind != ParameterValueKind::Integer) {
                return false;
            }
        }
        for (std::size_t index = 0; index < descriptor->outputs.size(); ++index) {
            if (definition.outputs[index].name != descriptor->outputs[index].name ||
                definition.outputs[index].valueKind != descriptor->outputs[index].kind) {
                return false;
            }
        }
        return true;
    }
    case NodeLoweringKind::ValueReroute:
        return inputs == 1 && outputs == 1 && definition.parameters.empty() &&
               definition.inputs.front().valueKind == definition.outputs.front().valueKind &&
               definition.inputs.front().name == definition.outputs.front().name;
    case NodeLoweringKind::ValueBoundsReadout:
        return inputs == 1 && outputs == 4 && definition.parameters.empty() &&
               definition.inputs.front().name == kLayerBoundsImagePortName &&
               definition.inputs.front().valueKind == SocketValueKind::Image &&
               definition.inputs.front().required &&
               definition.outputs[0].name == kLayerBoundsSizePortName &&
               definition.outputs[1].name == kLayerBoundsOriginPortName &&
               definition.outputs[2].name == kLayerBoundsAnchorPortName &&
               definition.outputs[3].name == kLayerBoundsCenterPortName &&
               std::ranges::all_of(definition.outputs, [](const auto& output) {
                   return output.valueKind == SocketValueKind::Vector2;
               });
    case NodeLoweringKind::Shape:
    case NodeLoweringKind::Solid:
    case NodeLoweringKind::ImageSource:
    case NodeLoweringKind::AudioSource:
    case NodeLoweringKind::Text:
    case NodeLoweringKind::LayerOutput:
    case NodeLoweringKind::LayerStack:
    case NodeLoweringKind::CompositionOutput:
    case NodeLoweringKind::Unsupported:
        break;
    }
    return false;
}

std::vector<NodeDefinition> valueNodeDefinitions() {
    std::vector<NodeDefinition> definitions;
    definitions.reserve(40 + valueUtilityDescriptors().size());
    definitions.push_back(valueConstantDefinition(
        kIntegerValueNodeType, kIntegerValueParameterSchemaKey, SocketValueKind::Integer,
        ParameterValueKind::Integer, std::int64_t{0}));
    definitions.push_back(
        valueConstantDefinition(kScalarValueNodeType, kScalarValueParameterSchemaKey,
                                SocketValueKind::Scalar, ParameterValueKind::Float64, 0.0));
    definitions.push_back(
        valueConstantDefinition(kVector2ValueNodeType, kVector2ValueParameterSchemaKey,
                                SocketValueKind::Vector2, ParameterValueKind::Vec2d, Vec2d{}));
    definitions.push_back(
        valueConstantDefinition(kVector3ValueNodeType, kVector3ValueParameterSchemaKey,
                                SocketValueKind::Vector3, ParameterValueKind::Vec3d, Vec3d{}));
    definitions.push_back(valueConstantDefinition(
        kStringValueNodeType, kStringValueParameterSchemaKey, SocketValueKind::String,
        ParameterValueKind::String, std::string{}));
    definitions.push_back(valueConstantDefinition(
        kColorValueNodeType, kColorValueParameterSchemaKey, SocketValueKind::Color,
        ParameterValueKind::Color4d, kDefaultValueColor));
    definitions.push_back(
        valueConstantDefinition(kBooleanValueNodeType, kBooleanValueParameterSchemaKey,
                                SocketValueKind::Boolean, ParameterValueKind::Boolean, false));
    definitions.push_back(timeDefinition());

    definitions.push_back(scalarMathDefinition());
    definitions.push_back(vectorMathDefinition(kVector2MathNodeType, 2));
    definitions.push_back(vectorMathDefinition(kVector3MathNodeType, 3));
    definitions.push_back(vectorReduceDefinition(kVector2ReduceNodeType, 2));
    definitions.push_back(vectorReduceDefinition(kVector3ReduceNodeType, 3));
    definitions.push_back(mapRangeDefinition());
    definitions.push_back(clampDefinition());
    definitions.push_back(mixDefinition());
    definitions.push_back(colorMixDefinition());
    definitions.push_back(compareDefinition());

    const auto scalar = scalarKind();
    definitions.push_back(switchDefinition(kScalarSwitchNodeType, scalar.socket, scalar.parameter,
                                           scalar.operandSchemaKey, scalar.defaultValue));
    definitions.push_back(switchDefinition(kIntegerSwitchNodeType, SocketValueKind::Integer,
                                           ParameterValueKind::Integer,
                                           kIntegerOperandParameterSchemaKey, std::int64_t{0}));
    definitions.push_back(switchDefinition(kBooleanSwitchNodeType, SocketValueKind::Boolean,
                                           ParameterValueKind::Boolean,
                                           kBooleanOperandParameterSchemaKey, false));
    definitions.push_back(switchDefinition(kVector2SwitchNodeType, SocketValueKind::Vector2,
                                           ParameterValueKind::Vec2d,
                                           kVector2OperandParameterSchemaKey, Vec2d{}));
    definitions.push_back(switchDefinition(kVector3SwitchNodeType, SocketValueKind::Vector3,
                                           ParameterValueKind::Vec3d,
                                           kVector3OperandParameterSchemaKey, Vec3d{}));
    definitions.push_back(switchDefinition(kColorSwitchNodeType, SocketValueKind::Color,
                                           ParameterValueKind::Color4d,
                                           kColorOperandParameterSchemaKey, kDefaultValueColor));
    definitions.push_back(switchDefinition(kStringSwitchNodeType, SocketValueKind::String,
                                           ParameterValueKind::String,
                                           kStringOperandParameterSchemaKey, std::string{}));

    definitions.push_back(separateVectorDefinition(kSeparateXyNodeType, 2));
    definitions.push_back(combineVectorDefinition(kCombineXyNodeType, 2));
    definitions.push_back(separateVectorDefinition(kSeparateXyzNodeType, 3));
    definitions.push_back(combineVectorDefinition(kCombineXyzNodeType, 3));
    definitions.push_back(separateColorDefinition());
    definitions.push_back(combineColorDefinition());

    definitions.push_back(randomDefinition());

    // ONE reroute (task FIX1, item I). Its DECLARED kind is Image only because a definition must
    // name one; the kind a reroute actually carries is resolved from the link it sits on, which is
    // what CanonicalGraph::outputKind()/inputKind() answer for this type and what every
    // connect-time and compile-time check therefore asks.
    definitions.push_back(rerouteDefinition());
    definitions.push_back(layerBoundsDefinition());

    // Task UTIL-1: one definition per descriptor, in the table's own order.
    for (const auto& descriptor : valueUtilityDescriptors()) {
        definitions.push_back(valueUtilityDefinition(descriptor));
    }
    return definitions;
}

} // namespace bloom::document::detail
