#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime {

struct NodeTypeKey {
    std::string typeId;
    std::uint32_t schemaVersion = 0;

    friend bool operator==(const NodeTypeKey&, const NodeTypeKey&) = default;
};

enum class SocketValueKind {
    Image,
};

enum class ParameterValueKind {
    Color4d,
    Vec2d,
    Float64,
    String,
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

    friend bool operator==(const ParameterDefinition&, const ParameterDefinition&) = default;
};

struct LayerSlotInputDefinition {
    std::string role;
    SocketValueKind valueKind = SocketValueKind::Image;
    bool requiredPerSlot = true;

    friend bool operator==(const LayerSlotInputDefinition&,
                           const LayerSlotInputDefinition&) = default;
};

enum class NodeLoweringKind {
    Solid,
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

} // namespace bloom::runtime
