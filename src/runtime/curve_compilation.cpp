#include <bloom/runtime/curve_compilation.hpp>

#include <utility>

namespace bloom::runtime {
namespace {

[[nodiscard]] CompiledKeyframeInterpolation
compiledInterpolation(const document::KeyframeInterpolation mode) noexcept {
    return mode == document::KeyframeInterpolation::Hold ? CompiledKeyframeInterpolation::Hold
                                                         : CompiledKeyframeInterpolation::Linear;
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

} // namespace bloom::runtime
