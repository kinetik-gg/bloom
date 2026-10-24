#pragma once

// Geometry helpers shared by the Viewer tool implementations (viewer_tools*.ipp). They live in one
// header so no tool file depends on the order in which another is included.

#include <bloom/document/parameter.hpp>
#include <bloom/render/path_raster.hpp>
#include <bloom/ui/viewer_overlays.hpp>

#include <QPainterPath>
#include <QPointF>
#include <QTransform>

#include <cstddef>

namespace bloom::ui::viewer_tool_geometry {

[[nodiscard]] inline QPointF toolPoint(const document::Vec2d point) { return {point.x, point.y}; }

[[nodiscard]] inline document::Vec2d toolValue(const QPointF point) {
    return {point.x(), point.y()};
}

[[nodiscard]] inline QPainterPath toolOutline(const render::Path& path) {
    QPainterPath outline;
    if (path.anchors.empty())
        return outline;
    const auto point = [](const render::PathPoint p) { return QPointF(p.x, p.y); };
    outline.moveTo(point(path.anchors.front().point));
    const auto segments = path.closed ? path.anchors.size() : path.anchors.size() - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const auto& a = path.anchors[i];
        const auto& b = path.anchors[(i + 1) % path.anchors.size()];
        outline.cubicTo(point(a.outHandle.value_or(a.point)), point(b.inHandle.value_or(b.point)),
                        point(b.point));
    }
    if (path.closed)
        outline.closeSubpath();
    return outline;
}

[[nodiscard]] inline render::Path toolRenderPath(const document::PathValue& path) {
    render::Path result;
    result.closed = path.closed;
    for (const auto& anchor : path.anchors) {
        render::PathAnchor next{{anchor.point.x, anchor.point.y}, {}, {}};
        if (anchor.inHandle)
            next.inHandle = {anchor.inHandle->x, anchor.inHandle->y};
        if (anchor.outHandle)
            next.outHandle = {anchor.outHandle->x, anchor.outHandle->y};
        result.anchors.push_back(next);
    }
    return result;
}

[[nodiscard]] inline QTransform toolScreenTransform(const ViewerMapping& mapping) {
    QTransform result;
    result.translate(mapping.displayRect.left(), mapping.displayRect.top());
    result.scale(mapping.displayRect.width() / mapping.compositionFormat.width(),
                 mapping.displayRect.height() / mapping.compositionFormat.height());
    return result;
}

} // namespace bloom::ui::viewer_tool_geometry
