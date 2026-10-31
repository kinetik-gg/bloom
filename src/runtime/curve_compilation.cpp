#include <bloom/runtime/curve_compilation.hpp>

#include <utility>

namespace bloom::runtime {
namespace {

// A total switch, not a ternary: a new document interpolation enumerator must be mapped
// deliberately rather than silently collapse into Linear (which is exactly what the ternary this
// replaced did). `-Werror=switch` makes the omission a build failure.
[[nodiscard]] CompiledKeyframeInterpolation
compiledInterpolation(const document::KeyframeInterpolation mode) noexcept {
    switch (mode) {
    case document::KeyframeInterpolation::Hold:
        return CompiledKeyframeInterpolation::Hold;
    case document::KeyframeInterpolation::Linear:
        return CompiledKeyframeInterpolation::Linear;
    case document::KeyframeInterpolation::EaseInOut:
        return CompiledKeyframeInterpolation::EaseInOut;
    case document::KeyframeInterpolation::Sine:
        return CompiledKeyframeInterpolation::Sine;
    case document::KeyframeInterpolation::Quad:
        return CompiledKeyframeInterpolation::Quad;
    case document::KeyframeInterpolation::Cubic:
        return CompiledKeyframeInterpolation::Cubic;
    case document::KeyframeInterpolation::Quart:
        return CompiledKeyframeInterpolation::Quart;
    case document::KeyframeInterpolation::Quint:
        return CompiledKeyframeInterpolation::Quint;
    case document::KeyframeInterpolation::Expo:
        return CompiledKeyframeInterpolation::Expo;
    case document::KeyframeInterpolation::Circ:
        return CompiledKeyframeInterpolation::Circ;
    case document::KeyframeInterpolation::Back:
        return CompiledKeyframeInterpolation::Back;
    case document::KeyframeInterpolation::Bounce:
        return CompiledKeyframeInterpolation::Bounce;
    case document::KeyframeInterpolation::Elastic:
        return CompiledKeyframeInterpolation::Elastic;
    }
    return CompiledKeyframeInterpolation::Linear;
}

// One component table, mapped key for key. Exactly compileAnimationCurve(ScalarAnimationCurve)'s
// body, which is the point: a component IS a scalar curve.
void compileComponent(const document::ComponentAnimationCurve& component,
                      std::vector<CompiledScalarKeyframe>& target) {
    target.reserve(component.keyframes.size());
    for (const auto& keyframe : component.keyframes)
        target.push_back({keyframe.id, keyframe.time, keyframe.value,
                          compiledInterpolation(keyframe.outgoingInterpolation),
                          keyframe.outgoingHandle, keyframe.incomingHandle, keyframe.easing});
}

} // namespace

CompiledScalarCurve compileAnimationCurve(const document::ScalarAnimationCurve& curve) {
    std::vector<CompiledScalarKeyframe> keyframes;
    keyframes.reserve(curve.keyframes.size());
    for (const auto& keyframe : curve.keyframes) {
        keyframes.push_back({keyframe.id, keyframe.time, keyframe.value,
                             compiledInterpolation(keyframe.outgoingInterpolation),
                             keyframe.outgoingHandle, keyframe.incomingHandle, keyframe.easing});
    }
    return {curve.id, std::move(keyframes)};
}

CompiledVec2Curve compileAnimationCurve(const document::Vec2AnimationCurve& curve) {
    CompiledVec2Curve result;
    result.id = curve.id;
    for (std::size_t index = 0; index < curve.components.size(); ++index)
        compileComponent(curve.components[index], result.components[index]);
    return result;
}

CompiledVec3Curve compileAnimationCurve(const document::Vec3AnimationCurve& curve) {
    CompiledVec3Curve result;
    result.id = curve.id;
    for (std::size_t index = 0; index < curve.components.size(); ++index)
        compileComponent(curve.components[index], result.components[index]);
    return result;
}

CompiledColor4Curve compileAnimationCurve(const document::Color4AnimationCurve& curve) {
    CompiledColor4Curve result;
    result.id = curve.id;
    for (std::size_t index = 0; index < curve.components.size(); ++index)
        compileComponent(curve.components[index], result.components[index]);
    return result;
}

CompiledPathCurve compileAnimationCurve(const document::PathAnimationCurve& curve) {
    CompiledPathCurve result;
    result.id = curve.id;
    result.keyframes.reserve(curve.keyframes.size());
    for (const auto& keyframe : curve.keyframes)
        result.keyframes.push_back({keyframe.id, keyframe.time, keyframe.value,
                                    compiledInterpolation(keyframe.outgoingInterpolation),
                                    keyframe.outgoingHandle, keyframe.incomingHandle,
                                    keyframe.easing});
    return result;
}

} // namespace bloom::runtime
