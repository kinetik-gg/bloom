#include <bloom/ui/viewer_overlays.hpp>

#include <bloom/ui/kit/tokens.hpp>

#include <QFontMetrics>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace bloom::ui {
namespace {

QPointF toScreen(const QRectF& displayRect, const QSize compositionSize,
                 const document::Vec2d point) {
    return {displayRect.left() + point.x * displayRect.width() / compositionSize.width(),
            displayRect.top() + point.y * displayRect.height() / compositionSize.height()};
}

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

void paintSelectionBounds(QPainter& painter, const QRectF& displayRect, const QSize compositionSize,
                          std::span<const runtime::EvaluatedOperationBounds> bounds) {
    for (const auto& bound : bounds) {
        QPolygonF polygon;
        for (const auto point : bound.polygon) {
            polygon << toScreen(displayRect, compositionSize, point);
        }
        painter.setPen(QPen(kit::color(kit::Color::Accent), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(polygon);
        painter.setBrush(kit::color(kit::Color::Accent));
        painter.setPen(Qt::NoPen);
        const auto anchor = toScreen(displayRect, compositionSize, bound.anchor);
        painter.drawEllipse(anchor, 3.0, 3.0);
    }
}

} // namespace

void paintViewerOverlays(QPainter& painter, const QRectF& canvasRect, const QRectF& displayRect,
                         const QSize compositionSize, const double effectiveZoom,
                         const ViewerOverlayOptions& options,
                         const std::span<const runtime::EvaluatedOperationBounds> bounds) {
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
    paintSelectionBounds(painter, displayRect, compositionSize, bounds);
    painter.restore();
}

} // namespace bloom::ui
