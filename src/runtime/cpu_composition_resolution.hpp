#ifndef BLOOM_RUNTIME_CPU_COMPOSITION_RESOLUTION_HPP
#define BLOOM_RUNTIME_CPU_COMPOSITION_RESOLUTION_HPP

// Shared parameter-resolution helper extracted verbatim from cpu_composition_evaluator.cpp so the
// CPU evaluator and the GPU scene preparation use ONE copy of the exact resolution arithmetic. The
// three animatable typed operands (Vec2/Color4/Scalar) resolve a constant, a value-graph output
// (a per-frame driver), or a curve sample; the untyped family resolves a constant or a driver. The
// definitions are `inline` (the evaluator's originals had internal linkage) so including this
// header in more than one runtime translation unit is well-formed and cannot change CPU behaviour.
//
// This header is PRIVATE to src/runtime; it is not a public runtime API.

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include "cpu_composition_evaluator_support.hpp"

#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime::detail {

// One driven parameter's value, read out of the frame's already-evaluated value graph. A value of
// the wrong alternative cannot be substituted here -- there is no defensible fallback for "this
// parameter wanted a colour and the graph produced a string" -- so it is reported as an invalid
// plan, which is what a compiler that honoured the document's typing could never produce.
template <typename Value>
[[nodiscard]] const Value* resolvedValue(const ValueOutputIndex output,
                                         const ResolvedEvaluation& resolved) noexcept {
    const auto index = output.value();
    if (index >= resolved.valueOutputs.size()) {
        return nullptr;
    }
    return std::get_if<Value>(&resolved.valueOutputs[index]);
}

template <typename Value> struct ResolvedParameter final {
    Value value;
    document::ParameterId parameterId;
    std::optional<document::AnimationCurveId> animationCurveId;
    std::optional<document::KeyframeId> keyframeId;
};

[[nodiscard]] inline std::optional<ResolvedParameter<document::Vec2d>>
resolveParameter(const CompiledVec2Parameter& parameter, const CompiledCompositionPlan& plan,
                 const ResolvedEvaluation& resolved) noexcept {
    if (const auto* constant = std::get_if<document::Vec2d>(&parameter.source)) {
        return ResolvedParameter<document::Vec2d>{*constant, parameter.id, std::nullopt,
                                                  std::nullopt};
    }
    if (const auto* driven = std::get_if<ValueOutputIndex>(&parameter.source)) {
        const auto* value = resolvedValue<document::Vec2d>(*driven, resolved);
        return value == nullptr ? std::nullopt
                                : std::optional(ResolvedParameter<document::Vec2d>{
                                      *value, parameter.id, std::nullopt, std::nullopt});
    }
    const auto* curve = std::get_if<Vec2CurveIndex>(&parameter.source);
    if (curve == nullptr) {
        return std::nullopt;
    }
    const auto index = curve->value();
    if (index >= resolved.vec2CurveValues.size() || index >= plan.vec2Curves().size()) {
        return std::nullopt;
    }
    const auto& sample = resolved.vec2CurveValues[index];
    return ResolvedParameter<document::Vec2d>{sample.value, parameter.id,
                                              plan.vec2Curves()[index].id, sample.segmentStart};
}

[[nodiscard]] inline std::optional<ResolvedParameter<core::Color4d>>
resolveParameter(const CompiledColorParameter& parameter, const CompiledCompositionPlan& plan,
                 const ResolvedEvaluation& resolved) noexcept {
    if (const auto* constant = std::get_if<core::Color4d>(&parameter.source)) {
        return ResolvedParameter<core::Color4d>{*constant, parameter.id, std::nullopt,
                                                std::nullopt};
    }
    if (const auto* driven = std::get_if<ValueOutputIndex>(&parameter.source)) {
        const auto* value = resolvedValue<core::Color4d>(*driven, resolved);
        return value == nullptr ? std::nullopt
                                : std::optional(ResolvedParameter<core::Color4d>{
                                      *value, parameter.id, std::nullopt, std::nullopt});
    }
    const auto* curve = std::get_if<Color4CurveIndex>(&parameter.source);
    if (curve == nullptr) {
        return std::nullopt;
    }
    const auto index = curve->value();
    if (index >= resolved.color4CurveValues.size() || index >= plan.color4Curves().size()) {
        return std::nullopt;
    }
    const auto& sample = resolved.color4CurveValues[index];
    return ResolvedParameter<core::Color4d>{sample.value, parameter.id,
                                            plan.color4Curves()[index].id, sample.segmentStart};
}

