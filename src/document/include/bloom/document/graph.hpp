#pragma once

#include <bloom/document/ids.hpp>
#include <bloom/document/layer_stack.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/validation.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::document {

inline constexpr std::string_view kLayerOutputNodeType = "bloom.layer-output";
inline constexpr std::string_view kLayerStackNodeType = "bloom.layer-stack";
inline constexpr std::string_view kSolidSourceNodeType = "bloom.solid-source";
inline constexpr std::string_view kTextSourceNodeType = "bloom.text-source";
inline constexpr std::string_view kCompositionOutputNodeType = "bloom.composition-output";
inline constexpr std::uint32_t kLayerOutputNodeSchemaVersion = 1;
inline constexpr std::uint32_t kLayerStackNodeSchemaVersion = 1;
inline constexpr std::uint32_t kSolidSourceNodeSchemaVersion = 1;
inline constexpr std::uint32_t kTextSourceNodeSchemaVersion = 1;
inline constexpr std::uint32_t kCompositionOutputNodeSchemaVersion = 1;

inline constexpr std::string_view kSolidSourceOutputPort = "image";
inline constexpr std::string_view kTextSourceOutputPort = "image";
inline constexpr std::string_view kLayerOutputContentInputPort = "image";
inline constexpr std::string_view kLayerOutputOutputPort = "image";
inline constexpr std::string_view kLayerStackContentInputRole = "content";
inline constexpr std::string_view kLayerStackOutputPort = "image";
inline constexpr std::string_view kCompositionOutputInputPort = "image";
inline constexpr std::string_view kCompositionOutputOutputPort = "image";

inline constexpr std::string_view kSolidColorParameterRole = "color";
inline constexpr std::string_view kTextParameterRole = "text";
inline constexpr std::string_view kPositionParameterRole = "position";
inline constexpr std::string_view kOpacityParameterRole = "opacity";

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

    friend bool operator==(const LayerOutputBoundary&, const LayerOutputBoundary&) = default;
};

class CanonicalGraph final {
  public:
    explicit CanonicalGraph(NodeId layerStackNodeId) noexcept : layerStack_(layerStackNodeId) {}

    [[nodiscard]] std::span<const NodeRecord> nodes() const noexcept { return nodes_; }
    [[nodiscard]] std::span<const EdgeRecord> edges() const noexcept { return edges_; }
    [[nodiscard]] std::span<const LayerOutputBoundary> layerOutputs() const noexcept {
        return layerOutputs_;
    }
    [[nodiscard]] const NodeRecord* findNode(NodeId id) const noexcept;
    [[nodiscard]] NodeRecord* findNode(NodeId id) noexcept;

    [[nodiscard]] const LayerStack& layerStack() const noexcept { return layerStack_; }
    [[nodiscard]] LayerStack& layerStack() noexcept { return layerStack_; }
    [[nodiscard]] const std::optional<OutputPortRef>& compositionOutput() const noexcept {
        return compositionOutput_;
    }

    [[nodiscard]] bool addNode(NodeRecord node);
    [[nodiscard]] bool addEdge(EdgeRecord edge);
    [[nodiscard]] bool addLayerOutput(LayerOutputBoundary boundary);
    void setCompositionOutput(OutputPortRef output) { compositionOutput_ = std::move(output); }

    [[nodiscard]] ValidationResult validate(const ParameterStore& parameters) const;

  private:
    std::vector<NodeRecord> nodes_;
    std::vector<EdgeRecord> edges_;
    std::vector<LayerOutputBoundary> layerOutputs_;
    LayerStack layerStack_;
    std::optional<OutputPortRef> compositionOutput_;
};

} // namespace bloom::document
