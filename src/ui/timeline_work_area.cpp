#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>
#include <cmath>
namespace bloom::ui {
void TimelineWorkAreaStrip::setRuler(TimelineRuler& ruler) {
    ruler_ = &ruler;
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::SizeHorCursor);
    connect(&ruler, &TimelineRuler::axisChanged, this, [this] {
        preview_.reset();
        update();
    });
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        preview_.reset();
        update();
    });
    connect(&session_, &CompositionSession::compositionChanged, this, [this] {
        preview_.reset();
        update();
    });
}
std::optional<TimelineAxis> TimelineWorkAreaStrip::axis() const {
    const auto* composition = session_.composition();
    if (!composition)
        return std::nullopt;
    return ruler_ ? ruler_->axisForWidth(width()) : TimelineAxis::create(*composition, width());
}
void TimelineWorkAreaStrip::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kit::color(kit::Color::Surface));
    const auto mapping = axis();
    if (!mapping)
        return;
    const auto range = preview_.value_or(session_.workArea());
    const qreal left = mapping->pixelForTime(range.start), right = mapping->pixelForTime(range.end);
    painter.fillRect(QRectF(left, 0, right - left, height()),
                     kit::withOpacity(kit::color(kit::Color::Accent), kit::kDisabledOpacity));
    const int grip = kit::px(kit::Spacing::XXS);
    painter.fillRect(QRectF(left, 0, grip, height()), kit::color(kit::Color::Accent));
    painter.fillRect(QRectF(right - grip, 0, grip, height()), kit::color(kit::Color::Accent));
}
void TimelineWorkAreaStrip::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    const auto mapping = axis();
    if (!mapping)
        return;
    const auto area = session_.workArea();
    const auto x = event->position().x();
    const auto left = std::abs(x - mapping->pixelForTime(area.start));
    const auto right = std::abs(x - mapping->pixelForTime(area.end));
    if (std::min(left, right) > kit::px(kit::Spacing::M))
        return;
    startHandle_ = left <= right;
    preview_ = area;
    dragRevision_ = session_.snapshot().revision();
    setFocus();
    event->accept();
}
void TimelineWorkAreaStrip::mouseMoveEvent(QMouseEvent* event) {
    if (!preview_)
        return;
    const auto mapping = axis();
    if (!mapping)
        return;
    auto time =
        frameTimeForIndex(mapping->frameRate, mapping->duration,
                          mapping->frameIndexForPixel(static_cast<int>(event->position().x())));
    if (mapping->secondsForPixel(event->position().x()) >= mapping->duration.toSeconds())
        time = mapping->duration;
    if (!time)
        return;
    auto next = *preview_;
    if (startHandle_)
        next.start = *time;
    else
        next.end = *time;
    if (next.start < next.end)
        preview_ = next;
    update();
    event->accept();
}
void TimelineWorkAreaStrip::mouseReleaseEvent(QMouseEvent* event) {
    if (!preview_ || event->button() != Qt::LeftButton)
        return;
    mouseMoveEvent(event);
    if (!preview_)
        return;
    const auto area = *preview_;
    preview_.reset();
    commands::Transaction transaction("Set Work Area", dragRevision_);
    transaction.emplace<commands::SetWorkArea>(session_.compositionId(), area.start, area.end);
    (void)session_.executeTransaction(std::move(transaction));
    update();
}
void TimelineWorkAreaStrip::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    preview_.reset();
    commands::Transaction transaction("Reset Work Area", session_.snapshot().revision());
    transaction.emplace<commands::ClearWorkArea>(session_.compositionId());
    (void)session_.executeTransaction(std::move(transaction));
}
void TimelineWorkAreaStrip::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        preview_.reset();
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
} // namespace bloom::ui
