#include <bloom/ui/kit/color_sampler.hpp>

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QRect>
#include <QSize>
#include <QWidget>

namespace bloom::ui::kit {

std::optional<KColor> sampleImagePixel(const QImage& image, const QPoint& point) {
    if (image.isNull() || !image.rect().contains(point)) {
        return std::nullopt;
    }
    return KColor::fromQColor(image.pixelColor(point));
}

std::optional<KColor> sampleWidgetPixel(QWidget& widget, const QPoint& localPoint) {
    if (!widget.rect().contains(localPoint)) {
        return std::nullopt;
    }
    const QPixmap grabbed = widget.grab(QRect(localPoint, QSize(1, 1)));
    if (grabbed.isNull()) {
        return std::nullopt;
    }
    return sampleImagePixel(grabbed.toImage(), QPoint(0, 0));
}

KColorSampler::KColorSampler(QObject* parent) : QObject(parent) {}

KColorSampler::~KColorSampler() { cancel(); }

void KColorSampler::begin() {
    if (active_) {
        return;
    }
    active_ = true;
    qApp->installEventFilter(this);
    QGuiApplication::setOverrideCursor(QCursor(Qt::CrossCursor));
}

void KColorSampler::end() {
    if (!active_) {
        return;
    }
    active_ = false;
    qApp->removeEventFilter(this);
    QGuiApplication::restoreOverrideCursor();
}

void KColorSampler::cancel() {
    if (!active_) {
        return;
    }
    end();
    Q_EMIT sampleCancelled();
}

bool KColorSampler::isActive() const noexcept { return active_; }

std::optional<KColor> KColorSampler::sampleAt(const QPoint& globalPos) const {
    // QApplication::widgetAt() only ever resolves to a widget this process created: that is the
    // whole of the "app windows only" scope from decision 5, enforced by what Qt itself can see
    // rather than by a manual bounds check that something could bypass.
    QWidget* target = QApplication::widgetAt(globalPos);
    if (target == nullptr) {
        return std::nullopt;
    }
    for (QWidget* owner = target; owner != nullptr; owner = owner->parentWidget()) {
        if (auto* source = dynamic_cast<KColorSampleSource*>(owner); source != nullptr) {
            if (auto value = source->sampleColorAt(owner->mapFromGlobal(globalPos)))
                return value;
            break;
        }
        if (owner->isWindow())
            break;
    }
    return sampleWidgetPixel(*target, target->mapFromGlobal(globalPos));
}

std::optional<KColor> KColorSampler::renderedSampleAt(const QPoint& globalPos) const {
    QWidget* target = QApplication::widgetAt(globalPos);
    if (target == nullptr) {
        return std::nullopt;
    }
    return sampleWidgetPixel(*target, target->mapFromGlobal(globalPos));
}

bool KColorSampler::eventFilter(QObject* watched, QEvent* event) {
    if (!active_) {
        return QObject::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::MouseMove: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (const auto sampled = sampleAt(mouse->globalPosition().toPoint()); sampled.has_value()) {
            Q_EMIT colorHovered(*sampled);
        }
        break;
    }
    case QEvent::MouseButtonPress: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::RightButton) {
            cancel();
            return true;
        }
        if (mouse->button() == Qt::LeftButton) {
            const auto sampled = sampleAt(mouse->globalPosition().toPoint());
            end();
            if (sampled.has_value()) {
                Q_EMIT colorPicked(*sampled);
            } else {
                Q_EMIT sampleCancelled();
            }
            return true;
        }
        break;
    }
    case QEvent::KeyPress: {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            cancel();
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace bloom::ui::kit
