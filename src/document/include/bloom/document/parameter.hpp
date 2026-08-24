#pragma once

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/validation.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::document {

inline constexpr std::string_view kSolidColorParameterSchemaKey = "bloom.solid.color";
inline constexpr std::string_view kTextParameterSchemaKey = "bloom.text.content";
inline constexpr std::string_view kPositionParameterSchemaKey = "bloom.transform.position";
inline constexpr std::string_view kOpacityParameterSchemaKey = "bloom.layer.opacity";

// The initial solid schema owns straight/unassociated RGBA authoring values in this encoding.
// Evaluation converts them to the canonical premultiplied image representation.
inline constexpr std::string_view kSolidColorEncoding = "bloom.reference.linear-srgb";

struct Vec2d {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const Vec2d&, const Vec2d&) = default;
};

using ParameterValue =
    std::variant<bool, std::int64_t, double, Vec2d, core::Color4d, std::string, core::RationalTime>;

struct ConstantValueSource {
    ParameterValue value;

    friend bool operator==(const ConstantValueSource&, const ConstantValueSource&) = default;
};

struct AnimationCurveSource {
    AnimationCurveId curveId;

    friend bool operator==(const AnimationCurveSource&, const AnimationCurveSource&) = default;
};

struct DriverBindingSource {
    DriverBindingId driverId;

    friend bool operator==(const DriverBindingSource&, const DriverBindingSource&) = default;
};

using ParameterSource =
    std::variant<ConstantValueSource, AnimationCurveSource, DriverBindingSource>;

struct ParameterRecord {
    ParameterId id;
    std::string schemaKey;
    ParameterSource source;

    friend bool operator==(const ParameterRecord&, const ParameterRecord&) = default;
};

struct ParameterBinding {
    std::string role;
    ParameterId parameterId;

    friend bool operator==(const ParameterBinding&, const ParameterBinding&) = default;
};

class ParameterStore final {
  public:
    [[nodiscard]] std::span<const ParameterRecord> records() const noexcept { return records_; }
    [[nodiscard]] const ParameterRecord* find(ParameterId id) const noexcept;

    [[nodiscard]] bool insert(ParameterRecord record);
    [[nodiscard]] bool erase(ParameterId id);
    [[nodiscard]] bool setSource(ParameterId id, ParameterSource source);

    [[nodiscard]] ValidationResult validate() const;

  private:
    std::vector<ParameterRecord> records_;
};

} // namespace bloom::document
