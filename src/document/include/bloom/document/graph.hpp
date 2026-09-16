#pragma once

#include <bloom/document/ids.hpp>
#include <bloom/document/layer_stack.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/validation.hpp>
#include <bloom/document/value_nodes.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::document {

inline constexpr std::string_view kLayerOutputNodeType = "bloom.layer-output";
inline constexpr std::string_view kLayerStackNodeType = "bloom.layer-stack";
inline constexpr std::string_view kSolidSourceNodeType = "bloom.solid-source";
inline constexpr std::string_view kTextSourceNodeType = "bloom.text-source";
inline constexpr std::string_view kCompositionOutputNodeType = "bloom.composition-output";
inline constexpr std::string_view kAudioSourceNodeType = "bloom.audio-source";
// Bumped to 2 by the layer transform breadth slice (task S4): a Layer Output now binds anchor,
// scale, and rotation alongside position and opacity. A version-1 node is not rejected -- Project
// I/O upgrades it on decode by injecting the three parameters at their defaults, which reproduce
// the version-1 picture exactly (see src/project/document_node_schema_upgrade.cpp and
// docs/architecture/project-format.md, "Node Schema Upgrades").
inline constexpr std::uint32_t kLayerOutputNodeSchemaVersion = 4;
inline constexpr std::uint32_t kLayerStackNodeSchemaVersion = 2;
inline constexpr std::uint32_t kSolidSourceNodeSchemaVersion = 2;
inline constexpr std::uint32_t kTextSourceNodeSchemaVersion = 2;
inline constexpr std::uint32_t kCompositionOutputNodeSchemaVersion = 1;
inline constexpr std::uint32_t kAudioSourceNodeSchemaVersion = 1;

inline constexpr std::string_view kSolidSourceOutputPort = "image";
inline constexpr std::string_view kAudioSourceOutputPort = "audio";
inline constexpr std::string_view kTextSourceOutputPort = "image";
inline constexpr std::string_view kLayerOutputContentInputPort = "image";
inline constexpr std::string_view kLayerOutputOutputPort = "image";
inline constexpr std::string_view kLayerOutputAudioInputPort = "audio";
inline constexpr std::string_view kLayerOutputAudioOutputPort = "audio";
inline constexpr std::string_view kLayerStackContentInputRole = "content";
inline constexpr std::string_view kLayerStackOutputPort = "image";
inline constexpr std::string_view kLayerStackAudioInputRole = "audio";
inline constexpr std::string_view kLayerStackAudioOutputPort = "audio";
inline constexpr std::string_view kCompositionOutputInputPort = "image";
inline constexpr std::string_view kCompositionOutputOutputPort = "image";
inline constexpr std::string_view kCompositionOutputAudioInputPort = "audio";

inline constexpr std::string_view kSolidColorParameterRole = "color";
inline constexpr std::string_view kSolidWidthParameterRole = "width";
inline constexpr std::string_view kSolidHeightParameterRole = "height";
inline constexpr std::string_view kTextParameterRole = "text";
inline constexpr std::string_view kTextSizeParameterRole = "size";
inline constexpr std::string_view kTextAlignmentParameterRole = "alignment";
inline constexpr std::string_view kTextLineHeightParameterRole = "line-height";
inline constexpr std::string_view kTextLetterSpacingParameterRole = "letter-spacing";
inline constexpr std::string_view kTextFontParameterRole = "font";
// Deliberately the same role string as kSolidColorParameterRole. A role is node-local -- it names
// which binding of THIS node a parameter fills -- while the schema key is the global identity of
// the value's meaning, and a text source's color means what a solid source's color means. Keeping
// the role spelling identical is what lets one properties row, one node-card color chip, and one
// session write path serve both sources without a second branch; the two distinct schema keys are
// what keep their validation and defaults separate.
inline constexpr std::string_view kTextColorParameterRole = "color";
inline constexpr std::string_view kPositionParameterRole = "position";
inline constexpr std::string_view kAnchorParameterRole = "anchor";
inline constexpr std::string_view kScaleParameterRole = "scale";
inline constexpr std::string_view kRotationParameterRole = "rotation";
inline constexpr std::string_view kOpacityParameterRole = "opacity";
inline constexpr std::string_view kBlendModeParameterRole = "blendMode";

struct NodeRecord {
    NodeId id;
    std::string typeId;
    std::vector<ParameterBinding> parameters;
    std::uint32_t schemaVersion;

    friend bool operator==(const NodeRecord&, const NodeRecord&) = default;
};

struct OutputPortRef {
    NodeId nodeId;
    std::string port;

    friend bool operator==(const OutputPortRef&, const OutputPortRef&) = default;
};

struct NodeInputRef {
    NodeId nodeId;
    std::string port;

    friend bool operator==(const NodeInputRef&, const NodeInputRef&) = default;
};

struct LayerStackInputRef {
    NodeId stackNodeId;
    LayerSlotId slotId;
    std::string role;

