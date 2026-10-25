#include <bloom/ui/kit/progress_bar.hpp>

#include <bloom/ui/kit/painting.hpp>

#include <QPainter>

#include <algorithm>

namespace bloom::ui::kit {
namespace {

// One sweep across the track and back; slow enough to read as "working", never as an alarm.
constexpr int kSweepMs = 1600;
// The indeterminate segment's share of the track.
constexpr double kSweepShare = 1.0 / 3.0;

} // namespace

KProgressBar::KProgressBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kProgressBar"));
    setFocusPolicy(Qt::NoFocus);
    setFixedHeight(px(Size::ProgressTrack));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    animation_.setStartValue(0.0);
    animation_.setKeyValueAt(0.5, 1.0);
    animation_.setEndValue(0.0);
    animation_.setDuration(kSweepMs);
    animation_.setEasingCurve(QEasingCurve::InOutSine);
    animation_.setLoopCount(-1);
    connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        sweep_ = value.toDouble();
        update();
    });
}

void KProgressBar::setProgress(const std::uint64_t completed, const std::uint64_t total) {
    const auto clamped = total == 0 ? completed : std::min(completed, total);
    if (clamped == completed_ && total == total_)
        return;
    completed_ = clamped;
    total_ = total;
    setAccessibleDescription(total_ == 0 ? tr("Busy") : tr("%1 of %2").arg(completed_).arg(total_));
    updateSweep();
    update();
}

double KProgressBar::fraction() const noexcept {
    if (total_ == 0)
        return 0.0;
    return static_cast<double>(completed_) / static_cast<double>(total_);
}

bool KProgressBar::sweeping() const noexcept {
    return animation_.state() == QAbstractAnimation::Running;
}

QRectF KProgressBar::trackRect() const { return QRectF(rect()); }

QRectF KProgressBar::fillRect() const {
    const QRectF track = trackRect();
    if (!indeterminate())
        return {track.left(), track.top(), track.width() * fraction(), track.height()};
    const double width = track.width() * kSweepShare;
    return {track.left() + (track.width() - width) * sweep_, track.top(), width, track.height()};
}

QSize KProgressBar::sizeHint() const {
    return {px(Size::DialogTextWidth), px(Size::ProgressTrack)};
}

QSize KProgressBar::minimumSizeHint() const {
    return {px(Size::ControlRoomy) * 2, px(Size::ProgressTrack)};
}

void KProgressBar::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    updateSweep();
}

void KProgressBar::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    updateSweep();
}

void KProgressBar::updateSweep() {
    const bool sweeping = indeterminate() && isVisible() && motionEnabled();
    if (sweeping && animation_.state() != QAbstractAnimation::Running) {
        animation_.start();
    } else if (!sweeping && animation_.state() == QAbstractAnimation::Running) {
        animation_.stop();
        sweep_ = 0.5;
    }
}

void KProgressBar::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    fillRoundedSurface(painter, trackRect(), color(Color::Field), {}, Radius::Full);
    QColor accent = color(Color::Accent);
    if (!isEnabled())
        accent = withOpacity(accent, kDisabledOpacity);
    const QRectF filled = fillRect();
    if (filled.width() > 0.0)
        fillRoundedSurface(painter, filled, accent, {}, Radius::Full);
}

} // namespace bloom::ui::kit
