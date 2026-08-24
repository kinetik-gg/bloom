#pragma once

#include <bloom/core/id.hpp>

#include <algorithm>
#include <compare>
#include <cstdint>
#include <limits>
#include <optional>

namespace bloom::document {

struct ProjectIdTag;
struct CompositionIdTag;
struct NodeIdTag;
struct EdgeIdTag;
struct LayerIdTag;
struct LayerSlotIdTag;
struct ParameterIdTag;
struct AnimationCurveIdTag;
struct KeyframeIdTag;
struct DriverBindingIdTag;
struct ExtensionRecordIdTag;

using ProjectId = core::Id<ProjectIdTag>;
using CompositionId = core::Id<CompositionIdTag>;
using NodeId = core::Id<NodeIdTag>;
using EdgeId = core::Id<EdgeIdTag>;
using LayerId = core::Id<LayerIdTag>;
using LayerSlotId = core::Id<LayerSlotIdTag>;
using ParameterId = core::Id<ParameterIdTag>;
using AnimationCurveId = core::Id<AnimationCurveIdTag>;
using KeyframeId = core::Id<KeyframeIdTag>;
using DriverBindingId = core::Id<DriverBindingIdTag>;
using ExtensionRecordId = core::Id<ExtensionRecordIdTag>;

struct IdAllocatorHighWater final {
    std::uint64_t composition = 0;
    std::uint64_t node = 0;
    std::uint64_t edge = 0;
    std::uint64_t layer = 0;
    std::uint64_t layerSlot = 0;
    std::uint64_t parameter = 0;
    std::uint64_t animationCurve = 0;
    std::uint64_t keyframe = 0;
    std::uint64_t driverBinding = 0;
    std::uint64_t extensionRecord = 0;

    friend constexpr auto operator<=>(const IdAllocatorHighWater&,
                                      const IdAllocatorHighWater&) noexcept = default;
};

class IdAllocator final {
  public:
    constexpr IdAllocator() noexcept = default;

    [[nodiscard]] static constexpr IdAllocator
    fromHighWater(const IdAllocatorHighWater highWater) noexcept {
        return IdAllocator(highWater);
    }

    [[nodiscard]] constexpr IdAllocatorHighWater highWater() const noexcept {
        return {
            .composition = composition_,
            .node = node_,
            .edge = edge_,
            .layer = layer_,
            .layerSlot = layerSlot_,
            .parameter = parameter_,
            .animationCurve = animationCurve_,
            .keyframe = keyframe_,
            .driverBinding = driverBinding_,
            .extensionRecord = extensionRecord_,
        };
    }

    [[nodiscard]] std::optional<CompositionId> allocateComposition() noexcept {
        return allocate<CompositionId>(composition_);
    }
    [[nodiscard]] std::optional<NodeId> allocateNode() noexcept { return allocate<NodeId>(node_); }
    [[nodiscard]] std::optional<EdgeId> allocateEdge() noexcept { return allocate<EdgeId>(edge_); }
    [[nodiscard]] std::optional<LayerId> allocateLayer() noexcept {
        return allocate<LayerId>(layer_);
    }
    [[nodiscard]] std::optional<LayerSlotId> allocateLayerSlot() noexcept {
        return allocate<LayerSlotId>(layerSlot_);
    }
    [[nodiscard]] std::optional<ParameterId> allocateParameter() noexcept {
        return allocate<ParameterId>(parameter_);
    }
    [[nodiscard]] std::optional<AnimationCurveId> allocateAnimationCurve() noexcept {
        return allocate<AnimationCurveId>(animationCurve_);
    }
    [[nodiscard]] std::optional<KeyframeId> allocateKeyframe() noexcept {
        return allocate<KeyframeId>(keyframe_);
    }
    [[nodiscard]] std::optional<DriverBindingId> allocateDriverBinding() noexcept {
        return allocate<DriverBindingId>(driverBinding_);
    }
    [[nodiscard]] std::optional<ExtensionRecordId> allocateExtensionRecord() noexcept {
        return allocate<ExtensionRecordId>(extensionRecord_);
    }

