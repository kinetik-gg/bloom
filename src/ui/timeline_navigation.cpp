#include <bloom/ui/timeline_ruler.hpp>

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/timeline_frame_math.hpp>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace bloom::ui {

QString formatTimelineFrameLabel(const std::uint64_t frame, const document::FrameRate rate,
                                 const bool timecode) {
    if (!timecode) {
        return QString::number(frame);
    }
    const auto numerator = static_cast<std::uint64_t>(rate.numerator());
    const auto denominator = static_cast<std::uint64_t>(rate.denominator());
    const auto nominal = std::max<std::uint64_t>(1, (numerator + denominator / 2) / denominator);
    const auto seconds = frame / nominal;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'))
        .arg(frame % nominal, 2, 10, QLatin1Char('0'));
}

std::optional<TimelineAxis> TimelineAxis::create(const document::Composition& composition,
                                                 const int widthPixels) {
    const auto rate = composition.format().frameRate();
    const auto duration = composition.duration();
    const auto maximum = maxFrameIndex(rate, duration);
    if (!maximum.has_value()) {
        return std::nullopt;
    }
    return TimelineAxis{rate, duration, widthPixels, *maximum, 0.0, duration.toSeconds()};
}

void TimelineAxis::zoomToRange(const double start, const double end) noexcept {
    if (!std::isfinite(start) || !std::isfinite(end) || !(end > start)) {
        return;
    }
    const double total = duration.toSeconds();
    const double minimum = std::min(total, static_cast<double>(frameRate.denominator()) /
                                               static_cast<double>(frameRate.numerator()));
    const double span = std::clamp(end - start, minimum, total);
    t0 = std::clamp(start, 0.0, total - span);
    t1 = t0 + span;
}

void TimelineAxis::zoomToFit() noexcept {
    t0 = 0.0;
    t1 = duration.toSeconds();
}

double TimelineAxis::secondsForPixel(const qreal pixelX) const noexcept {
    return t0 + pixelX / std::max(1, widthPixels - 1) * (t1 - t0);
}

qreal TimelineAxis::pixelForSeconds(const double seconds) const noexcept {
    return t1 > t0 ? (seconds - t0) / (t1 - t0) * std::max(0, widthPixels - 1) : 0.0;
}

qreal TimelineAxis::pixelForTime(const core::RationalTime time) const noexcept {
    return pixelForSeconds(time.toSeconds());
}

double TimelineAxis::pixelsPerFrame() const noexcept {
    return t1 > t0 ? std::max(1, widthPixels - 1) / (t1 - t0) *
                         static_cast<double>(frameRate.denominator()) /
                         static_cast<double>(frameRate.numerator())
                   : 1.0;
}

std::uint64_t TimelineAxis::frameIndexForPixel(const int pixelX) const noexcept {
    const int pixel = std::clamp(pixelX, 0, std::max(0, widthPixels - 1));
    const auto span = static_cast<std::int64_t>(std::max(1, widthPixels - 1));
    // Fit has an exact rational pixel fraction. Keep its halfway ties exact without forcing
    // viewport zoom (a presentation value) into the document's rational-time representation.
    if (t0 == 0.0 && t1 == duration.toSeconds() &&
        duration.denominator() <= std::numeric_limits<std::int64_t>::max() / span &&
        (pixel == 0 || duration.numerator() <= std::numeric_limits<std::int64_t>::max() / pixel)) {
        const auto time =
            core::RationalTime::create(duration.numerator() * pixel, duration.denominator() * span);
        if (time.has_value()) {
            const auto index = nearestFrameIndexForTime(frameRate, duration, *time);
            if (index.has_value()) {
                return *index;
            }
        }
    }
    const long double seconds = static_cast<long double>(t0) +
                                static_cast<long double>(pixel) / span *
                                    (static_cast<long double>(t1) - static_cast<long double>(t0));
    const long double index =
        std::floor(seconds * frameRate.numerator() / frameRate.denominator() + 0.5L);
    if (index <= 0.0L) {
        return 0;
    }
    if (index >= static_cast<long double>(maxIndex)) {
        return maxIndex;
    }
    return static_cast<std::uint64_t>(index);
}

std::optional<TimelineAxis> TimelineRuler::axisForWidth(const int widthPixels) const {
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return std::nullopt;
    }
    auto axis = TimelineAxis::create(*composition, widthPixels);
    if (axis.has_value() && visibleEnd_ > visibleStart_) {
        axis->zoomToRange(visibleStart_, visibleEnd_);
    }
    return axis;
}

