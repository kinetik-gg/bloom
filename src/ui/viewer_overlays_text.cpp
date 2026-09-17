#include <QPainter>
#include <algorithm>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/viewer_overlays.hpp>
#include <cmath>
#include <limits>

namespace bloom::ui {
QLineF textLayoutCaret(const render::TextLayout& layout, const std::size_t byte) {
    if (layout.lines.empty())
        return {};
    const auto* line = &layout.lines.front();
    for (const auto& candidate : layout.lines)
        if (candidate.byteRange.begin <= byte)
            line = &candidate;
    double x = line->box.x;
    for (const auto& glyph : layout.glyphs) {
        if (glyph.line != line->index)
            continue;
        if (glyph.byteOffset >= byte) {
            x = glyph.advanceRect.x;
            break;
        }
        x = glyph.advanceRect.x + glyph.advanceRect.width;
    }
    return {x, line->box.y, x, line->box.y + layout.caretHeight};
}
std::size_t nearestTextLayoutBoundary(const render::TextLayout& layout, const QPointF point) {
    if (layout.lines.empty())
        return 0;
    const auto* line = &layout.lines.front();
    double distance = std::numeric_limits<double>::max();
    for (const auto& candidate : layout.lines) {
        const double next = std::abs(point.y() - candidate.box.y - layout.caretHeight / 2);
        if (next < distance) {
            distance = next;
            line = &candidate;
        }
    }
    auto byte = line->byteRange.begin;
    distance = std::abs(point.x() - line->box.x);
    const auto consider = [&](std::size_t nextByte, double x) {
        const double next = std::abs(point.x() - x);
        if (next < distance) {
            distance = next;
            byte = nextByte;
        }
    };
    for (const auto& glyph : layout.glyphs)
        if (glyph.line == line->index)
            consider(glyph.byteOffset, glyph.advanceRect.x);
    consider(line->byteRange.end, line->box.x + line->box.width);
    return byte;
}
void paintViewerTextEdit(QPainter& painter, const QRectF& clip, const QTransform& transform,
                         const render::TextLayout& layout, const std::size_t cursor,
                         const std::size_t first, const std::size_t last, const bool preedit,
                         const bool caretVisible) {
    painter.save();
    painter.setClipRect(clip);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto accent = kit::color(kit::Color::Accent);
    QPen pen(accent, kit::px(kit::Size::Hairline));
    pen.setCosmetic(true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(kit::withOpacity(accent, kit::kDisabledOpacity));
    for (const auto& glyph : layout.glyphs) {
        if (glyph.byteOffset < first || glyph.byteOffset >= last)
            continue;
        const auto& r = glyph.advanceRect;
        const QRectF rect(r.x, r.y, r.width, r.height);
        if (preedit) {
            painter.setPen(pen);
            painter.drawLine(transform.map(QLineF(rect.bottomLeft(), rect.bottomRight())));
        } else {
            painter.drawPolygon(transform.map(QPolygonF(rect)));
        }
    }
    if (caretVisible) {
        painter.setPen(pen);
        auto caret = transform.map(textLayoutCaret(layout, cursor));
        const auto dpr = painter.device()->devicePixelRatioF();
        const auto snap = [dpr](const QPointF p) {
            return QPointF((std::floor(p.x() * dpr) + 0.5) / dpr,
                           (std::floor(p.y() * dpr) + 0.5) / dpr);
        };
        caret.setP1(snap(caret.p1()));
        caret.setP2(snap(caret.p2()));
        painter.drawLine(caret);
    }
    painter.restore();
}
} // namespace bloom::ui
