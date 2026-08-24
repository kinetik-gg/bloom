#pragma once

#include <bloom/core/color.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace bloom::runtime {

class OperationIndex final {
  public:
    [[nodiscard]] static constexpr OperationIndex fromRaw(const std::size_t value) noexcept {
        return OperationIndex(value);
    }

    [[nodiscard]] constexpr std::size_t value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const OperationIndex&,
                                      const OperationIndex&) noexcept = default;

  private:
    explicit constexpr OperationIndex(const std::size_t value) noexcept : value_(value) {}

    std::size_t value_ = 0;
};

struct CompiledSolid {
    document::NodeId sourceNodeId;
    document::ParameterId colorParameterId;
    core::Color4d color;

    friend bool operator==(const CompiledSolid&, const CompiledSolid&) = default;
};

struct CompiledLayerOutput {
    document::NodeId sourceNodeId;
    document::LayerId layerId;
    OperationIndex input;
    document::ParameterId positionParameterId;
    document::Vec2d position;
    document::ParameterId opacityParameterId;
    double opacity = 1.0;

    friend bool operator==(const CompiledLayerOutput&, const CompiledLayerOutput&) = default;
};

struct CompiledLayerStackEntry {
    document::LayerSlotId slotId;
    document::LayerId layerId;
    OperationIndex input;

    friend bool operator==(const CompiledLayerStackEntry&,
                           const CompiledLayerStackEntry&) = default;
};

struct CompiledLayerStack {
    document::NodeId sourceNodeId;
    std::vector<CompiledLayerStackEntry> entries;

    friend bool operator==(const CompiledLayerStack&, const CompiledLayerStack&) = default;
};

struct CompiledCompositionOutput {
    document::NodeId sourceNodeId;
    OperationIndex input;

    friend bool operator==(const CompiledCompositionOutput&,
                           const CompiledCompositionOutput&) = default;
};

using CompiledOperation =
    std::variant<CompiledSolid, CompiledLayerOutput, CompiledLayerStack, CompiledCompositionOutput>;

struct CompiledCompositionPlan {
    document::Revision sourceRevision;
    document::ProjectId projectId;
    document::CompositionId compositionId;
    document::CompositionFormat format;
    std::vector<CompiledOperation> operations;
    OperationIndex output;

    friend bool operator==(const CompiledCompositionPlan&,
                           const CompiledCompositionPlan&) = default;
};

} // namespace bloom::runtime