void TimelineRuler::zoomToRange(const double start, const double end) {
    auto axis = axisForWidth(width());
    if (!axis.has_value()) {
        return;
    }
    axis->zoomToRange(start, end);
    if (visibleStart_ == axis->t0 && visibleEnd_ == axis->t1) {
        return;
    }
    visibleStart_ = axis->t0;
    visibleEnd_ = axis->t1;
    emit axisChanged();
}

void TimelineRuler::zoomToFit() {
    visibleStart_ = 0.0;
    visibleEnd_ = 0.0;
    emit axisChanged();
}

void TimelineRuler::zoomBy(const double factor, const qreal anchorX) {
    const auto axis = axisForWidth(width());
    if (!axis.has_value() || !std::isfinite(factor) || factor <= 0.0) {
        return;
    }
    const double fraction = std::clamp(anchorX / std::max(1, width() - 1), 0.0, 1.0);
    const double anchor = axis->t0 + fraction * (axis->t1 - axis->t0);
    const double span = std::clamp(
        (axis->t1 - axis->t0) / factor,
        std::min(axis->duration.toSeconds(),
                 static_cast<double>(axis->frameRate.denominator()) / axis->frameRate.numerator()),
        axis->duration.toSeconds());
    zoomToRange(anchor - fraction * span, anchor + (1.0 - fraction) * span);
}

bool TimelineRuler::handleWheel(QWheelEvent* event) {
    const auto axis = axisForWidth(width());
    if (!axis.has_value()) {
        return false;
    }
    const bool horizontal = event->angleDelta().x() != 0 || event->pixelDelta().x() != 0;
    const bool zoom = event->modifiers().testFlag(Qt::ControlModifier);
    if (!zoom && !horizontal && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        return false;
    }
    const int angle = horizontal ? event->angleDelta().x() : event->angleDelta().y();
    const int pixels = horizontal ? event->pixelDelta().x() : event->pixelDelta().y();
    if (zoom) {
        const double steps = pixels != 0 ? pixels / 120.0 : angle / 120.0;
        zoomBy(std::pow(1.25, steps), event->position().x());
    } else {
        const double delta = pixels != 0 ? -pixels / static_cast<double>(std::max(1, width() - 1))
                                         : -angle / 120.0 * 0.1;
        const double seconds = delta * (axis->t1 - axis->t0);
        zoomToRange(axis->t0 + seconds, axis->t1 + seconds);
    }
    event->accept();
    return true;
}

void TimelineRuler::wheelEvent(QWheelEvent* event) {
    if (!handleWheel(event)) {
        QWidget::wheelEvent(event);
    }
}

void TimelineWorkAreaRow::setRuler(TimelineRuler& ruler) {
    ruler_ = &ruler;
    strip_->setRuler(ruler);
    connect(&ruler, &TimelineRuler::axisChanged, this, [this] { update(); });
}

