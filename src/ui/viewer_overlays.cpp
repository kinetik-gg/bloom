#include <bloom/ui/viewer_overlays.hpp>

#include <bloom/ui/kit/tokens.hpp>

#include <QFontMetrics>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace bloom::ui {
QPointF ViewerMapping::toScreen(const document::Vec2d point) const {
    return {displayRect.left() + point.x * displayRect.width() / compositionFormat.width(),
            displayRect.top() + point.y * displayRect.height() / compositionFormat.height()};
}

document::Vec2d ViewerMapping::toComposition(const QPointF point) const {
    // A degenerate or non-finite display rectangle has no composition-space answer. Returning the
    // composition origin keeps every caller's arithmetic finite; dividing anyway seeded a NaN that
    // every downstream zero-test (an empty-rect test, a `!= 0` divisor test) silently accepted,
    // and that NaN is what a gesture then offered the document.
    const double width = displayRect.width();
    const double height = displayRect.height();
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0 ||
        !std::isfinite(point.x()) || !std::isfinite(point.y())) {
        return {0.0, 0.0};
    }
    return {(point.x() - displayRect.left()) / width * compositionFormat.width(),
            (point.y() - displayRect.top()) / height * compositionFormat.height()};
}

std::array<QPointF, 8> viewerHandlePoints(const ViewerMapping& mapping,
                                          const runtime::EvaluatedOperationBounds& bounds) {
    std::array<QPointF, 8> points;
    for (std::size_t i = 0; i < 4; ++i) {
        points[i * 2] = mapping.toScreen(bounds.polygon[i]);
        points[i * 2 + 1] = (points[i * 2] + mapping.toScreen(bounds.polygon[(i + 1) % 4])) / 2;
    }
    return points;
}

ViewerHit hitTestViewer(const ViewerMapping& mapping, const QPointF screenPoint,
                        const std::span<const runtime::EvaluatedOperationBounds> topmostFirst,
                        const std::span<const runtime::EvaluatedOperationBounds> selected,
                        const std::span<const document::LayerId> pointText) {
    const auto near = [&](const QPointF point, const double radius) {
        return std::hypot(point.x() - screenPoint.x(), point.y() - screenPoint.y()) <= radius;
    };
    for (const auto& bounds : selected) {
        if (near(mapping.toScreen(bounds.anchor), kit::px(kit::Size::GizmoHandle)))
            return {bounds.layerId, ViewerHitRegion::Anchor};
        const auto handles = viewerHandlePoints(mapping, bounds);
        for (std::size_t i = 0; i < handles.size(); ++i)
            if ((i % 2 == 0 || std::ranges::find(pointText, bounds.layerId) == pointText.end()) &&
                near(handles[i], kit::px(kit::Size::GizmoHandle) / 2.0))
                return {bounds.layerId, ViewerHitRegion::Scale, static_cast<int>(i)};
        QPolygonF polygon;
        for (const auto point : bounds.polygon)
            polygon << mapping.toScreen(point);
        if (!polygon.containsPoint(screenPoint, Qt::OddEvenFill))
            for (std::size_t i = 0; i < handles.size(); i += 2)
                if ((i % 2 == 0 ||
                     std::ranges::find(pointText, bounds.layerId) == pointText.end()) &&
                    near(handles[i], kit::px(kit::Size::GizmoRotateZone)))
                    return {bounds.layerId, ViewerHitRegion::Rotate, static_cast<int>(i)};
    }
    for (const auto& bounds : topmostFirst) {
        if (!bounds.layerId.isValid() || bounds.output.empty())
            continue;
        QPolygonF polygon;
        for (const auto point : bounds.polygon)
            polygon << mapping.toScreen(point);
        if (polygon.containsPoint(screenPoint, Qt::OddEvenFill))
            return {bounds.layerId, ViewerHitRegion::Move};
    }
    return {};
}

namespace {

QRectF insetRect(const QRectF& displayRect, const double percentage) {
    const qreal horizontal = displayRect.width() * (1.0 - percentage) * 0.5;
    const qreal vertical = displayRect.height() * (1.0 - percentage) * 0.5;
    return displayRect.adjusted(horizontal, vertical, -horizontal, -vertical);
}

void drawGuideRect(QPainter& painter, const QRectF& rect, const QColor color,
                   const Qt::PenStyle style, const qreal width) {
    if (rect.isEmpty()) {
        return;
    }
    QPen pen(color, width, style);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5));
}

