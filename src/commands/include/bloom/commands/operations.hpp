#pragma once

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_operations.hpp>
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
inline constexpr std::string_view kAddTextLayerSizeParameterOutput = "sizeParameter";
inline constexpr std::string_view kAddTextLayerColorParameterOutput = "colorParameter";
inline constexpr std::string_view kAddTextLayerPositionParameterOutput = "positionParameter";
inline constexpr std::string_view kAddTextLayerAnchorParameterOutput = "anchorParameter";
inline constexpr std::string_view kAddTextLayerScaleParameterOutput = "scaleParameter";
inline constexpr std::string_view kAddTextLayerRotationParameterOutput = "rotationParameter";
inline constexpr std::string_view kAddTextLayerOpacityParameterOutput = "opacityParameter";
inline constexpr std::string_view kAddTextLayerBlendModeParameterOutput = "blendModeParameter";
inline constexpr std::string_view kAddTextLayerTextToLayerEdgeOutput = "textToLayerEdge";
inline constexpr std::string_view kAddTextLayerLayerToStackEdgeOutput = "layerToStackEdge";

inline constexpr std::string_view kAddSolidLayerLayerOutput = "layer";
inline constexpr std::string_view kAddSolidLayerSlotOutput = "slot";
inline constexpr std::string_view kAddSolidLayerSolidNodeOutput = "solidNode";
inline constexpr std::string_view kAddSolidLayerLayerOutputNodeOutput = "layerOutputNode";
inline constexpr std::string_view kAddSolidLayerColorParameterOutput = "colorParameter";
inline constexpr std::string_view kAddSolidLayerPositionParameterOutput = "positionParameter";
inline constexpr std::string_view kAddSolidLayerAnchorParameterOutput = "anchorParameter";
inline constexpr std::string_view kAddSolidLayerScaleParameterOutput = "scaleParameter";
inline constexpr std::string_view kAddSolidLayerRotationParameterOutput = "rotationParameter";
inline constexpr std::string_view kAddSolidLayerOpacityParameterOutput = "opacityParameter";
inline constexpr std::string_view kAddSolidLayerBlendModeParameterOutput = "blendModeParameter";
inline constexpr std::string_view kAddSolidLayerSolidToLayerEdgeOutput = "solidToLayerEdge";
inline constexpr std::string_view kAddSolidLayerLayerToStackEdgeOutput = "layerToStackEdge";

inline constexpr std::string_view kAddCompositionOutput = "composition";
inline constexpr std::string_view kDuplicateCompositionOutput = "composition";

class AddSolidLayer final : public Operation {
  public:
    AddSolidLayer(document::CompositionId compositionId, std::string name, core::Color4d color)
        : compositionId_(compositionId), name_(std::move(name)), color_(color),
          useCompositionCentre_(true) {}

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
    document::Vec2d position_{};
    bool useCompositionCentre_ = false;
    double opacity_ = 1.0;
};

// Creates the same canonical structured-layer topology AddSolidLayer does -- source node, Layer
// Output boundary, stack slot, and the two edges between them -- with a bloom.text-source in the
// source position. `size` is the em size in pixels and `color` is a straight reference-linear-sRGB
// authoring value, the same encoding a solid color uses; both default to the registered text
// definition's own defaults so a caller that only has content does not have to restate them.
//
// The font is not a parameter: the CPU reference path has exactly one embedded face today, so there
// is nothing to select and nothing to persist (see bloom/render/text_raster.hpp).
class AddTextLayer final : public Operation {
  public:
    AddTextLayer(document::CompositionId compositionId, std::string name, std::string text)
        : compositionId_(compositionId), name_(std::move(name)), text_(std::move(text)),
          useCompositionCentre_(true) {}

    AddTextLayer(document::CompositionId compositionId, std::string name, std::string text,
                 document::Vec2d position, double opacity = 1.0,
                 double size = document::kDefaultTextSizePixels,
                 core::Color4d color = core::Color4d{1.0, 1.0, 1.0, 1.0})
        : compositionId_(compositionId), name_(std::move(name)), text_(std::move(text)),
          position_(position), opacity_(opacity), size_(size), color_(color) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    std::string name_;
    std::string text_;
    document::Vec2d position_{};
    bool useCompositionCentre_ = false;
    double opacity_ = 1.0;
    double size_ = document::kDefaultTextSizePixels;
    core::Color4d color_{1.0, 1.0, 1.0, 1.0};
};

class SetProjectName final : public Operation {
  public:
    explicit SetProjectName(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    std::string name_;
};

class AddComposition final : public Operation {
  public:
    AddComposition(std::string name, document::CompositionFormat format,
                   document::FrameRate frameRate, core::RationalTime duration)
        : name_(std::move(name)), format_(format), frameRate_(frameRate), duration_(duration) {}

    AddComposition(std::string name, document::CompositionFormat format,
                   core::RationalTime duration)
        : AddComposition(std::move(name), format, format.frameRate(), duration) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    std::string name_;
    document::CompositionFormat format_;
    document::FrameRate frameRate_;
    core::RationalTime duration_;
};

class DeleteComposition final : public Operation {
  public:
    explicit DeleteComposition(document::CompositionId compositionId)
        : compositionId_(compositionId) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
};

class DuplicateComposition final : public Operation {
  public:
    explicit DuplicateComposition(document::CompositionId compositionId)
        : compositionId_(compositionId) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
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