bool TimelineWorkAreaRow::event(QEvent* event) {
    if (ruler_ != nullptr) {
        if (event->type() == QEvent::Wheel &&
            ruler_->handleWheel(static_cast<QWheelEvent*>(event))) {
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove ||
            event->type() == QEvent::MouseButtonRelease) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const int x = static_cast<int>(mouse->position().x());
            if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
                ruler_->beginScrub(x);
                return true;
            }
            if (event->type() == QEvent::MouseMove && mouse->buttons().testFlag(Qt::LeftButton)) {
                ruler_->updateScrub(x);
                return true;
            }
            if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton) {
                ruler_->endScrub(x);
                return true;
            }
        }
    }
    return QWidget::event(event);
}

TimelineNavigator::TimelineNavigator(TimelineRuler& ruler, QWidget* parent)
    : QWidget(parent), ruler_(ruler) {
    setObjectName("timelineNavigator");
    setAccessibleName(tr("Timeline visible range"));
    setToolTip(tr("Drag the window to scroll; drag either edge to zoom. Escape cancels."));
    setFixedHeight(kit::px(kit::Size::Control));
    setFocusPolicy(Qt::StrongFocus);
    connect(&ruler, &TimelineRuler::axisChanged, this, [this] { updateVisibility(); });
    updateVisibility();
}

void TimelineNavigator::updateVisibility() {
    const auto axis = ruler_.axisForWidth(width());
    const bool zoomed = axis.has_value() &&
                        (axis->t0 > 0.0 || axis->t1 < axis->duration.toSeconds());
    setVisible(zoomed);
    update();
}

bool TimelineNavigator::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride && drag_ != Drag::None &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

QRectF TimelineNavigator::windowRect() const {
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value()) {
        return {};
    }
    const double scale = std::max(1, width() - 1) / axis->duration.toSeconds();
    const int thumb = kit::px(kit::Size::TimelineNavigatorThumb);
    return {axis->t0 * scale, static_cast<qreal>(height() - thumb) / 2.0,
            (axis->t1 - axis->t0) * scale, static_cast<qreal>(thumb)};
}

void TimelineNavigator::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kit::color(kit::Color::SurfaceSunken));
    const auto window = windowRect();
    if (window.isEmpty()) {
        return;
    }
    painter.fillRect(window, kit::color(kit::Color::Muted));
}

void TimelineNavigator::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value()) {
        return;
    }
    setFocus(Qt::MouseFocusReason);
    start_ = axis->t0;
    end_ = axis->t1;
    pressX_ = event->position().x();
    const auto window = windowRect();
    const qreal tolerance = kit::px(kit::Spacing::S);
    drag_ = std::abs(pressX_ - window.left()) <= tolerance    ? Drag::Start
            : std::abs(pressX_ - window.right()) <= tolerance ? Drag::End
                                                              : Drag::Pan;
    if (!window.contains(event->position()) && drag_ == Drag::Pan) {
        pressX_ = window.center().x();
        mouseMoveEvent(event);
    }
    event->accept();
}

void TimelineNavigator::mouseMoveEvent(QMouseEvent* event) {
    if (drag_ == Drag::None) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value()) {
        return;
    }
    const double delta =
        (event->position().x() - pressX_) / std::max(1, width() - 1) * axis->duration.toSeconds();
    const double minimum =
        std::min(axis->duration.toSeconds(),
                 static_cast<double>(axis->frameRate.denominator()) / axis->frameRate.numerator());
    if (drag_ == Drag::Start) {
        ruler_.zoomToRange(std::clamp(start_ + delta, 0.0, end_ - minimum), end_);
    } else if (drag_ == Drag::End) {
        ruler_.zoomToRange(start_,
                           std::clamp(end_ + delta, start_ + minimum, axis->duration.toSeconds()));
    } else {
        ruler_.zoomToRange(start_ + delta, end_ + delta);
    }
    event->accept();
}

void TimelineNavigator::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || drag_ == Drag::None) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    mouseMoveEvent(event);
    drag_ = Drag::None;
    event->accept();
}

void TimelineNavigator::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && drag_ != Drag::None) {
        ruler_.zoomToRange(start_, end_);
        drag_ = Drag::None;
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace bloom::ui
