#include <bloom/ui/kit/value_field.hpp>

#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/theme.hpp>

#include <QApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace bloom::ui::kit {
namespace {

// The width the label column claims before the cell begins.
constexpr int kLabelColumnWidth = 72;

// The cell's own horizontal padding: the inset the painted value AND the inline editor both use, so
// the number sits at one x whichever of the two is drawing it.
constexpr int kCellPaddingX = px(Spacing::S);

// QLineEdit lays its text out inside its content rectangle inset by a further fixed two logical
// pixels per side (QLineEditPrivate::horizontalMargin), which Qt exposes through neither a public
// accessor nor a style hint. The cell's padding is therefore handed to the editor as TEXT margins
// reduced by exactly that much, which is what makes the glyphs land on the painter's own x --
// kit_value_field_tests.cpp pins that off a real render rather than off this arithmetic.
constexpr int kLineEditTextMargin = 2;

} // namespace

KValueField::KValueField(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kValueField"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setFont(kit::font(TypeRole::Ui));
    // The whole control is a scrub handle, label included -- exactly as in After Effects, where
    // dragging the parameter's name drags its value.
    setCursor(Qt::SizeHorCursor);

    // The text editor exists for the control's whole life rather than being built on demand: it
    // carries focus while editing, and a widget that is created and destroyed inside a focus
    // transition is a reliable way to lose that focus to the wrong place.
    editor_ = new QLineEdit(this);
    editor_->setObjectName(QStringLiteral("kValueFieldEditor"));
    editor_->setFrame(false);
    editor_->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    editor_->setFont(kit::font(TypeRole::Value));
    editor_->setCursor(Qt::IBeamCursor);
    // The editor contributes NO background of its own: the cell this widget paints is the only
    // fill, and the Accent hairline on that cell is the only border. Base is what the style fills a
    // line edit's panel with; Window is what Qt fills a widget's rectangle with whenever the widget
    // is painted as a root -- which is exactly what happens to it inside a QGraphicsProxyWidget on
    // a node card, and what previously left an opaque plate over the cell the moment editing began.
    QPalette editorPalette = editor_->palette();
    editorPalette.setColor(QPalette::Base, Qt::transparent);
    editorPalette.setColor(QPalette::Window, Qt::transparent);
    editorPalette.setColor(QPalette::Text, color(Color::Foreground));
    editorPalette.setColor(QPalette::Highlight, color(Color::Accent));
    editorPalette.setColor(QPalette::HighlightedText, color(Color::Foreground));
    editor_->setPalette(editorPalette);
    editor_->setAttribute(Qt::WA_NoSystemBackground, true);
    editor_->setAutoFillBackground(false);
    // A palette alone is not enough: the application installs a global stylesheet, and the moment
    // one exists QStyleSheetStyle -- not Fusion -- draws PE_PanelLineEdit, with a panel of its own
    // that ignores this widget's palette and lands an opaque plate over the cell. The one way to
    // reach that painter is a rule, so the editor carries the smallest possible one: no box, no
    // background, the cell's padding expressed as zero (it travels in the text margins instead),
    // and the selection colors the palette above already names.
    editor_->setStyleSheet(expandTokens(QStringLiteral(R"(
QLineEdit#kValueFieldEditor {
    background: transparent;
    border: none;
    padding: 0px;
    margin: 0px;
    color: {color.Foreground};
    selection-background-color: {color.Accent};
    selection-color: {color.Foreground};
}
)")));
    editor_->hide();
    editor_->installEventFilter(this);
}

void KValueField::setLabel(const QString& label) {
    label_ = label;
    update();
}

QString KValueField::label() const { return label_; }

void KValueField::setUnit(const QString& unit) {
    unit_ = unit;
    if (editing_) {
        // The unit's column is reserved inside the editor's own text margins, so changing it while
        // the editor is up has to re-derive them or the number would land on a stale x.
        layOutEditor();
    }
    updateGeometry();
    update();
}

QString KValueField::unit() const { return unit_; }

void KValueField::setRange(const double minimum, const double maximum) {
    minimum_ = minimum;
    maximum_ = std::max(maximum, minimum);
    commitValue(value_);
    update();
}

double KValueField::minimum() const noexcept { return minimum_; }

double KValueField::maximum() const noexcept { return maximum_; }

void KValueField::setSingleStep(const double step) { step_ = std::abs(step); }

double KValueField::singleStep() const noexcept { return step_; }

void KValueField::setDecimals(const int decimals) {
    decimals_ = std::clamp(decimals, 0, 9);
    updateGeometry();
    update();
}

int KValueField::decimals() const noexcept { return decimals_; }

double KValueField::value() const noexcept { return value_; }

void KValueField::setValue(const double value) { commitValue(value); }

void KValueField::commitValue(const double value) {
    const double clamped = std::clamp(value, minimum_, maximum_);
    if (clamped == value_) {
        return;
    }
    value_ = clamped;
    update();
    Q_EMIT valueChanged(value_);
}

void KValueField::stepBy(const int steps) {
    commitValue(value_ + step_ * static_cast<double>(steps));
}

double KValueField::scrubScale(const Qt::KeyboardModifiers modifiers) noexcept {
    // Shift outranks Ctrl when both are held: the coarse gesture is the one the artist asked for
    // last in every host that offers both, and one of the two has to win.
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        return 10.0;
    }
    if (modifiers.testFlag(Qt::ControlModifier)) {
        return 0.1;
    }
    return 1.0;
}