[[nodiscard]] inline std::optional<ResolvedParameter<double>>
resolveParameter(const CompiledScalarParameter& parameter, const CompiledCompositionPlan& plan,
                 const ResolvedEvaluation& resolved) noexcept {
    if (const auto* constant = std::get_if<double>(&parameter.source)) {
        return ResolvedParameter<double>{*constant, parameter.id, std::nullopt, std::nullopt};
    }
    if (const auto* driven = std::get_if<ValueOutputIndex>(&parameter.source)) {
        const auto* value = resolvedValue<double>(*driven, resolved);
        return value == nullptr ? std::nullopt
                                : std::optional(ResolvedParameter<double>{
                                      *value, parameter.id, std::nullopt, std::nullopt});
    }
    const auto* curve = std::get_if<ScalarCurveIndex>(&parameter.source);
    if (curve == nullptr) {
        return std::nullopt;
    }
    const auto index = curve->value();
    if (index >= resolved.scalarCurveValues.size() || index >= plan.scalarCurves().size()) {
        return std::nullopt;
    }
    const auto& sample = resolved.scalarCurveValues[index];
    return ResolvedParameter<double>{sample.value, parameter.id, plan.scalarCurves()[index].id,
                                     sample.segmentStart};
}

// Task DRIVE-1. The fourth member of this family, for the parameter kinds that cannot interpolate
// -- a String, an Integer (every enum-backed one included) and a Boolean. They have no curve table
// to index, so unlike their three animatable siblings they answer only the two questions that are
// left: the authored constant the compiler resolved, or the value-graph output a driver binding
// hands them this frame. A driven output of the wrong alternative answers nothing, for exactly the
// reason resolvedValue() gives -- there is no defensible substitute for "this parameter wanted a
// String and the graph produced a colour" -- so a malformed plan is diagnosed rather than trusted.
template <typename Value>
[[nodiscard]] inline std::optional<ResolvedParameter<Value>>
resolveParameter(const document::ParameterId id, const Value& authored,
                 const std::optional<ValueOutputIndex>& driven,
                 const ResolvedEvaluation& resolved) noexcept {
    if (!driven.has_value()) {
        return ResolvedParameter<Value>{authored, id, std::nullopt, std::nullopt};
    }
    const auto* value = resolvedValue<Value>(*driven, resolved);
    return value == nullptr
               ? std::nullopt
               : std::optional(ResolvedParameter<Value>{*value, id, std::nullopt, std::nullopt});
}

// The blend mode one layer composites with at this frame. An authored mode and a driven one are the
// same closed set of modes, because both go through core::blendModeFromStoredValue(): a driven
// Integer naming no implemented mode answers nothing rather than silently compositing Normal. The
// document cannot store such an integer -- ParameterStore refuses it on insert and validation
// refuses it on publication -- so only a malformed plan can produce one.
[[nodiscard]] inline std::optional<core::BlendMode>
resolveParameter(const CompiledLayerOutput& layer, const ResolvedEvaluation& resolved) noexcept {
    const auto stored =
        resolveParameter(layer.blendModeParameterId, core::blendModeStoredValue(layer.blendMode),
                         layer.drivenBlendMode, resolved);
    return stored.has_value() ? core::blendModeFromStoredValue(stored->value) : std::nullopt;
}

template <typename Value>
[[nodiscard]] EvaluationSubject parameterSubject(EvaluationSubject subject,
                                                 const ResolvedParameter<Value>& parameter,
                                                 std::string field) {
    subject.parameterId = parameter.parameterId;
    subject.animationCurveId = parameter.animationCurveId;
    subject.keyframeId = parameter.keyframeId;
    subject.field = std::move(field);
    return subject;
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_CPU_COMPOSITION_RESOLUTION_HPP
