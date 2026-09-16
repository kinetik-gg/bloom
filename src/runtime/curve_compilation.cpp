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
                             compiledInterpolation(keyframe.outgoingInterpolation),
                             keyframe.outgoingHandle, keyframe.incomingHandle});
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
    CompiledVec2Curve result{curve.id, std::move(keyframes)};
    result.defaultValue = document::Vec2d{};
    for (std::size_t index = 0; index < curve.components.size(); ++index) {
        auto& target = result.components[index];
        target.reserve(curve.components[index].keyframes.size());
        for (const auto& keyframe : curve.components[index].keyframes)
            target.push_back({keyframe.id, keyframe.time, keyframe.value,
                              compiledInterpolation(keyframe.outgoingInterpolation),
                              keyframe.outgoingHandle, keyframe.incomingHandle});
    }
    return result;
}

CompiledVec3Curve compileAnimationCurve(const document::Vec3AnimationCurve& curve) {
    std::vector<CompiledVec3Keyframe> keyframes;
    keyframes.reserve(curve.keyframes.size());
    for (const auto& keyframe : curve.keyframes)
        keyframes.push_back({keyframe.id, keyframe.time, keyframe.value,
                             compiledInterpolation(keyframe.outgoingInterpolation)});
    CompiledVec3Curve result{curve.id, std::move(keyframes)};
    result.defaultValue = document::Vec3d{};
    for (std::size_t index = 0; index < curve.components.size(); ++index) {
        auto& target = result.components[index];
        target.reserve(curve.components[index].keyframes.size());
        for (const auto& keyframe : curve.components[index].keyframes)
            target.push_back({keyframe.id, keyframe.time, keyframe.value,
                              compiledInterpolation(keyframe.outgoingInterpolation),
                              keyframe.outgoingHandle, keyframe.incomingHandle});
    }
    return result;
}

CompiledColor4Curve compileAnimationCurve(const document::Color4AnimationCurve& curve) {
    std::vector<CompiledColor4Keyframe> keyframes;
    keyframes.reserve(curve.keyframes.size());
    for (const auto& keyframe : curve.keyframes) {
        keyframes.push_back({keyframe.id, keyframe.time, keyframe.value,
                             compiledInterpolation(keyframe.outgoingInterpolation)});
    }
    CompiledColor4Curve result{curve.id, std::move(keyframes)};
    result.defaultValue = core::Color4d{};
    for (std::size_t index = 0; index < curve.components.size(); ++index) {
        auto& target = result.components[index];
        target.reserve(curve.components[index].keyframes.size());
        for (const auto& keyframe : curve.components[index].keyframes)
            target.push_back({keyframe.id, keyframe.time, keyframe.value,
                              compiledInterpolation(keyframe.outgoingInterpolation),
                              keyframe.outgoingHandle, keyframe.incomingHandle});
    }
    return result;
}

} // namespace bloom::runtime
