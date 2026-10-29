#include <bloom/ui/kit/color_swatches.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>

#include <bloom/ui/kit/painting.hpp>

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <chrono>

namespace bloom::ui::kit {
namespace {

using namespace std::chrono_literals;

// The press-and-hold duration that turns a click into a "set this slot" gesture. Deliberately not
// drawn from kit::Motion: that vocabulary times animated transitions and always pairs a duration
// with an easing curve (tokens.hpp's Fast/Pop/None), where this is a plain gesture-recognition
// threshold with no curve of its own -- a different kind of "duration" than the token exists for.
// 500ms is the conventional press-and-hold length (matching the platform long-press interval most
// touch and pointer UIs converge on): long enough that an ordinary click never fires it, short
// enough that the artist does not have to wait.
constexpr int kLongPressMs = static_cast<int>(std::chrono::milliseconds(500).count());

[[nodiscard]] int cellExtent() { return px(Size::IconLarge); }
[[nodiscard]] int cellGap() { return px(Spacing::XXS); }
// A larger cell than the token-minimum spacing step: at these control sizes (a ~26px chip,
// a 22px-tall alpha bar) a 2px checker reads as noise once anti-aliased, and pixel-sampling
// it in a test would be sampling blend artifacts rather than the pattern itself.
[[nodiscard]] int checkerCellPx() { return px(Size::CheckerCell); }

} // namespace

KRecentColorStore::KRecentColorStore(const int capacity) : capacity_(std::max(1, capacity)) {
    colors_.reserve(static_cast<std::size_t>(capacity_));
}

void KRecentColorStore::push(const KColor& value) {
    const auto existing = std::ranges::find(colors_, value);
    if (existing != colors_.end()) {
        colors_.erase(existing);
    }
    colors_.insert(colors_.begin(), value);
    if (static_cast<int>(colors_.size()) > capacity_) {
        colors_.resize(static_cast<std::size_t>(capacity_));
    }
}

void KRecentColorStore::clear() { colors_.clear(); }

int KRecentColorStore::capacity() const noexcept { return capacity_; }

void KRecentColorStore::setCapacity(const int capacity) {
    capacity_ = std::max(1, capacity);
    if (static_cast<int>(colors_.size()) > capacity_) {
        colors_.resize(static_cast<std::size_t>(capacity_));
    }
}

const std::vector<KColor>& KRecentColorStore::colors() const noexcept { return colors_; }

KColorSwatchRow::KColorSwatchRow(QWidget* parent) : QWidget(parent) {
    ensureKeyboardFocusTracking(*this);
    setObjectName(QStringLiteral("kColorSwatchRow"));
    // Tab reaches the row; the arrow keys walk its slots and Space/Enter applies one. A click
    // applies directly and does not take focus.
    setFocusPolicy(Qt::TabFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    pinned_.resize(static_cast<std::size_t>(slotCount_));
}

void KColorSwatchRow::setSlotCount(const int count) {
    const int clamped = std::clamp(count, 1, kMaxSlotCount);
    if (clamped == slotCount_) {
        return;
    }
    slotCount_ = clamped;
    pinned_.resize(static_cast<std::size_t>(slotCount_));
    focusedSlot_ = std::min(focusedSlot_, slotCount_ - 1);
    updateGeometry();
    update();
}

int KColorSwatchRow::slotCount() const noexcept { return slotCount_; }

void KColorSwatchRow::setRecentColorStore(KRecentColorStore* store) {
    store_ = store;
    update();
}

KRecentColorStore* KColorSwatchRow::recentColorStore() const noexcept { return store_; }

void KColorSwatchRow::setCurrentColor(const KColor& color) { currentColor_ = color; }

KColor KColorSwatchRow::currentColor() const noexcept { return currentColor_; }

void KColorSwatchRow::setSlotColor(const int index, const KColor& color) {
    if (index < 0 || index >= slotCount_) {
        return;
    }
    pinned_[static_cast<std::size_t>(index)] = color;
    update();
}

void KColorSwatchRow::clearSlot(const int index) {
    if (index < 0 || index >= slotCount_) {
        return;
    }
    pinned_[static_cast<std::size_t>(index)].reset();
    update();
}

std::optional<KColor> KColorSwatchRow::slotColor(const int index) const {
    if (index < 0 || index >= slotCount_) {
        return std::nullopt;
    }
    const auto& pinned = pinned_[static_cast<std::size_t>(index)];
    if (pinned.has_value()) {
        return pinned;
    }
    if (store_ != nullptr && index < static_cast<int>(store_->colors().size())) {
        return store_->colors()[static_cast<std::size_t>(index)];
    }
    return std::nullopt;
}

bool KColorSwatchRow::isSlotPinned(const int index) const {
    return index >= 0 && index < slotCount_ && pinned_[static_cast<std::size_t>(index)].has_value();
}

QRectF KColorSwatchRow::slotRect(const int index) const {
    if (index < 0 || index >= slotCount_) {
        return {};
    }
    const auto extent = static_cast<qreal>(cellExtent());
    const auto gap = static_cast<qreal>(cellGap());
    const qreal left = static_cast<qreal>(index) * (extent + gap);
    return {left, 0.0, extent, extent};
}

int KColorSwatchRow::slotAt(const QPointF& point) const {
    for (int index = 0; index < slotCount_; ++index) {
        if (slotRect(index).contains(point)) {
            return index;
        }
    }
    return -1;
}

QSize KColorSwatchRow::sizeHint() const {
    const int extent = cellExtent();
    const int gap = cellGap();
    const int width = slotCount_ * extent + std::max(0, slotCount_ - 1) * gap;
    return {width, extent};
}

QSize KColorSwatchRow::minimumSizeHint() const { return sizeHint(); }

void KColorSwatchRow::pinSlot(const int index) {
    if (index < 0 || index >= slotCount_) {
        return;
    }
    setSlotColor(index, currentColor_);
    Q_EMIT slotPinned(index, currentColor_);
}

void KColorSwatchRow::cancelLongPress() {
    if (longPressTimerId_ != 0) {
        killTimer(longPressTimerId_);
        longPressTimerId_ = 0;
    }
}

void KColorSwatchRow::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !isEnabled()) {
        QWidget::mousePressEvent(event);
        return;
    }
    const int slot = slotAt(event->position());
    if (slot < 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    pressedSlot_ = slot;
    cancelLongPress();
    longPressTimerId_ = startTimer(kLongPressMs);
    event->accept();
}

void KColorSwatchRow::mouseMoveEvent(QMouseEvent* event) {
    const int slot = slotAt(event->position());
    if (slot != hoveredSlot_) {
        hoveredSlot_ = slot;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void KColorSwatchRow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && pressedSlot_ >= 0) {
        // The long-press timer already fired and pinned the slot; a release afterward is just the
        // finger lifting, not a second gesture.
        const bool wasLongPress = longPressTimerId_ == 0;
        cancelLongPress();
        if (!wasLongPress) {
            const auto resolved = slotColor(pressedSlot_);
            if (resolved.has_value()) {
                Q_EMIT colorActivated(*resolved);
            }
        }
        pressedSlot_ = -1;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

int KColorSwatchRow::focusedSlot() const noexcept { return focusedSlot_; }

void KColorSwatchRow::keyPressEvent(QKeyEvent* event) {
    int next = focusedSlot_;
    switch (event->key()) {
    case Qt::Key_Left:
        next = std::max(0, focusedSlot_ - 1);
        break;
    case Qt::Key_Right:
        next = std::min(slotCount_ - 1, focusedSlot_ + 1);
        break;
    case Qt::Key_Home:
        next = 0;
        break;
    case Qt::Key_End:
        next = slotCount_ - 1;
        break;
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (const auto resolved = slotColor(focusedSlot_); resolved.has_value())
            Q_EMIT colorActivated(*resolved);
        event->accept();
        return;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    focusedSlot_ = next;
    update();
    event->accept();
}

void KColorSwatchRow::contextMenuEvent(QContextMenuEvent* event) {
    const int slot = slotAt(event->pos());
    if (slot >= 0 && isEnabled()) {
        pinSlot(slot);
        event->accept();
        return;
    }
    QWidget::contextMenuEvent(event);
}

void KColorSwatchRow::leaveEvent(QEvent* event) {
    hoveredSlot_ = -1;
    update();
    QWidget::leaveEvent(event);
}

void KColorSwatchRow::timerEvent(QTimerEvent* event) {
    if (event->timerId() == longPressTimerId_ && pressedSlot_ >= 0) {
        killTimer(longPressTimerId_);
        longPressTimerId_ = 0;
        pinSlot(pressedSlot_);
        return;
    }
    QWidget::timerEvent(event);
}

void KColorSwatchRow::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    for (int index = 0; index < slotCount_; ++index) {
        const QRectF rect = slotRect(index);
        const auto resolved = slotColor(index);
        const bool hovered = index == hoveredSlot_ && isEnabled();
        // A slot's border is the state channel, so the keyboard-focused slot is the one whose
        // single border turns Accent.
        const Color borderToken = borderForInteraction(
            isEnabled(), hasKeyboardFocus(*this) && index == focusedSlot_, hovered);

        if (resolved.has_value()) {
            const QColor swatch = resolved->toQColor();
            if (swatch.alpha() < 255) {
                drawAlphaCheckerboard(painter, rect, checkerCellPx(), Radius::Small);
            }
            fillRoundedSurface(painter, rect, swatch, color(borderToken), Radius::Small);
        } else {
            // An empty slot: Field fill (the same rung an input cell rests on), no dashed literal
            // pattern -- the hairline border alone is enough to read as "a slot" without inventing
            // a second visual language for "empty".
            fillRoundedSurface(painter, rect, color(Color::Field), color(borderToken),
                               Radius::Small);
        }

        if (isSlotPinned(index)) {
            // A pinned slot keeps a 2px inset accent edge -- the token vocabulary's own "selected"
            // recipe (visual-language.md: "a 2px inset accent edge where a fill would hide
            // content") -- so "explicitly set" reads as a real state, not a hover accident.
            const auto edge = static_cast<qreal>(px(Size::SelectionEdge));
            QPen pen(color(Color::Accent));
            pen.setWidthF(edge);
            painter.save();
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            const qreal inset = edge;
            painter.drawRoundedRect(
                rect.adjusted(inset, inset, -inset, -inset),
                static_cast<qreal>(radiusPx(Radius::Small, static_cast<int>(rect.width()))),
                static_cast<qreal>(radiusPx(Radius::Small, static_cast<int>(rect.width()))));
            painter.restore();
        }
    }
}

} // namespace bloom::ui::kit
