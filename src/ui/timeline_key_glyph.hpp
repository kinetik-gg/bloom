#pragma once

// The ONE key glyph. It lived inside timeline_ruler.cpp until the graph editor needed to draw the
// same keys on a different canvas; a second copy of these three shapes would have let a lane and
// the graph disagree about what a Hold key looks like, which is exactly the kind of drift a shared
// vocabulary exists to prevent. Header-only and free of widget state so both painters share one
// body rather than one interface.

#include <bloom/document/animation.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QPainter>
#include <QPointF>
#include <QPolygonF>

namespace bloom::ui {

// The glyph for one interpolation mode (task S5, item 2: "timeline keyframe glyph differs per
// interpolation"). The shapes are the standard timeline vocabulary, and each one says what the
// SEGMENT LEAVING the key does:
//
//   Linear     a diamond  -- the shape every key had before this task; a straight ramp out
//   Hold       a square   -- a held step out, drawn with the same corner-to-corner extent
//   EaseInOut  a circle   -- a rounded departure, matching the rounded ease it names
//
// Shape, not colour, carries the mode: the gold/Accent colour pair is already spoken for by
// unselected/selected, and overloading it would make a selected Hold key indistinguishable from an
// unselected eased one.
inline void paintKeyGlyph(QPainter& painter, const QPointF center,
                          const document::KeyframeInterpolation interpolation) {
    switch (interpolation) {
    case document::KeyframeInterpolation::Hold: {
        const qreal half = kit::kKeyDiamondRadius * 0.78;
        painter.drawRect(QRectF(center.x() - half, center.y() - half, 2.0 * half, 2.0 * half));
        return;
    }
    case document::KeyframeInterpolation::EaseInOut:
        painter.drawEllipse(center, kit::kKeyDiamondRadius * 0.92, kit::kKeyDiamondRadius * 0.92);
        return;
    case document::KeyframeInterpolation::Linear:
        break;
    }
    QPolygonF diamond;
    diamond << QPointF(center.x(), center.y() - kit::kKeyDiamondRadius)
            << QPointF(center.x() + kit::kKeyDiamondRadius, center.y())
            << QPointF(center.x(), center.y() + kit::kKeyDiamondRadius)
            << QPointF(center.x() - kit::kKeyDiamondRadius, center.y());
    painter.drawPolygon(diamond);
}

} // namespace bloom::ui
