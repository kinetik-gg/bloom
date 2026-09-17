#pragma once

// The compiled animation curves and the indices that address them, in their own header because BOTH
// compiled programs read them: the image chain's typed parameter operands, and -- since task FIX1,
// item G -- the value graph's operands, whose literal Scalar, Vector 2 and Colour nodes can carry a
// curve of their own. Keeping them in compiled_plan.hpp would have made the value graph include the
// image plan that includes the value graph.

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <utility>
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
    // The ease handles travel with the compiled key. They are the document's own value type rather
    // than a mirrored copy: the interpolation enum is mirrored so a new document mode has to be
    // mapped deliberately, but a handle is a pair of doubles with one definition of "default", and
    // a second copy of that definition is exactly the drift the sampler's bitwise test cannot
    // afford.
    document::KeyframeHandle outgoingHandle{};
    document::KeyframeHandle incomingHandle{};

    friend bool operator==(const CompiledScalarKeyframe&, const CompiledScalarKeyframe&) = default;
};

struct CompiledScalarCurve final {
    document::AnimationCurveId id;
    std::vector<CompiledScalarKeyframe> keyframes;

    friend bool operator==(const CompiledScalarCurve&, const CompiledScalarCurve&) = default;
};

// A compiled vector or colour curve is a table PER COMPONENT plus the parameter's own default for
// a component that carries no key at all. There is no whole-value table: sampling composes the
// typed value from the components, which is what kAnimationSamplingSemanticsVersion 2 already
// describes.
struct CompiledVec2Curve final {
    document::AnimationCurveId id;
    std::array<std::vector<CompiledScalarKeyframe>, 2> components{};
    document::Vec2d defaultValue{};

    friend bool operator==(const CompiledVec2Curve&, const CompiledVec2Curve&) = default;
};

struct CompiledVec3Curve final {
    document::AnimationCurveId id;
    std::array<std::vector<CompiledScalarKeyframe>, 3> components{};
    document::Vec3d defaultValue{};

    friend bool operator==(const CompiledVec3Curve&, const CompiledVec3Curve&) = default;
};

struct CompiledColor4Curve final {
    document::AnimationCurveId id;
    std::array<std::vector<CompiledScalarKeyframe>, 4> components{};
    core::Color4d defaultValue{};

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

class Vec3CurveIndex final {
  public:
    [[nodiscard]] static constexpr Vec3CurveIndex fromRaw(const std::size_t value) noexcept {
        return Vec3CurveIndex(value);
    }
    [[nodiscard]] constexpr std::size_t value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const Vec3CurveIndex&,
                                      const Vec3CurveIndex&) noexcept = default;

  private:
    explicit constexpr Vec3CurveIndex(const std::size_t value) noexcept : value_(value) {}
    std::size_t value_ = 0;
};

} // namespace bloom::runtime
