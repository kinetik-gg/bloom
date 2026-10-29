// KValueField's gestures: the scrub, the stepper, keys, the wheel and typed entry. Configuration,
// formatting, geometry and painting are in value_field.cpp.

#include <bloom/ui/kit/value_field.hpp>

#include "value_field_format.hpp"

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/inline_edit.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/value_expression.hpp>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace bloom::ui::kit {
namespace {

struct Typed {
    QString reason;
    double value = 0.0;
    std::int64_t integer = 0;
    bool empty = false;
};

[[nodiscard]] QString reasonFor(const ValueExpressionResult& result, const QString& unit) {
    switch (result.error) {
    case ValueExpressionError::None:
        return {};
    case ValueExpressionError::Empty:
        return KValueField::tr("Type a number");
    case ValueExpressionError::Syntax:
        return KValueField::tr("Not a number");
    case ValueExpressionError::UnknownUnit:
        return unit.isEmpty()
                   ? KValueField::tr("This value takes no unit, not %1")
                         .arg(QString::fromStdString(result.unit))
                   : KValueField::tr("This value is in %1, not %2")
                         .arg(unit, QString::fromStdString(result.unit));
    case ValueExpressionError::DivideByZero:
        return KValueField::tr("Division by zero");
    case ValueExpressionError::NotFinite:
        return KValueField::tr("The result is too large");
    case ValueExpressionError::NotInteger:
        return KValueField::tr("Must be a whole number");
    case ValueExpressionError::Overflow:
        return KValueField::tr("Outside the whole-number range");
    }
    return KValueField::tr("Not a number");
}

[[nodiscard]] std::int64_t saturatingAdd(const std::int64_t base, const std::int64_t delta) {
    if (delta > 0 && base > std::numeric_limits<std::int64_t>::max() - delta)
        return std::numeric_limits<std::int64_t>::max();
    if (delta < 0 && base < std::numeric_limits<std::int64_t>::min() - delta)
        return std::numeric_limits<std::int64_t>::min();
    return base + delta;
}

// Only where the windowing system lets an application move the pointer.
[[nodiscard]] bool pointerCanWarp() {
    static const bool warps = [] {
        const auto platform = QGuiApplication::platformName();
        return platform == QLatin1String("xcb") || platform == QLatin1String("windows") ||
               platform == QLatin1String("cocoa");
    }();
    return warps;
}

[[nodiscard]] bool isStepKey(const int key) {
    return key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_PageUp ||
           key == Qt::Key_PageDown;
}

// The characters that start an entry when typed into a keyboard-focused field at rest. Letters are
// left alone, so a tool shortcut still reaches the window.
[[nodiscard]] bool startsEntry(const QKeyEvent& key) {
    if (key.modifiers().testAnyFlags(Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier) ||
        key.text().size() != 1)
        return false;
    const QChar c = key.text().front();
    return c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('+') ||
           c == QLatin1Char('.') || c == QLatin1Char(',') || c == QLatin1Char('(');
}

} // namespace

// ---- Typed entry -------------------------------------------------------------------------------

QString KValueField::refusalFor(const QString& text) const {
    if (mixed_ && text.trimmed().isEmpty())
        return {};
    const auto utf8 = text.toUtf8();
    const auto unit = unit_.toUtf8();
    ValueExpressionContext context;
    context.current = interactionActive_ ? interactionBase_ : value_;
    context.currentInteger = interactionActive_ ? interactionIntegerBase_ : integerValue_;
    context.unit = std::string_view(unit.constData(), static_cast<std::size_t>(unit.size()));
    const std::string_view typed(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    const auto result = integer_ ? evaluateIntegerExpression(typed, context)
                                 : evaluateValueExpression(typed, context);
    if (!result.ok())
        return reasonFor(result, unit_);
    // Refuse, never clamp: a typed value outside the range is the artist's mistake to see.
    const bool below = integer_ ? result.integer < integerMinimum_ : result.value < minimum_;
    const bool above = integer_ ? result.integer > integerMaximum_ : result.value > maximum_;
    if (!below && !above)
        return {};
    const auto bound = [this](const bool upper) {
        return integer_ ? valueFieldExactText(upper ? integerMaximum_ : integerMinimum_)
                        : valueFieldExactText(upper ? maximum_ : minimum_);
    };
    const bool open = integer_ ? integerMaximum_ == std::numeric_limits<std::int64_t>::max()
                               : maximum_ == std::numeric_limits<double>::max();
    const bool floorOpen = integer_ ? integerMinimum_ == std::numeric_limits<std::int64_t>::min()
                                    : minimum_ == std::numeric_limits<double>::lowest();
    if (open)
        return tr("Must be at least %1").arg(bound(false));
    if (floorOpen)
        return tr("Must be at most %1").arg(bound(true));
    return tr("Must be from %1 to %2").arg(bound(false), bound(true));
}

bool KValueField::applyTyped(const QString& text) {
    if (!refusalFor(text).isEmpty())
        return false;
    if (mixed_ && text.trimmed().isEmpty())
        return true;
    const auto utf8 = text.toUtf8();
    const auto unit = unit_.toUtf8();
    ValueExpressionContext context;
    context.current = interactionBase_;
    context.currentInteger = interactionIntegerBase_;
    context.unit = std::string_view(unit.constData(), static_cast<std::size_t>(unit.size()));
    const std::string_view typed(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    if (integer_) {
        commitInteger(evaluateIntegerExpression(typed, context).integer);
    } else {
        commitValue(evaluateValueExpression(typed, context).value);
    }
    if (mixed_) {
        // A value typed into a mixed field is a real value from here on.
        mixed_ = false;
        update();
    }
    return true;
}

void KValueField::beginEdit(const QString& seed) {
    if (editing_ || !isEnabled()) {
        return;
    }
    beginInteraction();
    editing_ = true;
    layOutEditor();
    inline_->clearRefusal();
    editor_->setText(seed.isEmpty() ? (mixed_ ? QString{} : editText()) : seed);
    editor_->show();
    editor_->setFocus(Qt::OtherFocusReason);
    if (seed.isEmpty()) {
        // Select all on entry: a click-to-edit that kept the caret where the pointer landed would
        // make replacing the number a two-gesture job.
        editor_->selectAll();
    } else {
        editor_->deselect();
        editor_->end(false);
        (void)applyTyped(seed);
    }
    update();
}

void KValueField::endEdit(const bool applyText) {
    if (!editing_)
        return;
    if (applyText)
        (void)applyTyped(editor_->text());
    editing_ = false;
    // Focus comes back to the field BEFORE the editor hides. Hiding a focused widget hands focus to
    // the next one in the chain, and a Tab that then travelled on from there skipped a field.
    if (editor_->hasFocus())
        setFocus(Qt::OtherFocusReason);
    editor_->hide();
    inline_->clearRefusal();
    // Escape confirms, and a refused entry keeps the last valid value: either way the edit is a
    // commit of whatever the cell now holds (nothing at all when that is where it started).
    finishInteraction(true);
    update();
}

// ---- The edit boundary -------------------------------------------------------------------------

void KValueField::beginInteraction() {
    if (interactionActive_)
        return;
    interactionBase_ = value_;
    interactionIntegerBase_ = integerValue_;
    interactionActive_ = true;
    Q_EMIT editStarted();
}

void KValueField::finishInteraction(const bool keep) {
    if (!interactionActive_)
        return;
    interactionActive_ = false;
    wheelGesture_ = false;
    wheelIdle_->stop();
    if (!keep) {
        const QSignalBlocker blocker(this);
        if (integer_)
            commitInteger(interactionIntegerBase_);
        else
            commitValue(interactionBase_);
    }
    if (keep)
        Q_EMIT editFinished();
    else
        Q_EMIT editCancelled();
}

void KValueField::cancelEdit() {
    const bool wasScrubbing = scrubbing_;
    if (wasScrubbing)
        releaseKeyboard();
    pressed_ = false;
    scrubbing_ = false;
    stepping_ = false;
    keyStepping_ = false;
    stepperTimer_->stop();
    if (editing_) {
        editing_ = false;
        if (editor_->hasFocus())
            setFocus(Qt::OtherFocusReason);
        editor_->hide();
        inline_->clearRefusal();
    }
    finishInteraction(false);
    if (wasScrubbing)
        Q_EMIT scrubCancelled();
    update();
}

void KValueField::resetToDefault() {
    if (!default_.has_value() || !isEnabled())
        return;
    if (editing_)
        endEdit(true);
    else
        finishInteraction(true);
    beginInteraction();
    if (integer_)
        commitInteger(valueFieldSaturatingInteger(std::round(*default_)));
    else
        commitValue(*default_);
    finishInteraction(true);
}

void KValueField::stepFrom(const double base, const std::int64_t integerBase, const double steps) {
    const double delta = steps * step_;
    if (integer_)
        commitInteger(saturatingAdd(integerBase, valueFieldSaturatingInteger(std::round(delta))));
    else
        commitValue(base + delta);
}

void KValueField::stepBy(const int steps) {
    stepFrom(value_, integerValue_, static_cast<double>(steps));
}

void KValueField::stepperRepeat() {
    stepBy(stepperDirection_);
    stepperTimer_->setInterval(kAutoRepeatIntervalMs);
}

// ---- The scrub ---------------------------------------------------------------------------------

void KValueField::finishScrub() {
    const bool wasScrubbing = scrubbing_;
    pressed_ = false;
    scrubbing_ = false;
    if (wasScrubbing)
        releaseKeyboard();
    if (wasScrubbing) {
        // The one moment in the gesture at which a value is worth writing down.
        Q_EMIT scrubFinished();
        finishInteraction(true);
    }
    update();
}

void KValueField::wrapPointer(const QPointF& global) {
    if (!pointerCanWarp() || screen() == nullptr)
        return;
    // Within a margin of the screen edge the pointer jumps to the other side, and every later x is
    // shifted back by the jump, so the gesture carries on as if the screen had no edge.
    const QRect bounds = screen()->geometry();
    const int margin = px(Size::ScrubWrapMargin);
    const int x = static_cast<int>(std::lround(global.x()));
    int target = x;
    if (x <= bounds.left() + margin)
        target = bounds.right() - margin - 1;
    else if (x >= bounds.right() - margin)
        target = bounds.left() + margin + 1;
    if (target == x)
        return;
    QCursor::setPos(screen(), QPoint(target, static_cast<int>(std::lround(global.y()))));
    wrapOffset_ -= target - x;
}

void KValueField::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isEnabled()) {
        if (editing_)
            endEdit(true);
        if (stepper_ && event->position().x() >= width() - px(Size::StepperHitWidth)) {
            stepping_ = true;
            stepperDirection_ = event->position().y() < height() / 2.0 ? 1 : -1;
            beginInteraction();
            stepBy(stepperDirection_);
            stepperTimer_->start(kAutoRepeatDelayMs);
            event->accept();
            return;
        }
        pressed_ = true;
        scrubbing_ = false;
        wrapOffset_ = 0.0;
        pressPosition_ = event->position();
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
    const double x = event->position().x() + wrapOffset_;
    const double threshold = QApplication::startDragDistance();
    const double scale = scrubScale(event->modifiers());
    if (!scrubbing_) {
        if (std::abs(x - pressPosition_.x()) < threshold) {
            event->accept();
            return;
        }
        scrubbing_ = true;
        firstSegment_ = true;
        segmentStartX_ = pressPosition_.x();
        segmentBase_ = value_;
        segmentIntegerBase_ = integerValue_;
        segmentScale_ = scale;
        beginInteraction();
        // The field holds no focus during a scrub, so it takes the keyboard for the gesture's
        // length: Escape (which confirms) and the modifiers reach it, not the window.
        grabKeyboard();
        Q_EMIT scrubStarted();
    }
    // Measured from the segment start, not from the previous move: a segment that wanders back to
    // where it started lands exactly on its base, with no accumulated rounding drift. The first
    // segment treats the drag threshold as a dead zone, so crossing it never jumps the value.
    const auto follow = [this, threshold](const double to) {
        double offset = to - segmentStartX_;
        if (firstSegment_)
            offset = offset > 0.0 ? std::max(0.0, offset - threshold)
                                  : std::min(0.0, offset + threshold);
        stepFrom(segmentBase_, segmentIntegerBase_, offset * segmentScale_);
    };
    follow(x);
    if (scale != segmentScale_) {
        // A modifier pressed or released mid-drag ends this segment where the pointer is (at the
        // old scale) and starts the next one there, from the value reached -- never a jump to
        // what the new scale would have given the whole drag.
        segmentStartX_ = x;
        segmentBase_ = value_;
        segmentIntegerBase_ = integerValue_;
        segmentScale_ = scale;
        firstSegment_ = false;
    }
    wrapPointer(event->globalPosition());
    event->accept();
}

void KValueField::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && stepping_) {
        stepping_ = false;
        stepperTimer_->stop();
        finishInteraction(true);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || !pressed_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (scrubbing_) {
        finishScrub();
    } else {
        // A press that never travelled is a click, and a click opens the editor.
        pressed_ = false;
        beginEdit();
    }
    event->accept();
}

void KValueField::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && default_.has_value() &&
        labelRect().contains(event->position())) {
        // The first click of the pair opened the editor; the double-click on the name resets.
        resetToDefault();
        event->accept();
        return;
    }
    mousePressEvent(event);
}