bool KValueField::isScrubbing() const noexcept { return scrubbing_; }

bool KValueField::isEditing() const noexcept { return editing_; }

QLineEdit* KValueField::lineEdit() const noexcept { return editor_; }

QString KValueField::displayedValue() const { return QString::number(value_, 'f', decimals_); }

QRectF KValueField::labelRect() const {
    if (label_.isEmpty()) {
        return {};
    }
    return {0.0, 0.0, static_cast<qreal>(kLabelColumnWidth), static_cast<qreal>(height())};
}

QRectF KValueField::cellRect() const {
    const qreal left = label_.isEmpty() ? 0.0 : kLabelColumnWidth + px(Spacing::S);
    // The whole remaining width and the WHOLE height. This cell reserves no focus-ring strip: its
    // focus affordance is its own single border (kit::borderForInteraction), stroked on the cell's
    // own edge, so a margin outside that edge would be reserved for something nothing draws. An
    // earlier revision reserved kFocusRingWidth top and bottom, and hosted inside a
    // QGraphicsProxyWidget on a node card that unpainted strip read as a darker band above and
    // below every field.
    return {left, 0.0, std::max(0.0, width() - left), static_cast<qreal>(height())};
}

QRectF KValueField::cellTextRect() const {
    return cellRect().adjusted(kCellPaddingX, 0.0, -kCellPaddingX, 0.0);
}

Color KValueField::borderToken() const {
    return borderForInteraction(isEnabled(), editing_ || hasFocus(), hovered_);
}

QColor KValueField::cellBorderColor() const {
    const Color role = borderToken();
    // Borderless at rest: the resting token resolves to nothing drawn at all, so the cell is a
    // plain Field rectangle until hover or focus gives it its one border.
    return role == Color::Border ? QColor(Qt::transparent) : color(role);
}

State KValueField::visualState() const {
    if (!isEnabled()) {
        return State::Disabled;
    }
    if (hovered_) {
        return State::Hover;
    }
    if (editing_ || hasFocus()) {
        return State::Focused;
    }
    return State::Normal;
}

QSize KValueField::sizeHint() const {
    const QFontMetrics valueMetrics(kit::font(TypeRole::Value));
    // Sized for the widest number the range can produce, not for the number currently in it: the
    // field must not resize as digits change.
    const QString widest =
        QString::number(std::max(std::abs(minimum_), std::abs(maximum_)), 'f', decimals_);
    int cellWidth = valueMetrics.horizontalAdvance(widest) + px(Spacing::S) * 2;
    if (!unit_.isEmpty()) {
        cellWidth += valueMetrics.horizontalAdvance(unit_) + px(Spacing::XS);
    }
    const int labelWidth = label_.isEmpty() ? 0 : kLabelColumnWidth + px(Spacing::S);
    // Exactly the control height -- no ring margin, for the same reason cellRect() reserves none.
    return {labelWidth + cellWidth, px(Size::Control)};
}