    void reserveExisting(CompositionId id) noexcept { reserve(id, composition_); }
    void reserveExisting(NodeId id) noexcept { reserve(id, node_); }
    void reserveExisting(EdgeId id) noexcept { reserve(id, edge_); }
    void reserveExisting(LayerId id) noexcept { reserve(id, layer_); }
    void reserveExisting(LayerSlotId id) noexcept { reserve(id, layerSlot_); }
    void reserveExisting(ParameterId id) noexcept { reserve(id, parameter_); }
    void reserveExisting(AnimationCurveId id) noexcept { reserve(id, animationCurve_); }
    void reserveExisting(KeyframeId id) noexcept { reserve(id, keyframe_); }
    void reserveExisting(DriverBindingId id) noexcept { reserve(id, driverBinding_); }
    void reserveExisting(ExtensionRecordId id) noexcept { reserve(id, extensionRecord_); }

    [[nodiscard]] constexpr bool covers(const CompositionId id) const noexcept {
        return coversId(id, composition_);
    }
    [[nodiscard]] constexpr bool covers(const NodeId id) const noexcept {
        return coversId(id, node_);
    }
    [[nodiscard]] constexpr bool covers(const EdgeId id) const noexcept {
        return coversId(id, edge_);
    }
    [[nodiscard]] constexpr bool covers(const LayerId id) const noexcept {
        return coversId(id, layer_);
    }
    [[nodiscard]] constexpr bool covers(const LayerSlotId id) const noexcept {
        return coversId(id, layerSlot_);
    }
    [[nodiscard]] constexpr bool covers(const ParameterId id) const noexcept {
        return coversId(id, parameter_);
    }
    [[nodiscard]] constexpr bool covers(const AnimationCurveId id) const noexcept {
        return coversId(id, animationCurve_);
    }
    [[nodiscard]] constexpr bool covers(const KeyframeId id) const noexcept {
        return coversId(id, keyframe_);
    }
    [[nodiscard]] constexpr bool covers(const DriverBindingId id) const noexcept {
        return coversId(id, driverBinding_);
    }
    [[nodiscard]] constexpr bool covers(const ExtensionRecordId id) const noexcept {
        return coversId(id, extensionRecord_);
    }

    void mergeHighWater(const IdAllocator& other) noexcept {
        composition_ = std::max(composition_, other.composition_);
        node_ = std::max(node_, other.node_);
        edge_ = std::max(edge_, other.edge_);
        layer_ = std::max(layer_, other.layer_);
        layerSlot_ = std::max(layerSlot_, other.layerSlot_);
        parameter_ = std::max(parameter_, other.parameter_);
        animationCurve_ = std::max(animationCurve_, other.animationCurve_);
        keyframe_ = std::max(keyframe_, other.keyframe_);
        driverBinding_ = std::max(driverBinding_, other.driverBinding_);
        extensionRecord_ = std::max(extensionRecord_, other.extensionRecord_);
    }

  private:
    explicit constexpr IdAllocator(const IdAllocatorHighWater highWater) noexcept
        : composition_(highWater.composition), node_(highWater.node), edge_(highWater.edge),
          layer_(highWater.layer), layerSlot_(highWater.layerSlot), parameter_(highWater.parameter),
          animationCurve_(highWater.animationCurve), keyframe_(highWater.keyframe),
          driverBinding_(highWater.driverBinding), extensionRecord_(highWater.extensionRecord) {}

    template <core::TypedId IdType>
    [[nodiscard]] static std::optional<IdType> allocate(std::uint64_t& highestIssued) noexcept {
        if (highestIssued == std::numeric_limits<std::uint64_t>::max()) {
            return std::nullopt;
        }

        ++highestIssued;
        return IdType::fromRaw(highestIssued);
    }

    template <core::TypedId IdType>
    static void reserve(const IdType id, std::uint64_t& highestIssued) noexcept {
        if (id.isValid()) {
            highestIssued = std::max(highestIssued, id.value());
        }
    }

    template <core::TypedId IdType>
    [[nodiscard]] static constexpr bool coversId(const IdType id,
                                                 const std::uint64_t highestIssued) noexcept {
        return id.isValid() && id.value() <= highestIssued;
    }

    std::uint64_t composition_ = 0;
    std::uint64_t node_ = 0;
    std::uint64_t edge_ = 0;
    std::uint64_t layer_ = 0;
    std::uint64_t layerSlot_ = 0;
    std::uint64_t parameter_ = 0;
    std::uint64_t animationCurve_ = 0;
    std::uint64_t keyframe_ = 0;
    std::uint64_t driverBinding_ = 0;
    std::uint64_t extensionRecord_ = 0;
};

} // namespace bloom::document