// ---- Keys, wheel and focus ---------------------------------------------------------------------

void KValueField::wheelEvent(QWheelEvent* event) {
    const auto modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier |
                                                 Qt::AltModifier | Qt::MetaModifier);
    // A wheel over a field at rest belongs to whatever is scrolling behind it: stealing it would
    // silently change a parameter the artist was only scrolling past. It steps a field being
    // edited, or a keyboard-focused one with a modifier held.
    int delta = event->angleDelta().y();
    // Some platforms turn Shift+wheel into a horizontal wheel.
    if (delta == 0 && modifiers.testFlag(Qt::ShiftModifier))
        delta = event->angleDelta().x();
    if (!isEnabled() || delta == 0 || !(editing_ || (hasFocus() && modifiers != Qt::NoModifier))) {
        QWidget::wheelEvent(event);
        return;
    }
    // High-resolution wheels report fractions of a notch; whole notches step.
    wheelRemainder_ += delta;
    const int steps = wheelRemainder_ / QWheelEvent::DefaultDeltasPerStep;
    wheelRemainder_ %= QWheelEvent::DefaultDeltasPerStep;
    if (steps != 0) {
        if (!interactionActive_) {
            beginInteraction();
            wheelGesture_ = true;
        }
        stepFrom(value_, integerValue_, steps * scrubScale(modifiers));
        if (editing_) {
            editor_->setText(editText());
            editor_->selectAll();
        }
    }
    if (wheelGesture_)
        wheelIdle_->start();
    event->accept();
}

