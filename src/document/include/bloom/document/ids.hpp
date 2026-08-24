#pragma once

#include <bloom/core/id.hpp>

#include <algorithm>
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

class IdAllocator final {
  public:
    [[nodiscard]] std::optional<CompositionId> allocateComposition() noexcept {
        return allocate<CompositionId>(nextComposition_);
    }
    [[nodiscard]] std::optional<NodeId> allocateNode() noexcept {
        return allocate<NodeId>(nextNode_);
    }
    [[nodiscard]] std::optional<EdgeId> allocateEdge() noexcept {
        return allocate<EdgeId>(nextEdge_);
    }
    [[nodiscard]] std::optional<LayerId> allocateLayer() noexcept {
        return allocate<LayerId>(nextLayer_);
    }
    [[nodiscard]] std::optional<LayerSlotId> allocateLayerSlot() noexcept {
        return allocate<LayerSlotId>(nextLayerSlot_);
    }
    [[nodiscard]] std::optional<ParameterId> allocateParameter() noexcept {
        return allocate<ParameterId>(nextParameter_);
    }
    [[nodiscard]] std::optional<AnimationCurveId> allocateAnimationCurve() noexcept {
        return allocate<AnimationCurveId>(nextAnimationCurve_);
    }
    [[nodiscard]] std::optional<KeyframeId> allocateKeyframe() noexcept {
        return allocate<KeyframeId>(nextKeyframe_);
    }
    [[nodiscard]] std::optional<DriverBindingId> allocateDriverBinding() noexcept {
        return allocate<DriverBindingId>(nextDriverBinding_);
    }

    void reserveExisting(CompositionId id) noexcept { reserve(id, nextComposition_); }
    void reserveExisting(NodeId id) noexcept { reserve(id, nextNode_); }
    void reserveExisting(EdgeId id) noexcept { reserve(id, nextEdge_); }
    void reserveExisting(LayerId id) noexcept { reserve(id, nextLayer_); }
    void reserveExisting(LayerSlotId id) noexcept { reserve(id, nextLayerSlot_); }
    void reserveExisting(ParameterId id) noexcept { reserve(id, nextParameter_); }
    void reserveExisting(AnimationCurveId id) noexcept { reserve(id, nextAnimationCurve_); }
    void reserveExisting(KeyframeId id) noexcept { reserve(id, nextKeyframe_); }
    void reserveExisting(DriverBindingId id) noexcept { reserve(id, nextDriverBinding_); }

    void mergeHighWater(const IdAllocator& other) noexcept {
        merge(nextComposition_, other.nextComposition_);
        merge(nextNode_, other.nextNode_);
        merge(nextEdge_, other.nextEdge_);
        merge(nextLayer_, other.nextLayer_);
        merge(nextLayerSlot_, other.nextLayerSlot_);
        merge(nextParameter_, other.nextParameter_);
        merge(nextAnimationCurve_, other.nextAnimationCurve_);
        merge(nextKeyframe_, other.nextKeyframe_);
        merge(nextDriverBinding_, other.nextDriverBinding_);
    }

  private:
    template <core::TypedId IdType>
    [[nodiscard]] static std::optional<IdType> allocate(std::uint64_t& next) noexcept {
        if (next == 0) {
            return std::nullopt;
        }

        const auto allocated = IdType::fromRaw(next);
        if (next == std::numeric_limits<std::uint64_t>::max()) {
            next = 0;
        } else {
            ++next;
        }
        return allocated;
    }

    template <core::TypedId IdType> static void reserve(IdType id, std::uint64_t& next) noexcept {
        if (!id.isValid() || next == 0 || id.value() < next) {
            return;
        }

        if (id.value() == std::numeric_limits<std::uint64_t>::max()) {
            next = 0;
        } else {
            next = id.value() + 1;
        }
    }

    static void merge(std::uint64_t& next, const std::uint64_t otherNext) noexcept {
        if (next == 0 || otherNext == 0) {
            next = 0;
            return;
        }
        next = std::max(next, otherNext);
    }

    std::uint64_t nextComposition_ = 1;
    std::uint64_t nextNode_ = 1;
    std::uint64_t nextEdge_ = 1;
    std::uint64_t nextLayer_ = 1;
    std::uint64_t nextLayerSlot_ = 1;
    std::uint64_t nextParameter_ = 1;
    std::uint64_t nextAnimationCurve_ = 1;
    std::uint64_t nextKeyframe_ = 1;
    std::uint64_t nextDriverBinding_ = 1;
};

} // namespace bloom::document
