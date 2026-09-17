#include <bloom/ui/text_box_overlay.hpp>

#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <utility>

namespace bloom::ui {
namespace {

constexpr qreal kMinimumBoxExtent = kit::kHairlineWidth;

[[nodiscard]] QRectF normalizedPositive(QRectF box) noexcept {
    box = box.normalized();
    if (box.width() < kMinimumBoxExtent)
        box.setWidth(kMinimumBoxExtent);
    if (box.height() < kMinimumBoxExtent)
        box.setHeight(kMinimumBoxExtent);
    return box;
}

[[nodiscard]] QPointF handleCentre(const QRectF& box, const TextBoxHandle handle) noexcept {
    switch (handle) {
    case TextBoxHandle::TopLeft:
        return box.topLeft();
    case TextBoxHandle::Top:
        return {box.center().x(), box.top()};
    case TextBoxHandle::TopRight:
        return box.topRight();
    case TextBoxHandle::Right:
        return {box.right(), box.center().y()};
    case TextBoxHandle::BottomRight:
        return box.bottomRight();
    case TextBoxHandle::Bottom:
        return {box.center().x(), box.bottom()};
    case TextBoxHandle::BottomLeft:
        return box.bottomLeft();
    case TextBoxHandle::Left:
        return {box.left(), box.center().y()};
    }
    return box.topLeft();
}

[[nodiscard]] bool isCorner(const TextBoxHandle handle) noexcept {
    return handle == TextBoxHandle::TopLeft || handle == TextBoxHandle::TopRight ||
           handle == TextBoxHandle::BottomRight || handle == TextBoxHandle::BottomLeft;
}

} // namespace

TextBoxOverlayGeometry textBoxOverlayGeometry(QRectF box, const qreal handleSize) {
    box = box.normalized();
    const qreal half = std::max(1.0, handleSize) / 2.0;
    TextBoxOverlayGeometry geometry{.box = box};
    for (std::size_t index = 0; index < geometry.handles.size(); ++index) {
        const auto handle = static_cast<TextBoxHandle>(index);
        const auto centre = handleCentre(box, handle);
        geometry.handles[index] =
            QRectF(centre.x() - half, centre.y() - half, half * 2.0, half * 2.0);
    }
    return geometry;
}

std::optional<TextBoxHandle> hitTestTextBoxHandle(const TextBoxOverlayGeometry& geometry,
                                                  const QPointF point) {
    for (std::size_t index = 0; index < geometry.handles.size(); ++index)
        if (geometry.handles[index].contains(point))
            return static_cast<TextBoxHandle>(index);
    return std::nullopt;
}

void paintTextBoxOverlay(QPainter& painter, const QRectF box, const bool active) {
    const auto geometry = textBoxOverlayGeometry(box);
    painter.save();
    QPen border(kit::color(active ? kit::Color::Accent : kit::Color::Border));
    border.setStyle(Qt::DashLine);
    border.setWidthF(kit::kHairlineWidth);
    painter.setPen(border);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(geometry.box);
    if (active) {
        painter.setPen(kit::color(kit::Color::Accent));
        painter.setBrush(kit::color(kit::Color::SurfaceRaised));
        for (const auto& handle : geometry.handles)
            painter.drawRect(handle);
    }
    painter.restore();
}

void TextBoxResizeInteraction::begin(const QRectF box, const TextBoxHandle handle,
                                     const QPointF press, const QRectF composition, Commit commit,
                                     const bool shift, const qreal snapDistance) {
    original_ = normalizedPositive(box);
    box_ = original_;
    composition_ = composition.normalized();
    handle_ = handle;
    commit_ = std::move(commit);
    snapDistance_ = std::max(0.0, snapDistance);
    active_ = true;
    update(press, shift);
}

QPointF TextBoxResizeInteraction::snapped(const QPointF point) const noexcept {
    QPointF result = point;
    const auto snap = [this](const qreal value, const qreal edge) {
        return std::abs(value - edge) <= snapDistance_ ? edge : value;
    };
    result.setX(snap(result.x(), composition_.left()));
    result.setX(snap(result.x(), composition_.right()));
    result.setY(snap(result.y(), composition_.top()));
    result.setY(snap(result.y(), composition_.bottom()));
    return result;
}

QRectF TextBoxResizeInteraction::resized(const QPointF cursor, const bool shift) const noexcept {
    const QPointF point = snapped(cursor);
    qreal left = original_.left();
    qreal right = original_.right();
    qreal top = original_.top();
    qreal bottom = original_.bottom();
    const bool leftHandle = handle_ == TextBoxHandle::TopLeft || handle_ == TextBoxHandle::Left ||
                            handle_ == TextBoxHandle::BottomLeft;
    const bool rightHandle = handle_ == TextBoxHandle::TopRight ||
                             handle_ == TextBoxHandle::Right ||
                             handle_ == TextBoxHandle::BottomRight;
    const bool topHandle = handle_ == TextBoxHandle::TopLeft || handle_ == TextBoxHandle::Top ||
                           handle_ == TextBoxHandle::TopRight;
    const bool bottomHandle = handle_ == TextBoxHandle::BottomLeft ||
                              handle_ == TextBoxHandle::Bottom ||
                              handle_ == TextBoxHandle::BottomRight;
    if (leftHandle)
        left = std::min(point.x(), right - kMinimumBoxExtent);
    if (rightHandle)
        right = std::max(point.x(), left + kMinimumBoxExtent);
    if (topHandle)
        top = std::min(point.y(), bottom - kMinimumBoxExtent);
    if (bottomHandle)
        bottom = std::max(point.y(), top + kMinimumBoxExtent);

    if (shift && isCorner(handle_)) {
        const qreal aspect = original_.width() / original_.height();
        const qreal width = std::max(kMinimumBoxExtent, right - left);
        const qreal height = std::max(kMinimumBoxExtent, bottom - top);
        if (width / height > aspect) {
            const qreal adjusted = width / aspect;
            if (topHandle)
                top = bottom - adjusted;
            else
                bottom = top + adjusted;
        } else {
            const qreal adjusted = height * aspect;
            if (leftHandle)
                left = right - adjusted;
            else
                right = left + adjusted;
        }
    }
    return normalizedPositive(QRectF(QPointF(left, top), QPointF(right, bottom)));
}

void TextBoxResizeInteraction::update(const QPointF cursor, const bool shift) {
    if (active_)
        box_ = resized(cursor, shift);
}

void TextBoxResizeInteraction::release() {
    if (!active_)
        return;
    active_ = false;
    if (commit_ && box_ != original_)
        commit_(box_);
    commit_ = {};
}

void TextBoxResizeInteraction::cancel() noexcept {
    active_ = false;
    box_ = original_;
    commit_ = {};
}

} // namespace bloom::ui