void KValueField::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    if (isStepKey(key)) {
        if (!interactionActive_) {
            beginInteraction();
            keyStepping_ = true;
        }
        const int notch = key == Qt::Key_PageUp || key == Qt::Key_PageDown
                              ? static_cast<int>(kCoarseStepMultiple)
                              : 1;
        stepBy(key == Qt::Key_Up || key == Qt::Key_PageUp ? notch : -notch);
        event->accept();
        return;
    }
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        // The keyboard route into the text editor, for an artist who tabbed here.
        if (interactionActive_) {
            keyStepping_ = false;
            finishInteraction(true);
        } else {
            beginEdit();
        }
        event->accept();
        return;
    case Qt::Key_Escape:
        // Escape confirms a scrub, a held-key or a wheel edit. At rest it is not this field's: a
        // dialog around it rejects.
        if (scrubbing_ || pressed_) {
            finishScrub();
            event->accept();
            return;
        }
        if (interactionActive_) {
            keyStepping_ = false;
            finishInteraction(true);
            event->accept();
            return;
        }
        break;
    default:
        if (!interactionActive_ && startsEntry(*event)) {
            beginEdit(event->text());
            event->accept();
            return;
        }
        break;
    }
    QWidget::keyPressEvent(event);
}

void KValueField::keyReleaseEvent(QKeyEvent* event) {
    if (keyStepping_ && !event->isAutoRepeat() && isStepKey(event->key())) {
        keyStepping_ = false;
        finishInteraction(true);
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void KValueField::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    // Tab and Shift+Tab arrive ready to type, the way a form's fields do.
    if ((event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason) &&
        !editing_ && !interactionActive_)
        beginEdit();
}

void KValueField::focusOutEvent(QFocusEvent* event) {
    // A held-key or wheel edit ends when focus goes elsewhere. The editor taking focus from the
    // field is not that: it is the field's own text entry beginning.
    if (interactionActive_ && !editing_ && !scrubbing_ && !stepping_) {
        keyStepping_ = false;
        finishInteraction(true);
    }
    QWidget::focusOutEvent(event);
}

void KValueField::contextMenuEvent(QContextMenuEvent* event) {
    if (!default_.has_value() && actions().isEmpty()) {
        event->ignore();
        return;
    }
    std::unique_ptr<QMenu> menu(makeMenu(this));
    if (default_.has_value()) {
        auto* reset = menu->addAction(resetText_);
        reset->setObjectName(objectName() + QStringLiteral("ResetAction"));
        reset->setEnabled(isEnabled() && isNonDefault());
        connect(reset, &QAction::triggered, this, &KValueField::resetToDefault);
    }
    menu->addActions(actions());
    menu->exec(event->globalPos());
    event->accept();
}

bool KValueField::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        // A keyboard-focused field at rest owns the keys it answers, so a window shortcut cannot
        // take them first: the step keys, Return, the characters that start an entry, and Escape
        // while a held-key edit is open. Left/Right/Home/End are NOT claimed: they step the
        // transport, and only an open editor (which claims them itself) has a use for them.
        auto* key = static_cast<QKeyEvent*>(event);
        if (!hasFocus())
            break;
        if (isStepKey(key->key()) || key->key() == Qt::Key_Return ||
            key->key() == Qt::Key_Enter ||
            (key->key() == Qt::Key_Escape && interactionActive_) ||
            (!interactionActive_ && startsEntry(*key))) {
            event->accept();
            return true;
        }
        break;
    }
    case QEvent::ToolTip:
        // A fitted, rounded or mixed resting value shows the exact one on hover.
        if (toolTip().isEmpty() && !editing_ &&
            !(!specialValueText_.isEmpty() && value_ == minimum_ && !mixed_) &&
            (mixed_ || isDisplayRounded() || paintedText() != exactToolTip())) {
            const auto* help = static_cast<QHelpEvent*>(event);
            QToolTip::showText(help->globalPos(), exactToolTip(), this);
            return true;
        }
        break;
    case QEvent::UngrabMouse:
    case QEvent::WindowDeactivate:
        // Losing the pointer (a popup, a window switch, a compositor grab) or the window mid-scrub
        // ends the scrub where it is, as a commit; the same for a held stepper or key, and a wheel
        // burst.
        if (scrubbing_)
            finishScrub();
        pressed_ = false;
        if (stepping_) {
            stepping_ = false;
            stepperTimer_->stop();
            finishInteraction(true);
        }
        if (event->type() == QEvent::WindowDeactivate && (keyStepping_ || wheelGesture_)) {
            keyStepping_ = false;
            finishInteraction(true);
        }
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

bool KValueField::eventFilter(QObject* watched, QEvent* event) {
    // A compact field's label sits inside the cell, under the editor the first click of a
    // double-click opened; the double-click on that label is still the reset.
    if (watched == editor_ && event->type() == QEvent::MouseButtonDblClick &&
        default_.has_value()) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (labelRect().contains(editor_->mapToParent(mouse->position()))) {
            resetToDefault();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
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
    if (event->type() == QEvent::EnabledChange) {
        // A disabled field is not a scrub handle.
        setCursor(isEnabled() ? Qt::SizeHorCursor : Qt::ArrowCursor);
        if (!isEnabled()) {
            hovered_ = false;
            cancelEdit();
        }
    }
    QWidget::changeEvent(event);
}

void KValueField::resizeEvent(QResizeEvent* event) {
    if (editing_) {
        layOutEditor();
    }
    QWidget::resizeEvent(event);
}

} // namespace bloom::ui::kit
