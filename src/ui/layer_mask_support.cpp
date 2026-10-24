#include "layer_mask_support.hpp"

#include <QCoreApplication>
#include <algorithm>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/curve_compilation.hpp>
#include <cmath>

namespace bloom::ui::masks {
namespace {

using document::Vec2d;

Vec2d mapPoint(const QTransform& transform, const Vec2d point) {
    const auto mapped = transform.map(QPointF(point.x, point.y));
    return {mapped.x(), mapped.y()};
}

std::vector<QPointF> flatten(const document::PathValue& path) {
    std::vector<QPointF> points;
    const auto count = path.anchors.size();
    if (count < 2)
        return points;
    const auto segments = path.closed ? count : count - 1;
    constexpr int steps = 16;
    for (std::size_t i = 0; i < segments; ++i) {
        const auto& a = path.anchors[i];
        const auto& b = path.anchors[(i + 1) % count];
        const QPointF p0(a.point.x, a.point.y);
        const auto h0 = a.outHandle.value_or(a.point);
        const auto h1 = b.inHandle.value_or(b.point);
        const QPointF p1(h0.x, h0.y), p2(h1.x, h1.y), p3(b.point.x, b.point.y);
        for (int s = 0; s < steps; ++s) {
            const double t = static_cast<double>(s) / steps, u = 1.0 - t;
            points.push_back(u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 +
                             t * t * t * p3);
        }
    }
    if (!path.closed)
        points.emplace_back(path.anchors.back().point.x, path.anchors.back().point.y);
    return points;
}

} // namespace

std::optional<QTransform> contentToComposition(const runtime::EvaluatedOperationBounds& bounds) {
    if (bounds.local.empty())
        return std::nullopt;
    const auto point = [](const Vec2d p) { return QPointF(p.x, p.y); };
    const auto x = (point(bounds.polygon[1]) - point(bounds.polygon[0])) /
                   (bounds.local.right - bounds.local.left);
    const auto y = (point(bounds.polygon[3]) - point(bounds.polygon[0])) /
                   (bounds.local.bottom - bounds.local.top);
    const auto origin = point(bounds.polygon[0]) - x * bounds.local.left - y * bounds.local.top;
    const QTransform result(x.x(), x.y(), y.x(), y.y(), origin.x(), origin.y());
    if (!result.isInvertible() || !std::isfinite(result.determinant()) ||
        !std::isfinite(origin.x()) || !std::isfinite(origin.y()))
        return std::nullopt;
    return result;
}

document::PathValue mappedPath(const document::PathValue& path, const QTransform& transform) {
    auto result = path;
    for (auto& anchor : result.anchors) {
        anchor.point = mapPoint(transform, anchor.point);
        if (anchor.inHandle)
            anchor.inHandle = mapPoint(transform, *anchor.inHandle);
        if (anchor.outHandle)
            anchor.outHandle = mapPoint(transform, *anchor.outHandle);
    }
    return result;
}

document::PathValue rectanglePath(const QRectF& rect) {
    document::PathValue path;
    path.closed = true;
    path.anchors = {{{rect.left(), rect.top()}, {}, {}},
                    {{rect.right(), rect.top()}, {}, {}},
                    {{rect.right(), rect.bottom()}, {}, {}},
                    {{rect.left(), rect.bottom()}, {}, {}}};
    return path;
}

document::PathValue ellipsePath(const QRectF& rect) {
    const auto centre = rect.center();
    const double rx = rect.width() / 2.0, ry = rect.height() / 2.0;
    const double kx = rx * kEllipseKappa, ky = ry * kEllipseKappa;
    document::PathValue path;
    path.closed = true;
    // Top, right, bottom, left, clockwise on screen.
    path.anchors = {{{centre.x(), centre.y() - ry},
                     Vec2d{centre.x() - kx, centre.y() - ry},
                     Vec2d{centre.x() + kx, centre.y() - ry}},
                    {{centre.x() + rx, centre.y()},
                     Vec2d{centre.x() + rx, centre.y() - ky},
                     Vec2d{centre.x() + rx, centre.y() + ky}},
                    {{centre.x(), centre.y() + ry},
                     Vec2d{centre.x() + kx, centre.y() + ry},
                     Vec2d{centre.x() - kx, centre.y() + ry}},
                    {{centre.x() - rx, centre.y()},
                     Vec2d{centre.x() - rx, centre.y() + ky},
                     Vec2d{centre.x() - rx, centre.y() - ky}}};
    for (auto& anchor : path.anchors)
        anchor.mode = document::PathAnchorMode::Mirrored;
    return path;
}

std::optional<document::PathValue> maskPathAt(const document::Composition& composition,
                                              const document::LayerMask& mask,
                                              const core::RationalTime keyTime) {
    const auto* record = composition.parameters().find(mask.path);
    if (record == nullptr)
        return std::nullopt;
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&record->source)) {
        if (const auto* path = std::get_if<document::PathValue>(&constant->value))
            return *path;
        return std::nullopt;
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&record->source);
    const auto* curve = source ? composition.animationCurves().findPath(source->curveId) : nullptr;
    if (curve == nullptr || curve->keyframes.empty())
        return std::nullopt;
    return runtime::samplePathCurve(runtime::compileAnimationCurve(*curve), keyTime);
}

