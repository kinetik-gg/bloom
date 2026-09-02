#include <bloom/ui/kit/radio_group.hpp>

#include <bloom/ui/kit/painting.hpp>

#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace bloom::ui::kit {
namespace {

// A discrete row's radio mark, and the gap between it and its label.
constexpr int kMarkDiameter = 14;

} // namespace

KRadioGroup::KRadioGroup(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kRadioGroup"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setFont(kit::font(TypeRole::Ui));
}

int KRadioGroup::addOption(const QString& text, std::optional<IconId> icon) {
    options_.append(Option{text, icon, true});
    const int index = static_cast<int>(options_.size()) - 1;
    if (currentIndex_ < 0) {
        commitIndex(index);
    }
    updateGeometry();
    update();
    return index;
}

int KRadioGroup::count() const { return static_cast<int>(options_.size()); }

QString KRadioGroup::optionText(const int index) const {
    return index >= 0 && index < count() ? options_.at(index).text : QString{};
}

void KRadioGroup::setOptionEnabled(const int index, const bool enabled) {
    if (index < 0 || index >= count()) {
        return;
    }
    options_[index].enabled = enabled;
    if (!enabled && currentIndex_ == index) {
        currentIndex_ = -1;
    }
    update();
}

bool KRadioGroup::isOptionEnabled(const int index) const {
    return index >= 0 && index < count() && options_.at(index).enabled;
}

int KRadioGroup::currentIndex() const noexcept { return currentIndex_; }

void KRadioGroup::setCurrentIndex(const int index) { commitIndex(index); }

void KRadioGroup::commitIndex(const int index) {
    // A disabled option is refused on every path, click and programmatic alike.
    if (index < 0 || index >= count() || !options_.at(index).enabled || currentIndex_ == index) {
        return;
    }
    currentIndex_ = index;
    update();
    Q_EMIT currentIndexChanged(index);
}

void KRadioGroup::setPresentation(const Presentation presentation) {
    if (presentation_ == presentation) {
        return;
    }
    presentation_ = presentation;
    updateGeometry();
    update();
}

KRadioGroup::Presentation KRadioGroup::presentation() const noexcept { return presentation_; }

int KRadioGroup::optionExtent() const { return px(Size::Control); }

QRect KRadioGroup::optionRect(const int index) const {
    if (index < 0 || index >= count()) {
        return {};
    }
    if (presentation_ == Presentation::Segmented) {
        const int segment = width() / std::max(1, count());
        const int left = segment * index;
        // The last segment absorbs the integer-division remainder, so the bar always ends flush
        // with the control rather than a pixel or two short of it.
        const int segmentWidth = index == count() - 1 ? width() - left : segment;
        return {left, 0, segmentWidth, height()};
    }
    return {0, optionExtent() * index, width(), optionExtent()};
}

State KRadioGroup::stateForOption(const int index) const {
    if (index < 0 || index >= count()) {
        return State::Normal;
    }
    if (!isEnabled() || !options_.at(index).enabled) {
        return State::Disabled;
    }
    if (index == currentIndex_) {
        return State::Selected;
    }
    if (index == hoveredIndex_) {
        return State::Hover;
    }
    return State::Normal;
}

int KRadioGroup::optionAt(const QPoint& point) const {
    for (int index = 0; index < count(); ++index) {
        if (optionRect(index).contains(point)) {
            return index;
        }
    }
    return -1;
}

QSize KRadioGroup::sizeHint() const {
    const QFontMetrics metrics(font());
    if (presentation_ == Presentation::Segmented) {
        int widest = 0;
        for (const auto& option : options_) {
            int optionWidth = metrics.horizontalAdvance(option.text) + px(Spacing::M) * 2;
            if (option.icon.has_value()) {
                optionWidth += px(Size::IconSmall) + px(Spacing::XS);
            }
            widest = std::max(widest, optionWidth);
        }
        return {widest * std::max(1, count()), optionExtent()};
    }
    int widest = 0;
    for (const auto& option : options_) {
        widest = std::max(widest, metrics.horizontalAdvance(option.text));
    }
    return {widest + kMarkDiameter + px(Spacing::S) + px(Spacing::XS) * 2,
            optionExtent() * std::max(1, count())};
}

QSize KRadioGroup::minimumSizeHint() const { return sizeHint(); }