QSize KValueField::minimumSizeHint() const {
    // task WIDTH-1: sizeHint() above is this field's PREFERRED width, sized for the widest number
    // its own range can produce so the cell never resizes as digits change. minimumSizeHint() is a
    // different question -- the FLOOR this cell may shrink to when the panel hosting it (Properties
    // above all) is narrower than every field's combined preferred width. That floor is
    // Size::ValueCellMin, not the range's own widest string, so a narrow panel degrades every cell
    // in it to the same legible width instead of each row committing to its own.
    const int labelWidth = label_.isEmpty() ? 0 : kLabelColumnWidth + px(Spacing::S);
    return {labelWidth + px(Size::ValueCellMin), px(Size::Control)};
}

void KValueField::layOutEditor() {
    // The editor IS the cell: exactly its rectangle, so the Field fill and the one Accent hairline
    // this widget paints stay the cell's own outline rather than being joined by a second, smaller
    // rectangle with its own background and corners. The cell's padding reaches the editor as text
    // margins instead of as geometry, and the unit's column is reserved on the right because the
    // unit keeps being painted while editing -- together that is what holds the number's x fixed
    // across entering and leaving edit.
    editor_->setGeometry(cellRect().toRect());
    const QFontMetrics valueMetrics(kit::font(TypeRole::Value));
    const int unitColumn =
        unit_.isEmpty() ? 0 : valueMetrics.horizontalAdvance(unit_) + px(Spacing::XS);
    editor_->setTextMargins(kCellPaddingX - kLineEditTextMargin, 0,
                            kCellPaddingX + unitColumn - kLineEditTextMargin, 0);
}

void KValueField::beginEdit() {
    if (editing_ || !isEnabled()) {
        return;
    }
    editing_ = true;
    layOutEditor();
    editor_->setText(displayedValue());
    editor_->show();
    editor_->setFocus(Qt::MouseFocusReason);
    // Select all on entry: a click-to-edit that kept the caret where the pointer landed would make
    // replacing the number a two-gesture job.
    editor_->selectAll();
    update();
}

void KValueField::endEdit(const bool keep) {
    if (!editing_) {
        return;
    }
    // Cleared first so the focus change the hide() below causes cannot re-enter this function.
    editing_ = false;
    const QString typed = editor_->text();
    editor_->hide();
    if (keep) {
        bool parsed = false;
        const double entered = typed.toDouble(&parsed);
        if (parsed) {
            commitValue(entered);
        }
        // Unparseable text is refused the way Esc is: the old value stays, rather than a typo
        // silently becoming zero.
    }
    setFocus(Qt::OtherFocusReason);
    update();
}

void KValueField::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isEnabled()) {
        pressed_ = true;
        scrubbing_ = false;
        pressPosition_ = event->position().toPoint();
        pressValue_ = value_;
        setFocus(Qt::MouseFocusReason);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void KValueField::mouseMoveEvent(QMouseEvent* event) {
    if (!pressed_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const int dx = event->position().toPoint().x() - pressPosition_.x();
    if (!scrubbing_) {
        if (std::abs(dx) < QApplication::startDragDistance()) {
            event->accept();
            return;
        }
        scrubbing_ = true;
    }
    // Measured from the press, not from the previous move: a scrub that wanders back to where it
    // started puts the value back exactly, with no accumulated rounding drift.
    commitValue(pressValue_ + static_cast<double>(dx) * step_ * scrubScale(event->modifiers()));
    event->accept();
}

void KValueField::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !pressed_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool wasScrubbing = scrubbing_;
    pressed_ = false;
    scrubbing_ = false;
    if (!wasScrubbing) {
        // A press that never travelled is a click, and a click opens the editor.
        beginEdit();
    }
    update();
    event->accept();
}

void KValueField::wheelEvent(QWheelEvent* event) {
    if (!hasFocus() || !isEnabled()) {
        // A wheel over an unfocused field belongs to whatever is scrolling behind it. Stealing it
        // would silently change a parameter the artist was only scrolling past.
        QWidget::wheelEvent(event);
        return;
    }
    stepBy(event->angleDelta().y() > 0 ? 1 : -1);
    event->accept();
}

bool KValueField::event(QEvent* event) {
    // A focused value field owns its editing keys the way a spin box does: claim the
    // ShortcutOverride so window-level shortcuts (frame stepping, transport jumps) stay inert
    // while the artist is adjusting a value. Left/Right/Home/End are claimed for parity with the
    // spin-box widgets this field replaces even though only Up/Down/Page currently step.
    if (event->type() == QEvent::ShortcutOverride && hasFocus()) {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Home:
        case Qt::Key_End:
            event->accept();
            return true;
        default:
            break;
        }
    }
    return QWidget::event(event);
}