void paintSafeAreas(QPainter& painter, const QRectF& displayRect,
                    const ViewerOverlayOptions& options) {
    const QColor accent = kit::color(kit::Color::Accent);
    const QColor muted = kit::color(kit::Color::Muted);
    drawGuideRect(painter, insetRect(displayRect, options.safeAreaSettings.action), muted,
                  Qt::DashLine, 1.0);
    drawGuideRect(painter, insetRect(displayRect, options.safeAreaSettings.title), accent,
                  Qt::DashLine, 1.0);
    if (options.safeAreaPreset != ViewerSafeAreaPreset::Social) {
        return;
    }
    const qreal squareSide = std::min(displayRect.width(), displayRect.height());
    const QRectF square(displayRect.center().x() - squareSide * 0.5,
                        displayRect.center().y() - squareSide * 0.5, squareSide, squareSide);
    const qreal cropWidth = displayRect.height() * 0.8;
    const QRectF crop(displayRect.center().x() - cropWidth * 0.5, displayRect.top(), cropWidth,
                      displayRect.height());
    drawGuideRect(painter, square, muted, Qt::DotLine, 1.0);
    drawGuideRect(painter, crop, accent, Qt::DotLine, 1.0);
}

void paintCentreCross(QPainter& painter, const QRectF& displayRect) {
    QPen pen(kit::color(kit::Color::Accent), 1.0, Qt::DashLine);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.drawLine(QPointF(displayRect.center().x(), displayRect.top()),
                     QPointF(displayRect.center().x(), displayRect.bottom()));
    painter.drawLine(QPointF(displayRect.left(), displayRect.center().y()),
                     QPointF(displayRect.right(), displayRect.center().y()));
}

void paintThirds(QPainter& painter, const QRectF& displayRect) {
    QPen pen(kit::color(kit::Color::Muted), 1.0, Qt::DashLine);
    pen.setCosmetic(true);
    painter.setPen(pen);
    for (const qreal fraction : {1.0 / 3.0, 2.0 / 3.0}) {
        const qreal x = displayRect.left() + displayRect.width() * fraction;
        const qreal y = displayRect.top() + displayRect.height() * fraction;
        painter.drawLine(QPointF(x, displayRect.top()), QPointF(x, displayRect.bottom()));
        painter.drawLine(QPointF(displayRect.left(), y), QPointF(displayRect.right(), y));
    }
}

void paintRulers(QPainter& painter, const QRectF& canvasRect, const QRectF& displayRect,
                 const QSize compositionSize) {
    const QColor rulerColor = kit::color(kit::Color::Faint);
    const QColor tickColor = kit::color(kit::Color::Border);
    const QFont font = kit::font(kit::TypeRole::Value);
    const QFontMetrics metrics(font);
    painter.setFont(font);
    painter.setPen(QPen(tickColor, 1.0));
    const int topHeight = metrics.height() + kit::px(kit::Spacing::XS);
    const int leftWidth =
        metrics.horizontalAdvance(QStringLiteral("0000")) + kit::px(kit::Spacing::XS);
    const QRectF topBand(displayRect.left(),
                         std::max(canvasRect.top(), displayRect.top() - topHeight),
                         displayRect.width(), topHeight);
    const QRectF leftBand(std::max(canvasRect.left(), displayRect.left() - leftWidth),
                          displayRect.top(), leftWidth, displayRect.height());
    painter.fillRect(topBand, kit::color(kit::Color::Surface));
    painter.fillRect(leftBand, kit::color(kit::Color::Surface));
    painter.setPen(rulerColor);

    const auto stepFor = [](const int extent) {
        if (extent <= 64)
            return 8;
        if (extent <= 256)
            return 32;
        if (extent <= 1024)
            return 128;
        return 256;
    };
    const int xStep = stepFor(compositionSize.width());
    const int yStep = stepFor(compositionSize.height());
    for (int x = 0; x <= compositionSize.width(); x += xStep) {
        const qreal screenX =
            displayRect.left() + displayRect.width() * x / compositionSize.width();
        painter.drawLine(QPointF(screenX, topBand.bottom() - kit::px(kit::Size::PlayheadHalfWidth)),
                         QPointF(screenX, topBand.bottom()));
        painter.drawText(QPointF(screenX + 2.0, topBand.top() + metrics.ascent()),
                         QString::number(x));
    }
    for (int y = 0; y <= compositionSize.height(); y += yStep) {
        const qreal screenY =
            displayRect.top() + displayRect.height() * y / compositionSize.height();
        painter.drawLine(QPointF(leftBand.right() - kit::px(kit::Size::PlayheadHalfWidth), screenY),
                         QPointF(leftBand.right(), screenY));
        painter.save();
        painter.translate(leftBand.left() + metrics.ascent(), screenY - 2.0);
        painter.rotate(-90.0);
        painter.drawText(QPointF(0.0, 0.0), QString::number(y));
        painter.restore();
    }
}