void KRadioGroup::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isEnabled()) {
        commitIndex(optionAt(event->pos()));
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void KRadioGroup::mouseMoveEvent(QMouseEvent* event) {
    const int hovered = isEnabled() ? optionAt(event->pos()) : -1;
    if (hovered != hoveredIndex_) {
        hoveredIndex_ = hovered;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void KRadioGroup::leaveEvent(QEvent* event) {
    if (hoveredIndex_ != -1) {
        hoveredIndex_ = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

void KRadioGroup::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hoveredIndex_ = -1;
    }
    QWidget::changeEvent(event);
}

void KRadioGroup::selectNeighbour(const int direction) {
    // Arrow keys skip disabled options rather than parking on them, so keyboard navigation can
    // never land somewhere it cannot commit.
    for (int step = 1; step <= count(); ++step) {
        const int candidate = currentIndex_ + direction * step;
        if (candidate < 0 || candidate >= count()) {
            return;
        }
        if (options_.at(candidate).enabled) {
            commitIndex(candidate);
            return;
        }
    }
}

void KRadioGroup::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Up:
        selectNeighbour(-1);
        event->accept();
        return;
    case Qt::Key_Right:
    case Qt::Key_Down:
        selectNeighbour(1);
        event->accept();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void KRadioGroup::paintSegmented(QPainter& painter) {
    const QRectF bar(rect());
    fillRoundedSurface(painter, bar, color(Color::Surface), color(Color::Border), Radius::Small);

    const QFontMetrics metrics(font());
    for (int index = 0; index < count(); ++index) {
        const State state = stateForOption(index);
        const QRectF segment(optionRect(index));
        if (state == State::Selected) {
            fillRoundedSurface(painter, segment.adjusted(1.0, 1.0, -1.0, -1.0),
                               color(Color::Accent), {}, Radius::Small);
        } else if (state == State::Hover) {
            fillRoundedSurface(painter, segment.adjusted(1.0, 1.0, -1.0, -1.0),
                               color(surfaceStep(Color::Surface, 1)), {}, Radius::Small);
        }

        const QColor ink =
            state == State::Selected ? color(Color::Foreground) : inkForState(Color::Muted, state);
        QRectF content = segment.adjusted(px(Spacing::S), 0.0, -px(Spacing::S), 0.0);
        if (const std::optional<IconId> glyph = options_.at(index).icon; glyph.has_value()) {
            const auto box = static_cast<qreal>(px(Size::IconSmall));
            const QRectF iconRect(content.left(), content.center().y() - box / 2.0, box, box);
            painter.drawPixmap(iconRect.toRect(), iconPixmap(glyph.value(), Size::IconSmall, ink,
                                                             devicePixelRatioF()));
            content.setLeft(iconRect.right() + px(Spacing::XS));
        }
        painter.setPen(ink);
        painter.drawText(content, Qt::AlignCenter,
                         metrics.elidedText(options_.at(index).text, Qt::ElideRight,
                                            static_cast<int>(content.width())));
    }
}

void KRadioGroup::paintDiscrete(QPainter& painter) {
    for (int index = 0; index < count(); ++index) {
        const State state = stateForOption(index);
        const QRectF row(optionRect(index));
        if (state == State::Hover) {
            fillRoundedSurface(painter, row, color(surfaceStep(Color::Background, 1)), {},
                               Radius::Small);
        }

        const auto mark = static_cast<qreal>(kMarkDiameter);
        const QRectF markRect(row.left() + px(Spacing::XS), row.center().y() - mark / 2.0, mark,
                              mark);
        const QColor markBorder =
            state == State::Selected ? color(Color::Accent) : color(borderForState(state));
        fillRoundedSurface(painter, markRect, color(Color::Field), markBorder, Radius::Full);
        if (state == State::Selected) {
            const qreal dot = mark / 2.5;
            fillRoundedSurface(painter,
                               QRectF(markRect.center().x() - dot / 2.0,
                                      markRect.center().y() - dot / 2.0, dot, dot),
                               color(Color::Accent), {}, Radius::Full);
        }

        const QColor ink =
            inkForState(state == State::Selected ? Color::Foreground : Color::Muted, state);
        painter.setPen(ink);
        painter.drawText(row.adjusted(markRect.width() + px(Spacing::S) + px(Spacing::XS), 0.0,
                                      -px(Spacing::XS), 0.0),
                         Qt::AlignVCenter | Qt::AlignLeft, options_.at(index).text);
    }
}

void KRadioGroup::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setFont(font());
    if (presentation_ == Presentation::Segmented) {
        paintSegmented(painter);
        return;
    }
    paintDiscrete(painter);
}

} // namespace bloom::ui::kit