QPainterPath outline(const document::PathValue& path) {
    QPainterPath result;
    if (path.anchors.empty())
        return result;
    const auto point = [](const Vec2d p) { return QPointF(p.x, p.y); };
    result.moveTo(point(path.anchors.front().point));
    const auto count = path.anchors.size();
    const auto segments = path.closed ? count : count - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const auto& a = path.anchors[i];
        const auto& b = path.anchors[(i + 1) % count];
        result.cubicTo(point(a.outHandle.value_or(a.point)), point(b.inHandle.value_or(b.point)),
                       point(b.point));
    }
    if (path.closed)
        result.closeSubpath();
    return result;
}

QPainterPath offsetOutline(const document::PathValue& path, const double distance) {
    QPainterPath result;
    if (!path.closed)
        return result;
    const auto points = flatten(path);
    const auto n = points.size();
    if (n < 3)
        return result;
    double area = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& p = points[i];
        const auto& q = points[(i + 1) % n];
        area += p.x() * q.y() - q.x() * p.y();
    }
    // In y-down coordinates a positive area winds clockwise on screen; its outward normal is the
    // edge direction rotated by -90 degrees.
    const double outward = area >= 0.0 ? 1.0 : -1.0;
    const auto normal = [&](const QPointF& from, const QPointF& to) {
        const QPointF edge = to - from;
        const double length = std::hypot(edge.x(), edge.y());
        if (length == 0.0)
            return QPointF();
        return QPointF(edge.y(), -edge.x()) * (outward / length);
    };
    for (std::size_t i = 0; i < n; ++i) {
        const auto& previous = points[(i + n - 1) % n];
        const auto& current = points[i];
        const auto& next = points[(i + 1) % n];
        const auto shifted =
            current + (normal(previous, current) + normal(current, next)) * (distance / 2.0);
        if (i == 0)
            result.moveTo(shifted);
        else
            result.lineTo(shifted);
    }
    result.closeSubpath();
    return result;
}

QString modeText(const document::LayerMaskMode mode) {
    using Mode = document::LayerMaskMode;
    switch (mode) {
    case Mode::None:
        return QCoreApplication::translate("masks", "None");
    case Mode::Add:
        return QCoreApplication::translate("masks", "Add");
    case Mode::Subtract:
        return QCoreApplication::translate("masks", "Subtract");
    case Mode::Intersect:
        return QCoreApplication::translate("masks", "Intersect");
    case Mode::Difference:
        return QCoreApplication::translate("masks", "Difference");
    }
    return {};
}

QString channelText(const document::TrackMatteChannel channel) {
    return channel == document::TrackMatteChannel::Luma
               ? QCoreApplication::translate("masks", "Luma")
               : QCoreApplication::translate("masks", "Alpha");
}

std::vector<document::LayerId> matteSourceCandidates(const document::Composition& composition,
                                                     const document::LayerId layer) {
    std::vector<document::LayerId> result;
    for (const auto& stack : composition.graph().merges()) {
        const auto entries = stack.entries();
        if (std::ranges::none_of(entries, [&](const auto& e) { return e.layerId == layer; }))
            continue;
        for (const auto& entry : entries)
            if (entry.layerId.isValid() && entry.layerId != layer)
                result.push_back(entry.layerId);
    }
    return result;
}

} // namespace bloom::ui::masks