bool KValueField::eventFilter(QObject* watched, QEvent* event) {
    if (watched != editor_) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            endEdit(false);
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            // Consumed here rather than through QLineEdit::returnPressed, because QLineEdit
            // deliberately leaves Return unaccepted so a dialog's default button can still see it
            // -- which would send it straight on to this field's own Return handler and reopen the
            // editor the commit just closed.
            endEdit(true);
            return true;
        }
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            // Tab commits, then travels on the way Tab always does.
            endEdit(true);
            return false;
        }
    }
    if (event->type() == QEvent::FocusOut) {
        // Clicking away is a commit, not a cancel: the artist typed a value and then went
        // somewhere else, which in every numeric field in this class of application means "take
        // it". Esc is the way to not take it.
        endEdit(true);
    }
    return QWidget::eventFilter(watched, event);
}

void KValueField::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Up:
        stepBy(1);
        event->accept();
        return;
    case Qt::Key_Down:
        stepBy(-1);
        event->accept();
        return;
    case Qt::Key_PageUp:
        stepBy(10);
        event->accept();
        return;
    case Qt::Key_PageDown:
        stepBy(-10);
        event->accept();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        // The keyboard route into the text editor, for an artist who tabbed here rather than
        // clicking.
        beginEdit();
        event->accept();
        return;
    case Qt::Key_Escape:
        if (scrubbing_) {
            // Abandoning a scrub puts back exactly the value the press started from.
            pressed_ = false;
            scrubbing_ = false;
            commitValue(pressValue_);
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void KValueField::enterEvent(QEnterEvent* event) {
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
    QWidget::enterEvent(event);
}

void KValueField::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void KValueField::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hovered_ = false;
        pressed_ = false;
        scrubbing_ = false;
        endEdit(false);
    }
    QWidget::changeEvent(event);
}

void KValueField::resizeEvent(QResizeEvent* event) {
    if (editing_) {
        layOutEditor();
    }
    QWidget::resizeEvent(event);
}

void KValueField::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const State state = visualState();

    if (!label_.isEmpty()) {
        painter.setFont(kit::font(TypeRole::Ui));
        painter.setPen(inkForState(Color::Muted, state));
        const QFontMetrics metrics(painter.font());
        painter.drawText(labelRect(), Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(label_, Qt::ElideRight, kLabelColumnWidth));
    }

    const QRectF cell = cellRect();
    fillRoundedSurface(painter, cell, color(surfaceForState(Color::Field, state)),
                       cellBorderColor(), Radius::Small);

    // The value takes the monospaced role; the unit takes muted ink so it reads as a unit rather
    // than as part of the number.
    painter.setFont(kit::font(TypeRole::Value));
    const QFontMetrics valueMetrics(painter.font());
    const QRectF text = cellTextRect();
    QRectF unitRect;
    if (!unit_.isEmpty()) {
        const auto unitWidth = static_cast<qreal>(valueMetrics.horizontalAdvance(unit_));
        unitRect = QRectF(text.right() - unitWidth, text.top(), unitWidth, text.height());
        painter.setPen(inkForState(Color::Muted, state));
        painter.drawText(unitRect, Qt::AlignVCenter | Qt::AlignRight, unit_);
    }
    if (editing_) {
        // The line edit draws the NUMBER while it is up; the cell, its one border and the unit stay
        // this widget's own. The unit deliberately keeps being painted -- a unit that vanished on
        // entering edit would both lose information and move the number it labels.
        return;
    }
    painter.setPen(inkForState(Color::Foreground, state));
    const qreal valueRight = unit_.isEmpty() ? text.right() : unitRect.left() - px(Spacing::XS);
    painter.drawText(QRectF(text.left(), text.top(), valueRight - text.left(), text.height()),
                     Qt::AlignVCenter | Qt::AlignRight, displayedValue());
}

} // namespace bloom::ui::kit
