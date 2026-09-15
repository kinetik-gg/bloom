#pragma once

#include <bloom/document/parameter.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::document {

struct NodeTypeKey {
    std::string typeId;
    std::uint32_t schemaVersion = 0;

    friend bool operator==(const NodeTypeKey&, const NodeTypeKey&) = default;
};

enum class SocketValueKind {
    Image,
    Color,
    Scalar,
    Vector2,
    String,
    // Task S7. Integer and Boolean carry ParameterValue's existing std::int64_t and bool members,
    // so neither needed a new authoring type; Vector3 needed document::Vec3d.
    Integer,
    Boolean,
    Vector3,
};

enum class ParameterValueKind {
    Color4d,
    Vec2d,
    Float64,
    String,
    // A small signed integer. Its first use was an enumeration stored under its own closed mapping
    // (the Layer Output blend mode), which is why there is no separate "Enum" kind: the stored
    // value IS an integer, and the schema key -- not the value kind -- is what names the
    // enumeration the integer belongs to. Task S7 gave it a second, plainer use: an Integer value
    // node and every index, seed and count operand in the value library.
    Integer,
    // Task S7.
    Boolean,
    Vec3d,
};

// The one correspondence between an authored value's kind and the socket kind that carries it.
// Every value-graph node definition is checked against this rather than restating the pairing, so a
// parameter and the socket that can drive it cannot disagree about what they hold.
[[nodiscard]] constexpr SocketValueKind
socketKindForParameterValueKind(const ParameterValueKind kind) noexcept {
    switch (kind) {
    case ParameterValueKind::Color4d:
        return SocketValueKind::Color;
    case ParameterValueKind::Vec2d:
        return SocketValueKind::Vector2;
    case ParameterValueKind::Float64:
        return SocketValueKind::Scalar;
    case ParameterValueKind::String:
        return SocketValueKind::String;
    case ParameterValueKind::Integer:
        return SocketValueKind::Integer;
    case ParameterValueKind::Boolean:
        return SocketValueKind::Boolean;
    case ParameterValueKind::Vec3d:
        return SocketValueKind::Vector3;
    }
    return SocketValueKind::Image;
}

// The promotion whitelist (task S7, item 1). A connection is accepted when the kinds are EQUAL, or
// when the source kind appears here as promotable to the destination kind.
//
// Deliberately a short explicit whitelist rather than implicit numeric coercion anywhere a number
// meets a number. Each entry below is a widening with exactly one answer and no lost information:
//
//   Integer -> Scalar    every std::int64_t a document can hold is exactly representable as a
//                        double up to 2^53, and the compiler emits an explicit conversion operation
//                        so the widening is visible in a plan dump rather than implied.
//   Boolean -> Integer   false is 0 and true is 1, the same mapping every stored boolean already
//   has. Boolean -> Scalar    the same mapping, widened once more; offered directly so a Compare
//   result
//                        can feed a Mix factor without an intervening conversion node.
//   Scalar  -> Vector2   "splat": the one value in every component. A vector built from one number
//   Scalar  -> Vector3   has no other defensible reading, and it is what every comparable tool
//   does.
//
// Nothing promotes INTO Boolean, String, or Image, and no vector promotes to another width: each of
// those would have to invent information (which components? which spelling? which pixels?), and a
// refusal the artist can see is better than a guess they cannot.
[[nodiscard]] constexpr bool
isPromotedSocketConnection(const SocketValueKind source,
                           const SocketValueKind destination) noexcept {
    switch (source) {
    case SocketValueKind::Integer:
        return destination == SocketValueKind::Scalar;
    case SocketValueKind::Boolean:
        return destination == SocketValueKind::Integer || destination == SocketValueKind::Scalar;
    case SocketValueKind::Scalar:
        return destination == SocketValueKind::Vector2 || destination == SocketValueKind::Vector3;
    case SocketValueKind::Image:
    case SocketValueKind::Color:
    case SocketValueKind::Vector2:
    case SocketValueKind::String:
    case SocketValueKind::Vector3:
        return false;
    }
    return false;
}

// The ONE question every connect-time and validation-time kind check asks, so the editor's drag
// affinity, ConnectPorts, CanonicalGraph::validate() and the compiler's edge check cannot disagree
// about which links exist.
[[nodiscard]] constexpr bool
isAcceptedSocketConnection(const SocketValueKind source,
                           const SocketValueKind destination) noexcept {
    return source == destination || isPromotedSocketConnection(source, destination);
}

struct InputPortDefinition {
    std::string name;
    SocketValueKind valueKind = SocketValueKind::Image;
    bool required = true;

    friend bool operator==(const InputPortDefinition&, const InputPortDefinition&) = default;
};

struct OutputPortDefinition {
    std::string name;
    SocketValueKind valueKind = SocketValueKind::Image;

    friend bool operator==(const OutputPortDefinition&, const OutputPortDefinition&) = default;
};

struct ParameterDefinition {
    std::string role;
    std::string schemaKey;
    ParameterValueKind valueKind = ParameterValueKind::Float64;
    bool required = true;
    bool supportsAnimation = false;
    ParameterValue defaultValue = 0.0;

    friend bool operator==(const ParameterDefinition&, const ParameterDefinition&) = default;
};

struct LayerSlotInputDefinition {
    std::string role;
    SocketValueKind valueKind = SocketValueKind::Image;
    bool requiredPerSlot = true;

    friend bool operator==(const LayerSlotInputDefinition&,
                           const LayerSlotInputDefinition&) = default;
};

