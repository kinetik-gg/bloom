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
    }
    return CompiledKeyframeInterpolation::Linear;
}

} // namespace

CompiledScalarCurve compileAnimationCurve(const document::ScalarAnimationCurve& curve) {
    std::vector<CompiledScalarKeyframe> keyframes;
    keyframes.reserve(curve.keyframes.size());
    for (const auto& keyframe : curve.keyframes) {
        keyframes.push_back({keyframe.id, keyframe.time, keyframe.value,
                             compiledInterpolation(keyframe.outgoingInterpolation)});
    }
    return {curve.id, std::move(keyframes)};
}

CompiledVec2Curve compileAnimationCurve(const document::Vec2AnimationCurve& curve) {
    std::vector<CompiledVec2Keyframe> keyframes;
    keyframes.reserve(curve.keyframes.size());
    for (const auto& keyframe : curve.keyframes) {
        keyframes.push_back({keyframe.id, keyframe.time, keyframe.value,
                             compiledInterpolation(keyframe.outgoingInterpolation)});
    }
    return {curve.id, std::move(keyframes)};
}

CompiledColor4Curve compileAnimationCurve(const document::Color4AnimationCurve& curve) {
    std::vector<CompiledColor4Keyframe> keyframes;
    keyframes.reserve(curve.keyframes.size());
    for (const auto& keyframe : curve.keyframes) {
        keyframes.push_back({keyframe.id, keyframe.time, keyframe.value,
                             compiledInterpolation(keyframe.outgoingInterpolation)});
    }
    return {curve.id, std::move(keyframes)};
}

} // namespace bloom::runtime
