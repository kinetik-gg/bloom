#pragma once
#include <bloom/document/node_definition_registry.hpp>
namespace bloom::document {
inline constexpr std::string_view kShapeSourceNodeType = "bloom.shape-source";
// Closed, durable Integer mappings under bloom.shape.kind / stroke-align / stroke-join /
// stroke-cap / fill-rule. Values must never be reordered.
enum class ShapeKind : std::int64_t { Rectangle, Ellipse, Triangle, Polygon, Star, Line, Path };
enum class ShapeStrokeAlign : std::int64_t { Center, Inside, Outside };
enum class ShapeStrokeJoin : std::int64_t { Miter, Round, Bevel };
enum class ShapeStrokeCap : std::int64_t { Butt, Round, Square };
enum class ShapeFillRule : std::int64_t { NonZero, EvenOdd };
[[nodiscard]] NodeDefinition shapeDefinition();
[[nodiscard]] bool shapeConstantMatchesSchema(std::string_view schema, const ParameterValue& value);
[[nodiscard]] bool shapeRoleVisible(ShapeKind kind, std::string_view role) noexcept;
[[nodiscard]] std::string_view shapeKindName(ShapeKind kind) noexcept;
} // namespace bloom::document
