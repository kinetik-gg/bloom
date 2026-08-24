#pragma once

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/operation.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace bloom::commands {

inline constexpr std::string_view kAddTextLayerLayerOutput = "layer";
inline constexpr std::string_view kAddTextLayerSlotOutput = "slot";
inline constexpr std::string_view kAddTextLayerTextNodeOutput = "textNode";
inline constexpr std::string_view kAddTextLayerLayerOutputNodeOutput = "layerOutputNode";
inline constexpr std::string_view kAddTextLayerTextParameterOutput = "textParameter";
inline constexpr std::string_view kAddTextLayerPositionParameterOutput = "positionParameter";
inline constexpr std::string_view kAddTextLayerOpacityParameterOutput = "opacityParameter";
inline constexpr std::string_view kAddTextLayerTextToLayerEdgeOutput = "textToLayerEdge";
inline constexpr std::string_view kAddTextLayerLayerToStackEdgeOutput = "layerToStackEdge";

inline constexpr std::string_view kAddSolidLayerLayerOutput = "layer";
inline constexpr std::string_view kAddSolidLayerSlotOutput = "slot";
inline constexpr std::string_view kAddSolidLayerSolidNodeOutput = "solidNode";
inline constexpr std::string_view kAddSolidLayerLayerOutputNodeOutput = "layerOutputNode";
inline constexpr std::string_view kAddSolidLayerColorParameterOutput = "colorParameter";
inline constexpr std::string_view kAddSolidLayerPositionParameterOutput = "positionParameter";
inline constexpr std::string_view kAddSolidLayerOpacityParameterOutput = "opacityParameter";
inline constexpr std::string_view kAddSolidLayerSolidToLayerEdgeOutput = "solidToLayerEdge";
inline constexpr std::string_view kAddSolidLayerLayerToStackEdgeOutput = "layerToStackEdge";

class AddSolidLayer final : public Operation {
  public:
    AddSolidLayer(document::CompositionId compositionId, std::string name, core::Color4d color,
                  document::Vec2d position, double opacity = 1.0)
        : compositionId_(compositionId), name_(std::move(name)), color_(color), position_(position),
          opacity_(opacity) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    std::string name_;
    core::Color4d color_;
    document::Vec2d position_;
    double opacity_ = 1.0;
};

class AddTextLayer final : public Operation {
  public:
    AddTextLayer(document::CompositionId compositionId, std::string name, std::string text,
                 document::Vec2d position, double opacity = 1.0)
        : compositionId_(compositionId), name_(std::move(name)), text_(std::move(text)),
          position_(position), opacity_(opacity) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    std::string name_;
    std::string text_;
    document::Vec2d position_;
    double opacity_ = 1.0;
};

class SetProjectName final : public Operation {
  public:
    explicit SetProjectName(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    std::string name_;
};

class SetCompositionName final : public Operation {
  public:
    SetCompositionName(document::CompositionId compositionId, std::string name)
        : compositionId_(compositionId), name_(std::move(name)) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    std::string name_;
};

class SetCompositionDuration final : public Operation {
  public:
    SetCompositionDuration(document::CompositionId compositionId, core::RationalTime duration)
        : compositionId_(compositionId), duration_(duration) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    core::RationalTime duration_;
};

class SetCompositionFormat final : public Operation {
  public:
    SetCompositionFormat(document::CompositionId compositionId, document::CompositionFormat format)
        : compositionId_(compositionId), format_(format) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::CompositionFormat format_;
};

class SetParameterSource final : public Operation {
  public:
    SetParameterSource(document::CompositionId compositionId, document::ParameterId parameterId,
                       document::ParameterSource source)
        : compositionId_(compositionId), parameterId_(parameterId), source_(std::move(source)) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::ParameterId parameterId_;
    document::ParameterSource source_;
};

class MoveLayerBefore final : public Operation {
  public:
    MoveLayerBefore(document::CompositionId compositionId, document::LayerSlotId slotId,
                    std::optional<document::LayerSlotId> beforeSlotId)
        : compositionId_(compositionId), slotId_(slotId), beforeSlotId_(beforeSlotId) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::LayerSlotId slotId_;
    std::optional<document::LayerSlotId> beforeSlotId_;
};

} // namespace bloom::commands
