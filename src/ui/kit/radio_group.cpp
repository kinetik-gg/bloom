#include <bloom/ui/kit/radio_group.hpp>

#include <bloom/ui/kit/accessibility.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace bloom::ui::kit {
namespace {

// A discrete row's radio mark.
constexpr int kMarkDiameter = px(Size::RadioMark);

} // namespace

KRadioGroup::KRadioGroup(QWidget* parent) : QWidget(parent) {
    installKitAccessibility();
    ensureKeyboardFocusTracking(*this);
    setObjectName(QStringLiteral("kRadioGroup"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
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
    // Disabling the current option keeps it current: it is still the value the control reflects,
    // so it stays marked -- faded -- rather than the group silently falling into the mixed state.
    options_[index].enabled = enabled;
    update();
}

bool KRadioGroup::isOptionEnabled(const int index) const {
    return index >= 0 && index < count() && options_.at(index).enabled;
}

int KRadioGroup::currentIndex() const noexcept { return currentIndex_; }

void KRadioGroup::setCurrentIndex(const int index) { commitIndex(index); }

void KRadioGroup::setMixed() {
    if (currentIndex_ == -1)
        return;
    currentIndex_ = -1;
    update();
}

void KRadioGroup::commitIndex(const int index) {
    // A disabled option is refused on every path, click and programmatic alike.
    if (index < 0 || index >= count() || !options_.at(index).enabled || currentIndex_ == index) {
        return;
    }
    currentIndex_ = index;
    update();
    Q_EMIT currentIndexChanged(index);
}

void KRadioGroup::setIconsOnly(const bool enabled) {
    iconsOnly_ = enabled;
    updateGeometry();
    update();
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
    if (iconsOnly_)
        return {px(Size::Control) * count(), px(Size::Control)};
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

QSize KRadioGroup::minimumSizeHint() const {
    // Labels elide, so the floor is an ellipsis and a letter per option (or the icon alone), not
    // the widest label: a narrow panel squeezes the choice instead of overflowing.
    const QFontMetrics metrics(font());
    const int ellipsis = metrics.horizontalAdvance(QStringLiteral("A…"));
    if (iconsOnly_)
        return sizeHint();
    if (presentation_ == Presentation::Segmented)
        return {(ellipsis + px(Spacing::S) * 2) * std::max(1, count()), optionExtent()};
    return {kMarkDiameter + px(Spacing::S) + px(Spacing::XS) * 2 + ellipsis,
            optionExtent() * std::max(1, count())};
}

Color KRadioGroup::borderToken() const {
    return borderForInteraction(isEnabled(), hasKeyboardFocus(*this), hovered_);
}

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
        if (iconsOnly_)
            setToolTip(hovered >= 0 ? optionText(hovered) : QString{});
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void KRadioGroup::enterEvent(QEnterEvent* event) {
    hovered_ = isEnabled();
    update();
    QWidget::enterEvent(event);
}

void KRadioGroup::leaveEvent(QEvent* event) {
    hovered_ = false;
    hoveredIndex_ = -1;
    update();
    QWidget::leaveEvent(event);
}

void KRadioGroup::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hoveredIndex_ = -1;
        hovered_ = false;
    }
    QWidget::changeEvent(event);
}

void KRadioGroup::selectNeighbour(const int direction) {
    // Arrow keys skip disabled options rather than parking on them, so keyboard navigation can
    // never land somewhere it cannot commit. From the mixed state (no current option) Right/Down
    // enter at the first option and Left/Up at the last.
    const int origin = currentIndex_ >= 0 ? currentIndex_ : (direction > 0 ? -1 : count());
    for (int step = 1; step <= count(); ++step) {
        const int candidate = origin + direction * step;
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
    // The bar is one control body wearing the one control look (Secondary): a SurfaceRaised plate
    // and the single border by the kit-wide rule. Each segment takes the same recipe for its own
    // state -- the hover rung, the Accent plate when chosen -- so a segment reads exactly like the
    // button it is.
    const QRectF bar = controlBounds(*this);
    const ButtonColors resting =
        buttonColors(ButtonKind::Secondary, isEnabled() ? State::Normal : State::Disabled, false);
    fillRoundedSurface(painter, bar, resting.fill, color(borderToken()), Radius::Small);
    // A segment sits one hairline inside the bar, so its corner is the bar's minus that inset:
    // concentric, never a 3px corner pressed into a 3px frame.
    const auto inset = static_cast<qreal>(px(Spacing::ControlInset));
    const qreal segmentCorner = radiusPx(Radius::Small, 0) - inset;

    const QFontMetrics metrics(font());
    for (int index = 0; index < count(); ++index) {
        const State state = stateForOption(index);
        const QRectF segment = QRectF(optionRect(index)).intersected(bar);
        const QRectF inner = segment.adjusted(inset, inset, -inset, -inset);
        const bool fadedCurrent = index == currentIndex_ && state == State::Disabled;
        const ButtonColors colors =
            buttonColors(ButtonKind::Secondary, fadedCurrent ? State::Selected : state, false);
        if (state == State::Selected || state == State::Hover) {
            fillRoundedSurface(painter, inner, colors.fill, {}, segmentCorner);
        } else if (fadedCurrent) {
            fillRoundedSurface(painter, inner, disabledInk(colors.fill), {}, segmentCorner);
        }

        const QColor ink = fadedCurrent ? disabledInk(colors.ink) : colors.ink;
        QRectF content = segment.adjusted(px(Spacing::S), 0.0, -px(Spacing::S), 0.0);
        if (const std::optional<IconId> glyph = options_.at(index).icon; glyph.has_value()) {
            const auto size = iconsOnly_ ? iconSize(IconRole::Chrome) : Size::IconSmall;
            const auto box = static_cast<qreal>(px(size));
            const QRectF iconRect(iconsOnly_ ? segment.center().x() - box / 2.0 : content.left(),
                                  content.center().y() - box / 2.0, box, box);
            painter.drawPixmap(iconRect.toRect(),
                               iconPixmap(glyph.value(), size, ink, devicePixelRatioF(),
                                          iconWeight(IconRole::Chrome)));
            content.setLeft(iconRect.right() + px(Spacing::XS));
        }
        if (iconsOnly_)
            continue;
        painter.setPen(ink);
        painter.drawText(content, Qt::AlignCenter,
                         metrics.elidedText(options_.at(index).text, Qt::ElideRight,
                                            static_cast<int>(content.width())));
    }
}

void KRadioGroup::paintDiscrete(QPainter& painter) {
    const QFontMetrics metrics(font());
    const bool focused = hasKeyboardFocus(*this);
    for (int index = 0; index < count(); ++index) {
        const State state = stateForOption(index);
        const QRectF row(optionRect(index));
        if (state == State::Hover) {
            // A wash rather than a ladder rung: the group cannot know which surface it sits on, so
            // it lightens whatever is behind it by the hover amount instead of assuming one.
            fillRoundedSurface(painter, row,
                               withOpacity(color(Color::Foreground), kHoverWashOpacity), {},
                               Radius::Small);
        }

        const auto mark = static_cast<qreal>(kMarkDiameter);
        const QRectF markRect(row.left() + px(Spacing::XS),
                              std::round(row.center().y() - mark / 2.0), mark, mark);
        // The current mark carries keyboard focus: its own border turns Accent (the dot alone
        // already marks selection in Accent), and with no current option the first mark does.
        const bool focusMark =
            focused && (index == currentIndex_ || (currentIndex_ < 0 && index == 0));
        const QColor markBorder = state == State::Selected || focusMark
                                      ? color(Color::Accent)
                                      : color(borderForState(state));
        fillRoundedSurface(painter, markRect, color(Color::Field), markBorder, Radius::Full);
        if (focusMark && state == State::Selected) {
            // Selected already rings the mark in Accent, so focus adds a second hairline ring
            // just outside it.
            const qreal ring = kHairlineWidth;
            fillRoundedSurface(painter, markRect.adjusted(-ring, -ring, ring, ring), {},
                               color(Color::Accent), Radius::Full);
        }
        if (state == State::Selected || (index == currentIndex_ && state == State::Disabled)) {
            const auto dot = static_cast<qreal>(px(Size::RadioDot));
            fillRoundedSurface(painter,
                               QRectF(markRect.left() + (mark - dot) / 2.0,
                                      markRect.top() + (mark - dot) / 2.0, dot, dot),
                               state == State::Selected ? color(Color::Accent)
                                                        : disabledInk(color(Color::Accent)),
                               {}, Radius::Full);
        }

        const QColor ink =
            inkForState(state == State::Selected ? Color::Foreground : Color::Muted, state);
        painter.setPen(ink);
        const QRectF label = row.adjusted(markRect.width() + px(Spacing::S) + px(Spacing::XS), 0.0,
                                          -px(Spacing::XS), 0.0);
        painter.drawText(label, Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(options_.at(index).text, Qt::ElideRight,
                                            static_cast<int>(label.width())));
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
