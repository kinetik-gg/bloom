#pragma once

// The compiled animation curves and the indices that address them, in their own header because BOTH
// compiled programs read them: the image chain's typed parameter operands, and -- since task FIX1,
// item G -- the value graph's operands, whose literal Scalar, Vector 2 and Colour nodes can carry a
// curve of their own. Keeping them in compiled_plan.hpp would have made the value graph include the
// image plan that includes the value graph.

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bloom::runtime {

enum class CompiledKeyframeInterpolation : std::uint8_t {
    Hold,
    Linear,
    EaseInOut,
};

struct CompiledScalarKeyframe final {
    document::KeyframeId id;
    core::RationalTime time;
    double value = 0.0;
    CompiledKeyframeInterpolation outgoingInterpolation = CompiledKeyframeInterpolation::Linear;

    friend bool operator==(const CompiledScalarKeyframe&, const CompiledScalarKeyframe&) = default;
};

struct CompiledVec2Keyframe final {
    document::KeyframeId id;
    core::RationalTime time;
    document::Vec2d value;
    CompiledKeyframeInterpolation outgoingInterpolation = CompiledKeyframeInterpolation::Linear;

    friend bool operator==(const CompiledVec2Keyframe&, const CompiledVec2Keyframe&) = default;
};

struct CompiledScalarCurve final {
    document::AnimationCurveId id;
    std::vector<CompiledScalarKeyframe> keyframes;

    friend bool operator==(const CompiledScalarCurve&, const CompiledScalarCurve&) = default;
};

struct CompiledVec2Curve final {
    document::AnimationCurveId id;
    std::vector<CompiledVec2Keyframe> keyframes;

    friend bool operator==(const CompiledVec2Curve&, const CompiledVec2Curve&) = default;
};

struct CompiledColor4Keyframe final {
    document::KeyframeId id;
    core::RationalTime time;
    core::Color4d value;
    CompiledKeyframeInterpolation outgoingInterpolation = CompiledKeyframeInterpolation::Linear;

    friend bool operator==(const CompiledColor4Keyframe&, const CompiledColor4Keyframe&) = default;
};

struct CompiledColor4Curve final {
    document::AnimationCurveId id;
    std::vector<CompiledColor4Keyframe> keyframes;

    friend bool operator==(const CompiledColor4Curve&, const CompiledColor4Curve&) = default;
};

class ScalarCurveIndex final {
  public:
    [[nodiscard]] static constexpr ScalarCurveIndex fromRaw(const std::size_t value) noexcept {
        return ScalarCurveIndex(value);
    }

    [[nodiscard]] constexpr std::size_t value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const ScalarCurveIndex&,
                                      const ScalarCurveIndex&) noexcept = default;

  private:
    explicit constexpr ScalarCurveIndex(const std::size_t value) noexcept : value_(value) {}

    std::size_t value_ = 0;
};

class Vec2CurveIndex final {
  public:
    [[nodiscard]] static constexpr Vec2CurveIndex fromRaw(const std::size_t value) noexcept {
        return Vec2CurveIndex(value);
    }

    [[nodiscard]] constexpr std::size_t value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const Vec2CurveIndex&,
                                      const Vec2CurveIndex&) noexcept = default;

  private:
    explicit constexpr Vec2CurveIndex(const std::size_t value) noexcept : value_(value) {}

    std::size_t value_ = 0;
};

class Color4CurveIndex final {
  public:
    [[nodiscard]] static constexpr Color4CurveIndex fromRaw(const std::size_t value) noexcept {
        return Color4CurveIndex(value);
    }

    [[nodiscard]] constexpr std::size_t value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const Color4CurveIndex&,
                                      const Color4CurveIndex&) noexcept = default;

  private:
    explicit constexpr Color4CurveIndex(const std::size_t value) noexcept : value_(value) {}

    std::size_t value_ = 0;
};

} // namespace bloom::runtime