    friend bool operator==(const LayerStackInputRef&, const LayerStackInputRef&) = default;
};

using InputPortRef = std::variant<NodeInputRef, LayerStackInputRef>;

struct EdgeRecord {
    EdgeId id;
    OutputPortRef source;
    InputPortRef destination;

    friend bool operator==(const EdgeRecord&, const EdgeRecord&) = default;
};

struct LayerOutputBoundary {
    NodeId nodeId;
    LayerId layerId;
    std::string name;
    std::string outputPort;

    std::optional<std::array<std::uint8_t, 3>> labelColor{};
    bool enabled = true;
    bool solo = false;
    bool locked = false;
    core::RationalTime inPoint{};
    // Zero means the composition duration, including for boundaries created before insertion.
    core::RationalTime outPoint{};
    std::optional<LayerId> parent{};
    [[nodiscard]] core::RationalTime endPoint(core::RationalTime duration) const noexcept {
        return outPoint == core::RationalTime{} ? duration : outPoint;
    }

    friend bool operator==(const LayerOutputBoundary&, const LayerOutputBoundary&) = default;
};

class CanonicalGraph final {
  public:
    explicit CanonicalGraph(NodeId layerStackNodeId) noexcept : layerStacks_{} {
        if (layerStackNodeId.isValid())
            layerStacks_.emplace_back(layerStackNodeId);
    }

    [[nodiscard]] std::span<const NodeRecord> nodes() const noexcept { return nodes_; }
    [[nodiscard]] std::span<const EdgeRecord> edges() const noexcept { return edges_; }
    [[nodiscard]] std::span<const LayerOutputBoundary> layerOutputs() const noexcept {
        return layerOutputs_;
    }
    [[nodiscard]] const NodeRecord* findNode(NodeId id) const noexcept;
    [[nodiscard]] NodeRecord* findNode(NodeId id) noexcept;

    // Legacy authoring convenience: the Merge directly feeding Output, otherwise the first Merge.
    [[nodiscard]] const LayerStack& layerStack() const noexcept;
    [[nodiscard]] LayerStack& layerStack() noexcept;
    [[nodiscard]] const LayerStack* merge(NodeId id) const noexcept;
    [[nodiscard]] LayerStack* merge(NodeId id) noexcept;
    [[nodiscard]] std::span<const LayerStack> merges() const noexcept { return layerStacks_; }
    [[nodiscard]] std::optional<NodeId> outputMergeId() const noexcept;
    [[nodiscard]] const std::optional<OutputPortRef>& compositionOutput() const noexcept {
        return compositionOutput_;
    }

    [[nodiscard]] bool addNode(NodeRecord node);
    [[nodiscard]] bool addEdge(EdgeRecord edge,
                               const NodeDefinitionRegistry& registry = builtInNodeDefinitions());
    [[nodiscard]] std::optional<SocketValueKind>
    outputKind(const OutputPortRef& output,
               const NodeDefinitionRegistry& registry = builtInNodeDefinitions()) const;
    [[nodiscard]] std::optional<SocketValueKind>
    inputKind(const InputPortRef& input,
              const NodeDefinitionRegistry& registry = builtInNodeDefinitions()) const;
    // The kind a reroute node carries: the kind of whatever feeds it, through a chain of reroutes,
    // or nothing when it is unconnected (task FIX1, item I). Both kind accessors above answer with
    // this for a reroute, so every connect-time and validation-time check asks one question.
    [[nodiscard]] std::optional<SocketValueKind>
    rerouteKind(NodeId id, const NodeDefinitionRegistry& registry = builtInNodeDefinitions()) const;
    [[nodiscard]] bool addLayerOutput(LayerOutputBoundary boundary);
    [[nodiscard]] LayerOutputBoundary* findLayer(LayerId id) noexcept;
    [[nodiscard]] const LayerOutputBoundary* findLayer(LayerId id) const noexcept;
    [[nodiscard]] bool eraseNode(NodeId id);
    [[nodiscard]] bool eraseEdge(EdgeId id);
    [[nodiscard]] bool renameLayer(LayerId id, std::string name);
    void setCompositionOutput(OutputPortRef output) { compositionOutput_ = std::move(output); }

    [[nodiscard]] ValidationResult
    validate(const ParameterStore& parameters,
             const NodeDefinitionRegistry& registry = builtInNodeDefinitions()) const;

  private:
    [[nodiscard]] std::optional<SocketValueKind>
    rerouteKind(NodeId id, const NodeDefinitionRegistry& registry,
                std::unordered_set<std::uint64_t>& seen) const;

    std::vector<NodeRecord> nodes_;
    std::vector<EdgeRecord> edges_;
    std::vector<LayerOutputBoundary> layerOutputs_;
    std::vector<LayerStack> layerStacks_;
    LayerStack emptyStack_{NodeId{}};
    std::optional<OutputPortRef> compositionOutput_;
};

} // namespace bloom::document