// Which section of an Add surface a node type is listed under (task S1, item 4). The vocabulary is
// the artist's, not the lowering's: what a node is FOR, which is why it is declared beside the type
// rather than derived from NodeLoweringKind (several unrelated lowerings are Sources, and the
// Unsupported lowering spans every section).
enum class NodeCategory : std::uint8_t {
    Sources,
    Layers,
    Compositing,
    Values,
    // Task UTIL-1. Arithmetic and shaping got their own section because Utilities had become the
    // place everything that is not a source, a layer or an output ends up: a Math node, a Switch, a
    // Reroute and a string Trim are not one family, and burying the arithmetic among the plumbing
    // is what made an artist scroll past it. The category is NOT persisted -- a document stores the
    // node's type id -- so moving a type between sections needs no migration.
    Math,
    Output,
    Utilities,
    // Retained persisted schemas, excluded from new-node authoring categories.
    Compatibility,
};

// How many instances of a node type one composition may hold (task S1, item 5). The composition's
// single evaluation endpoint and its single layer-stack operator are structural singletons: a
// second one is not a graph a composition can mean, which is why the refusal belongs to the
// definition rather than to each surface that offers an Add.
enum class NodeCardinality : std::uint8_t {
    Many,
    OnePerComposition,
};

enum class NodeLoweringKind {
    Solid,
    Text,
    ImageSource,
    LayerOutput,
    LayerStack,
    CompositionOutput,
    // The value-graph lowerings (task S7). Each compiles to a runtime::CompiledValueOperation, not
    // a CompiledOperation: these produce ONE value per frame, not pixels, and they are evaluated in
    // their own pass before any image operation reads one.
    //
    // They are grouped by the KERNEL they reach rather than one per node type, so the twenty-odd
    // library nodes share a handful of lowerings the way the Solid and Text sources share
    // compiledColorParameter(): a Switch over Scalar and a Switch over Colour are the same
    // operation on different storage, and giving each its own lowering would be the same code
    // twice.
    ValueConstant,
    ValueTime,
    ValueScalarMath,
    ValueVectorMath,
    ValueVectorReduce,
    ValueMapRange,
    ValueClamp,
    ValueMix,
    ValueColorMix,
    ValueCompare,
    ValueSwitch,
    ValueSeparate,
    ValueCombine,
    ValueRandom,
    ValueReroute,
    // Task UTIL-1's conversion, string, logic, numeric and readout library. ONE lowering for all of
    // it, because every one of those nodes has the same shape -- a fixed operand list in, a fixed
    // output list out, and a kernel selected by the node's own TYPE rather than by a stored
    // operation. The shape itself lives in document::valueUtilityDescriptors(), so the definition,
    // the validation, the lowering and the kernel all read one table instead of four copies of it.
    ValueUtility,
    Unsupported,
};

// Whether this lowering belongs to the value graph rather than the image chain. The compiler reads
// it to split one reachable node set into the two passes, so the split is declared beside the
// lowerings instead of being an ever-growing condition at the call site.
[[nodiscard]] constexpr bool isValueLowering(const NodeLoweringKind lowering) noexcept {
    switch (lowering) {
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
    case NodeLoweringKind::ValueUtility:
        return true;
    case NodeLoweringKind::Solid:
    case NodeLoweringKind::Text:
    case NodeLoweringKind::ImageSource:
    case NodeLoweringKind::LayerOutput:
    case NodeLoweringKind::LayerStack:
    case NodeLoweringKind::CompositionOutput:
    case NodeLoweringKind::Unsupported:
        return false;
    }
    return false;
}

struct NodeDefinition {
    NodeTypeKey key;
    NodeLoweringKind lowering = NodeLoweringKind::Unsupported;
    std::vector<InputPortDefinition> inputs;
    std::vector<OutputPortDefinition> outputs;
    std::vector<ParameterDefinition> parameters;
    std::optional<LayerSlotInputDefinition> layerSlotInput;
    NodeCardinality cardinality = NodeCardinality::Many;
    NodeCategory category = NodeCategory::Utilities;

    friend bool operator==(const NodeDefinition&, const NodeDefinition&) = default;
};

enum class NodeRegistrationStatus {
    Registered,
    InvalidDefinition,
    DuplicateDefinition,
    Frozen,
};

class NodeDefinitionRegistry final {
  public:
    NodeDefinitionRegistry() = default;
    NodeDefinitionRegistry(const NodeDefinitionRegistry&) = delete;
    NodeDefinitionRegistry& operator=(const NodeDefinitionRegistry&) = delete;
    NodeDefinitionRegistry(NodeDefinitionRegistry&&) = delete;
    NodeDefinitionRegistry& operator=(NodeDefinitionRegistry&&) = delete;

    [[nodiscard]] NodeRegistrationStatus registerDefinition(NodeDefinition definition);
    void freeze();

    [[nodiscard]] bool isFrozen() const noexcept { return frozen_; }
    [[nodiscard]] std::span<const NodeDefinition> definitions() const noexcept {
        return definitions_;
    }
    [[nodiscard]] const NodeDefinition* find(std::string_view typeId,
                                             std::uint32_t schemaVersion) const noexcept;
    [[nodiscard]] bool containsType(std::string_view typeId) const noexcept;

  private:
    std::vector<NodeDefinition> definitions_;
    bool frozen_ = false;
};

[[nodiscard]] bool registerBuiltInNodeDefinitions(NodeDefinitionRegistry& registry);
[[nodiscard]] const NodeDefinitionRegistry& builtInNodeDefinitions();

} // namespace bloom::document