void paintPixelGrid(QPainter& painter, const QRectF& displayRect, const QSize compositionSize,
                    const double effectiveZoom) {
    if (effectiveZoom < 4.0 || compositionSize.width() <= 0 || compositionSize.height() <= 0) {
        return;
    }
    const double scaleX = displayRect.width() / compositionSize.width();
    const double scaleY = displayRect.height() / compositionSize.height();
    const int firstX = std::max(0, static_cast<int>(std::floor(-displayRect.left() / scaleX)));
    const int firstY = std::max(0, static_cast<int>(std::floor(-displayRect.top() / scaleY)));
    const int lastX = std::min(compositionSize.width(),
                               static_cast<int>(std::ceil((displayRect.right()) / scaleX)));
    const int lastY = std::min(compositionSize.height(),
                               static_cast<int>(std::ceil((displayRect.bottom()) / scaleY)));
    painter.save();
    painter.setClipRect(displayRect);
    QPen pen(kit::color(kit::Color::Border), 1.0);
    pen.setCosmetic(true);
    painter.setPen(pen);
    for (int x = firstX; x <= lastX; ++x) {
        const qreal screenX = displayRect.left() + x * scaleX;
        painter.drawLine(QPointF(screenX, displayRect.top()),
                         QPointF(screenX, displayRect.bottom()));
    }
    for (int y = firstY; y <= lastY; ++y) {
        const qreal screenY = displayRect.top() + y * scaleY;
        painter.drawLine(QPointF(displayRect.left(), screenY),
                         QPointF(displayRect.right(), screenY));
    }
    painter.restore();
}

void paintSelectionBounds(QPainter& painter, const ViewerMapping& mapping,
                          const std::span<const runtime::EvaluatedOperationBounds> bounds,
                          const std::span<const document::LayerId> pointText) {
    const auto device = painter.deviceTransform();
    const auto inverse = device.inverted();
    const auto snap = [&](const QPointF point) {
        const auto physical = device.map(point);
        return inverse.map(QPointF(std::floor(physical.x()) + 0.5, std::floor(physical.y()) + 0.5));
    };
    QPen pen(kit::color(kit::Color::Accent), kit::px(kit::Size::Hairline));
    pen.setCosmetic(true);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const double halfHandle = kit::px(kit::Size::GizmoHandle) / 2.0;
    for (const auto& bound : bounds) {
        QPolygonF polygon;
        for (const auto point : bound.polygon)
            polygon << snap(mapping.toScreen(point));
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(polygon);
        painter.setBrush(kit::color(kit::Color::Surface));
        const auto handles = viewerHandlePoints(mapping, bound);
        for (std::size_t i = 0; i < handles.size(); ++i) {
            if (i % 2 != 0 && std::ranges::find(pointText, bound.layerId) != pointText.end())
                continue;
            const auto point = handles[i];
            const auto topLeft = snap(point - QPointF(halfHandle, halfHandle));
            const auto bottomRight = snap(point + QPointF(halfHandle, halfHandle));
            painter.drawRect(QRectF(topLeft, bottomRight));
        }
        const auto anchor = snap(mapping.toScreen(bound.anchor));
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.drawEllipse(anchor, halfHandle, halfHandle);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.drawLine(snap(anchor - QPointF(halfHandle, 0)),
                         snap(anchor + QPointF(halfHandle, 0)));
        painter.drawLine(snap(anchor - QPointF(0, halfHandle)),
                         snap(anchor + QPointF(0, halfHandle)));
    }
}

} // namespace

void paintViewerOverlays(QPainter& painter, const QRectF& canvasRect, const ViewerMapping& mapping,
                         const double effectiveZoom, const ViewerOverlayOptions& options,
                         const std::span<const runtime::EvaluatedOperationBounds> bounds,
                         const std::span<const document::LayerId> pointText) {
    const auto& displayRect = mapping.displayRect;
    const QSize compositionSize(static_cast<int>(mapping.compositionFormat.width()),
                                static_cast<int>(mapping.compositionFormat.height()));
    if (displayRect.isEmpty() || compositionSize.width() <= 0 || compositionSize.height() <= 0) {
        return;
    }
    painter.save();
    if (options.pixelGrid) {
        paintPixelGrid(painter, displayRect, compositionSize, effectiveZoom);
    }
    if (options.safeAreas) {
        paintSafeAreas(painter, displayRect, options);
    }
    if (options.thirds) {
        paintThirds(painter, displayRect);
    }
    if (options.centreCross) {
        paintCentreCross(painter, displayRect);
    }
    if (options.rulers) {
        paintRulers(painter, canvasRect, displayRect, compositionSize);
    }
    paintSelectionBounds(painter, mapping, bounds, pointText);
    painter.restore();
}

} // namespace bloom::ui
