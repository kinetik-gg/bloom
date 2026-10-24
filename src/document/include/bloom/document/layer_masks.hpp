#pragma once

#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/validation.hpp>

#include <array>
#include <string_view>
#include <utility>

// ADR 0028 layer masks: the parameter set each mask owns and its document validation.
namespace bloom::document {

inline constexpr std::string_view kMaskNodeType = "bloom.mask";
inline constexpr std::string_view kMaskNodeOutputPort = "mask";

// The Mask node: one path with the layer-mask controls, producing a Mask over the canvas.
[[nodiscard]] NodeDefinition maskDefinition();

[[nodiscard]] bool isLayerMaskParameterSchemaKey(std::string_view schemaKey) noexcept;

// The four parameters a mask owns, paired with the schema each must carry.
[[nodiscard]] std::array<std::pair<ParameterId, std::string_view>, 4>
layerMaskParameters(const LayerMask& mask) noexcept;

// Mask ids are unique in the composition; every mask parameter resolves, carries its schema, is
// owned by exactly that mask (never bound by a node), and is a constant inside its schema domain
// or an animation curve (whose keys validateAnimationCurveReferences checks).
[[nodiscard]] ValidationResult validateLayerMasks(const CanonicalGraph& graph,
                                                  const ParameterStore& parameters);

} // namespace bloom::document
