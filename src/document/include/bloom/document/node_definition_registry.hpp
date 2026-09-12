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
};

enum class ParameterValueKind {
    Color4d,
    Vec2d,
    Float64,
    String,
    // A small signed integer. Today's only use is an enumeration stored under its own closed
    // mapping (the Layer Output blend mode), which is why there is no separate "Enum" kind: the
    // stored value IS an integer, and the schema key -- not the value kind -- is what names the
    // enumeration the integer belongs to.
    Integer,
};

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
    Output,
    Utilities,
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
    LayerOutput,
    LayerStack,
    CompositionOutput,
    Unsupported,
};

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
