#include <array>
#include <bloom/document/shape.hpp>
#include <cmath>
namespace bloom::document {
NodeDefinition shapeDefinition() {
    NodeDefinition node{{std::string(kShapeSourceNodeType), 1},
                        NodeLoweringKind::Shape,
                        {},
                        {{"image", SocketValueKind::Image}},
                        {},
                        std::nullopt,
                        NodeCardinality::Many,
                        NodeCategory::Sources};
    const auto add = [&](std::string role, const std::string& key, ParameterValueKind kind,
                         ParameterValue value, bool animated = false) {
        node.parameters.push_back(
            {std::move(role), "bloom.shape." + key, kind, true, animated, std::move(value)});
        // Discrete selectors and the path are compile-time, inline operands.
        if (animated)
            node.inputs.push_back(
                {node.parameters.back().role, *socketKindForParameterValueKind(kind), false});
    };
    add("kind", "kind", ParameterValueKind::Integer, std::int64_t{0});
    add("size", "size", ParameterValueKind::Vec2d, Vec2d{320, 240}, true);
    add("cornerRadius", "corner-radius", ParameterValueKind::Float64, 0.0);
    add("points", "points", ParameterValueKind::Integer, std::int64_t{5});
    add("innerRatio", "inner-ratio", ParameterValueKind::Float64, 0.5);
    add("lineStart", "line-start", ParameterValueKind::Vec2d, Vec2d{});
    add("lineEnd", "line-end", ParameterValueKind::Vec2d, Vec2d{320, 0});
    add("path", "path", ParameterValueKind::Path, PathValue{});
    add("fillEnabled", "fill-enabled", ParameterValueKind::Boolean, true);
    add("fillColor", "fill-color", ParameterValueKind::Color4d, core::Color4d{1, 1, 1, 1}, true);
    add("strokeEnabled", "stroke-enabled", ParameterValueKind::Boolean, false);
    add("strokeColor", "stroke-color", ParameterValueKind::Color4d, core::Color4d{0, 0, 0, 1},
        true);
    add("strokeWidth", "stroke-width", ParameterValueKind::Float64, 2.0, true);
    add("strokeAlign", "stroke-align", ParameterValueKind::Integer, std::int64_t{0});
    add("strokeJoin", "stroke-join", ParameterValueKind::Integer, std::int64_t{0});
    add("strokeCap", "stroke-cap", ParameterValueKind::Integer, std::int64_t{0});
    add("fillRule", "fill-rule", ParameterValueKind::Integer, std::int64_t{0});
    return node;
}
bool shapeConstantMatchesSchema(std::string_view schema, const ParameterValue& value) {
    schema.remove_prefix(std::string_view("bloom.shape.").size());
    if (schema == "path") {
        const auto* path = std::get_if<PathValue>(&value);
        return path && path->isValid();
    }
    if (schema == "size" || schema == "line-start" || schema == "line-end") {
        const auto* p = std::get_if<Vec2d>(&value);
        return p && std::isfinite(p->x) && std::isfinite(p->y) &&
               (schema != "size" || (p->x >= 0 && p->y >= 0));
    }
    if (schema == "fill-color" || schema == "stroke-color") {
        const auto* c = std::get_if<core::Color4d>(&value);
        return c && c->isValid();
    }
    if (schema == "fill-enabled" || schema == "stroke-enabled")
        return std::holds_alternative<bool>(value);
    if (schema == "corner-radius" || schema == "inner-ratio" || schema == "stroke-width") {
        const auto* d = std::get_if<double>(&value);
        return d && std::isfinite(*d) && *d >= 0 && (schema != "inner-ratio" || *d <= 1);
    }
    const auto* i = std::get_if<std::int64_t>(&value);
    if (!i)
        return false;
    if (schema == "kind")
        return *i >= 0 && *i <= 6;
    if (schema == "points")
        return *i >= 3 && *i <= 64;
    if (schema == "fill-rule")
        return *i >= 0 && *i <= 1;
    return (schema == "stroke-align" || schema == "stroke-join" || schema == "stroke-cap") &&
           *i >= 0 && *i <= 2;
}
bool shapeRoleVisible(const ShapeKind kind, const std::string_view role) noexcept {
    if (role == "size")
        return kind <= ShapeKind::Star;
    if (role == "cornerRadius")
        return kind == ShapeKind::Rectangle || kind == ShapeKind::Polygon;
    if (role == "points")
        return kind == ShapeKind::Polygon || kind == ShapeKind::Star;
    if (role == "innerRatio")
        return kind == ShapeKind::Star;
    if (role == "lineStart" || role == "lineEnd")
        return kind == ShapeKind::Line;
    if (role == "path")
        return kind == ShapeKind::Path;
    if (role == "strokeCap")
        return kind == ShapeKind::Line || kind == ShapeKind::Path;
    if (role == "fillEnabled" || role == "fillColor" || role == "fillRule" || role == "strokeAlign")
        return kind != ShapeKind::Line;
    return true;
}
std::string_view shapeKindName(const ShapeKind kind) noexcept {
    constexpr std::array names{"Rectangle", "Ellipse", "Triangle", "Polygon",
                               "Star",      "Line",    "Path"};
    const auto index = static_cast<std::size_t>(kind);
    return index < names.size() ? names[index] : "Shape";
}
} // namespace bloom::document
