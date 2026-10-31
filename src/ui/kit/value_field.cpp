#include <bloom/ui/kit/value_field.hpp>

#include "value_field_format.hpp"

#include <bloom/ui/kit/accessibility.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/inline_edit.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/theme.hpp>

#include <QAccessible>
#include <QFontMetrics>
#include <QPainter>
#include <QPalette>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>

// The value field is split in two: this file owns its configuration, formatting, geometry and
// painting; value_field_input.cpp owns the gestures (scrub, keys, wheel, typed entry).

namespace bloom::ui::kit {
namespace {

// The cell's own horizontal padding: the inset the painted value AND the inline editor both use, so
// the number sits at one x whichever of the two is drawing it.
constexpr int kCellPaddingX = px(Spacing::S);

// QLineEdit lays its text out inside its content rectangle inset by a further fixed two logical
// pixels per side (QLineEditPrivate::horizontalMargin), which Qt exposes through neither a public
// accessor nor a style hint. The cell's padding is therefore handed to the editor as TEXT margins
// reduced by exactly that much, which is what makes the glyphs land on the painter's own x --
// kit_value_field_tests.cpp pins that off a real render rather than off this arithmetic.
constexpr int kLineEditTextMargin = 2;

// The resting text of a multi-selection whose values differ.
const QString& mixedText() {
    static const QString text = QString(QChar(0x2014));
    return text;
}

} // namespace

KValueField::KValueField(QWidget* parent) : QWidget(parent) {
    installKitAccessibility();
    setFixedHeight(px(Size::Control));
    ensureKeyboardFocusTracking(*this);
    setObjectName(QStringLiteral("kValueField"));
    // Tab reaches the field; a click does not take focus. A press is a scrub or opens the editor,
    // and neither needs the field itself to hold focus afterwards -- holding it is what used to
    // leave the transport and frame-step keys dead after a scrub.
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover, true);
    setFont(kit::font(TypeRole::Ui));
    // The whole control is a scrub handle, label included -- exactly as in After Effects, where
    // dragging the parameter's name drags its value.
    setCursor(Qt::SizeHorCursor);
    resetText_ = tr("Reset");

    // The text editor exists for the control's whole life rather than being built on demand: it
    // carries focus while editing, and a widget that is created and destroyed inside a focus
    // transition is a reliable way to lose that focus to the wrong place. It is the kit text field
    // in its embedded form: no box of its own, because the cell this widget paints is the only
    // fill and its hairline the only border.
    editor_ = new KTextField(this);
    editor_->setObjectName(QStringLiteral("kValueFieldEditor"));
    editor_->setEmbedded(true);
    editor_->setTypeRole(TypeRole::Value);
    editor_->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    editor_->setCursor(Qt::IBeamCursor);
    // Window is what Qt fills a widget's rectangle with whenever the widget is painted as a root --
    // which is exactly what happens to it inside a QGraphicsProxyWidget on a node card.
    QPalette editorPalette = editor_->palette();
    editorPalette.setColor(QPalette::Base, Qt::transparent);
    editorPalette.setColor(QPalette::Window, Qt::transparent);
    editorPalette.setColor(QPalette::Text, color(Color::Foreground));
    editorPalette.setColor(QPalette::Highlight, color(Color::Accent));
    editorPalette.setColor(QPalette::HighlightedText, color(Color::Foreground));
    editor_->setPalette(editorPalette);
    editor_->setAttribute(Qt::WA_NoSystemBackground, true);
    editor_->setAutoFillBackground(false);
    editor_->hide();
    editor_->installEventFilter(this);
    inline_ = &editor_->useInlineEdit([this](const QString& text) { return refusalFor(text); });
    connect(inline_, &KInlineEdit::refusalChanged, this, [this] { update(); });
    connect(inline_, &KInlineEdit::accepted, this,
            [this](const QString&, const KInlineEdit::Ending ending) {
                endEdit(true);
                if (ending == KInlineEdit::Ending::Tab || ending == KInlineEdit::Ending::Backtab)
                    focusNextPrevChild(ending == KInlineEdit::Ending::Tab);
            });
    connect(inline_, &KInlineEdit::refused, this, [this] { endEdit(false); });
    connect(editor_, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (editing_)
            (void)applyTyped(text);
    });

    stepperTimer_ = new QTimer(this);
    connect(stepperTimer_, &QTimer::timeout, this, &KValueField::stepperRepeat);
    wheelIdle_ = new QTimer(this);
    wheelIdle_->setSingleShot(true);
    wheelIdle_->setInterval(kWheelGestureIdleMs);
    connect(wheelIdle_, &QTimer::timeout, this, [this] {
        if (wheelGesture_) {
            wheelGesture_ = false;
            finishInteraction(true);
        }
    });
}

// #401: the setters below are called on every projection, including once per playback tick. An
// unchanged setting does nothing at all -- no geometry request, no repaint -- so a time change
// costs a field only the repaint its own new number needs.
void KValueField::setCompact(const bool compact) {
    if (compact_ == compact)
        return;
    compact_ = compact;
    if (compact) {
        setMinimumWidth(px(Size::PropertiesFieldMinWidth));
        setMaximumWidth(px(Size::PropertiesFieldWidth));
        setFixedHeight(px(Size::Control));
    }
    updateGeometry();
    update();
}

void KValueField::setOutlinedAtRest(const bool outlined) {
    outlinedAtRest_ = outlined;
    update();
}

void KValueField::setStepper(const bool stepper) {
    stepper_ = stepper;
    if (editing_)
        layOutEditor();
    update();
}

void KValueField::setLabel(const QString& label) {
    if (label_ == label)
        return;
    label_ = label;
    updateGeometry();
    if (editing_)
        layOutEditor();
    update();
}

QString KValueField::label() const { return label_; }

void KValueField::setUnit(const QString& unit) {
    if (unit_ == unit)
        return;
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

void KValueField::setSpecialValueText(const QString& text) {
    if (specialValueText_ == text)
        return;
    specialValueText_ = text;
    updateGeometry();
    update();
}

QString KValueField::specialValueText() const { return specialValueText_; }

void KValueField::setPresentation(const ValueFieldPresentation& presentation) {
    if (interactionActive_ || this->presentation() == presentation)
        return;
    setIntegerMode(presentation.integer);
    setDisplayScale(presentation.displayScale);
    if (presentation.integer)
        setIntegerRange(valueFieldSaturatingInteger(std::ceil(presentation.minimum)),
                        valueFieldSaturatingInteger(std::floor(presentation.maximum)));
    else
        setRange(presentation.minimum, presentation.maximum);
    setSoftRange(presentation.softMinimum, presentation.softMaximum);
    setSingleStep(presentation.step);
    setDecimals(presentation.integer ? 0 : presentation.decimals);
    setUnit(presentation.unit);
}

ValueFieldPresentation KValueField::presentation() const {
    return {.minimum = minimum(),
            .maximum = maximum(),
            .softMinimum = softMinimum(),
            .softMaximum = softMaximum(),
            .step = step_,
            .decimals = decimals_,
            .unit = unit_,
            .displayScale = displayScale_,
            .integer = integer_};
}

void KValueField::setRange(const double minimum, const double maximum) {
    if (integer_) {
        setIntegerRange(valueFieldSaturatingInteger(std::ceil(minimum)),
                        valueFieldSaturatingInteger(std::floor(maximum)));
        return;
    }
    if (minimum_ == minimum && maximum_ == std::max(maximum, minimum))
        return;
    minimum_ = minimum;
    maximum_ = std::max(maximum, minimum);
    commitValue(value_);
    update();
}

double KValueField::minimum() const noexcept { return minimum_; }

double KValueField::maximum() const noexcept { return maximum_; }

void KValueField::setSoftRange(const double minimum, const double maximum) {
    softMinimum_ = std::max(minimum, minimum_);
    softMaximum_ = std::min(std::max(maximum, minimum), maximum_);
}

void KValueField::setSingleStep(const double step) {
    step_ = std::abs(step);
    if (integer_)
        step_ = std::max(1.0, std::round(step_));
}

double KValueField::singleStep() const noexcept { return step_; }

void KValueField::setDecimals(const int decimals) {
    const int next = integer_ ? 0 : std::clamp(decimals, 0, 9);
    if (decimals_ == next)
        return;
    decimals_ = next;
    updateGeometry();
    update();
}

int KValueField::decimals() const noexcept { return decimals_; }

void KValueField::setDisplayScale(const double scale) {
    displayScale_ = scale != 0.0 && std::isfinite(scale) ? scale : 1.0;
}

double KValueField::value() const noexcept { return value_; }

void KValueField::setValue(const double value) {
    // See the header: a live gesture owns the cell. A projection that arrived mid-drag -- another
    // surface's edit, a time change -- would otherwise snap the number back under the pointer and
    // the rest of the gesture would carry on from the wrong base.
    if (scrubbing_ || editing_)
        return;
    if (mixed_) {
        mixed_ = false;
        update();
    }
    if (integer_)
        commitInteger(valueFieldSaturatingInteger(std::round(value)));
    else
        commitValue(value);
}

void KValueField::setIntegerMode(const bool integer) {
    if (integer_ == integer)
        return;
    integer_ = integer;
    if (integer) {
        integerMinimum_ = valueFieldSaturatingInteger(std::ceil(minimum_));
        integerMaximum_ =
            std::max(integerMinimum_, valueFieldSaturatingInteger(std::floor(maximum_)));
        integerValue_ = std::clamp(valueFieldSaturatingInteger(std::round(value_)), integerMinimum_,
                                   integerMaximum_);
        minimum_ = static_cast<double>(integerMinimum_);
        maximum_ = static_cast<double>(integerMaximum_);
        value_ = static_cast<double>(integerValue_);
        decimals_ = 0;
        step_ = std::max(1.0, std::round(step_));
        displayScale_ = 1.0;
    }
    updateGeometry();
    update();
}

void KValueField::setIntegerRange(const std::int64_t minimum, const std::int64_t maximum) {
    if (!integer_)
        setIntegerMode(true);
    else if (integerMinimum_ == minimum && integerMaximum_ == std::max(minimum, maximum))
        return;
    integerMinimum_ = minimum;
    integerMaximum_ = std::max(minimum, maximum);
    minimum_ = static_cast<double>(integerMinimum_);
    maximum_ = static_cast<double>(integerMaximum_);
    commitInteger(integerValue_);
    updateGeometry();
    update();
}

void KValueField::setIntegerValue(const std::int64_t value) {
    if (scrubbing_ || editing_)
        return;
    if (!integer_)
        setIntegerMode(true);
    if (mixed_) {
        mixed_ = false;
        update();
    }
    commitInteger(value);
}

void KValueField::setMixed(const bool mixed) {
    if (interactionActive_ || mixed_ == mixed)
        return;
    mixed_ = mixed;
    update();
}

void KValueField::setDefaultValue(const double value) {
    default_ = value;
    update();
}

void KValueField::clearDefaultValue() {
    default_.reset();
    update();
}

bool KValueField::isNonDefault() const noexcept {
    // Half of the last shown place is the tolerance: a default the display cannot tell from the
    // value is no difference to mark.
    return default_.has_value() && !mixed_ &&
           std::abs(value_ - *default_) > 0.5 * std::pow(10.0, -decimals_);
}

void KValueField::setResetText(const QString& text) { resetText_ = text; }

void KValueField::commitValue(const double value) {
    if (!std::isfinite(value))
        return;
    if (integer_) {
        commitInteger(valueFieldSaturatingInteger(std::round(value)));
        return;
    }
    const double clamped = std::clamp(value, minimum_, maximum_);
    if (clamped == value_)
        return;
    value_ = clamped;
    update();
    if (QAccessible::isActive()) {
        QAccessibleValueChangeEvent change(this, value_);
        QAccessible::updateAccessibility(&change);
    }
    Q_EMIT valueChanged(value_);
}

void KValueField::commitInteger(const std::int64_t value) {
    const std::int64_t clamped = std::clamp(value, integerMinimum_, integerMaximum_);
    if (clamped == integerValue_ && static_cast<double>(clamped) == value_)
        return;
    integerValue_ = clamped;
    value_ = static_cast<double>(clamped);
    update();
    if (QAccessible::isActive()) {
        QAccessibleValueChangeEvent change(this, static_cast<qlonglong>(integerValue_));
        QAccessible::updateAccessibility(&change);
    }
    Q_EMIT integerValueChanged(static_cast<qint64>(integerValue_));
    Q_EMIT valueChanged(value_);
}

double KValueField::scrubScale(const Qt::KeyboardModifiers modifiers) noexcept {
    // Shift outranks Ctrl when both are held: the coarse gesture is the one the artist asked for
    // last in every host that offers both, and one of the two has to win.
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        return kCoarseStepMultiple;
    }
    if (modifiers.testFlag(Qt::ControlModifier)) {
        return 1.0 / kCoarseStepMultiple;
    }
    return 1.0;
}

bool KValueField::isScrubbing() const noexcept { return scrubbing_; }

bool KValueField::isEditing() const noexcept { return editing_; }

QLineEdit* KValueField::lineEdit() const noexcept { return editor_; }

QString KValueField::refusal() const { return inline_->refusal(); }

QString KValueField::numberText(const int decimals) const {
    return integer_ ? valueFieldExactText(integerValue_) : valueFieldNumber(value_, decimals);
}

QString KValueField::editText() const {
    return integer_ ? valueFieldExactText(integerValue_) : valueFieldExactText(value_);
}

QString KValueField::displayedValue() const {
    if (mixed_)
        return mixedText();
    if (!specialValueText_.isEmpty() && value_ == minimum_)
        return specialValueText_;
    return numberText(decimals_);
}

KValueField::RestingText KValueField::restingText() const {
    const auto full = displayedValue();
    const bool special = mixed_ || (!specialValueText_.isEmpty() && value_ == minimum_);
    const bool wantsUnit = !unit_.isEmpty() && !special;
    const QFontMetrics metrics(kit::font(TypeRole::Value));
    const auto column = cellTextRect().width();
    const auto unitColumn = wantsUnit ? metrics.horizontalAdvance(unit_) + px(Spacing::XS) : 0;
    const auto fits = [&](const QString& text, const int reserved) {
        return metrics.horizontalAdvance(text) + reserved <= column;
    };
    if (fits(full, unitColumn) || special)
        return {full, wantsUnit, false};
    if (fits(full, 0))
        return {full, false, false};
    // Fewer places, rounded, until the number fits.
    if (!integer_) {
        for (int places = decimals_ - 1; places >= 0; --places) {
            const auto shorter = numberText(places);
            if (fits(shorter, 0))
                return {shorter, false, true};
        }
    }
    // Then scientific notation with fewer and fewer significant digits, and an ellipsis when not
    // even one digit fits: a number cut by its column would misstate the value.
    constexpr int kMostSignificantDigits = 6;
    for (int digits = kMostSignificantDigits; digits >= 1; --digits) {
        const auto scientific = valueFieldScientific(value_, digits);
        if (fits(scientific, 0))
            return {scientific, false, true};
    }
    return {QString(QChar(0x2026)), false, true};
}

QString KValueField::paintedText() const {
    const auto text = restingText();
    return text.unit ? text.value + QLatin1Char(' ') + unit_ : text.value;
}

bool KValueField::isDisplayRounded() const { return restingText().rounded; }

QString KValueField::exactToolTip() const {
    if (mixed_)
        return tr("Mixed values");
    const auto exact = editText();
    return unit_.isEmpty() ? exact : exact + QLatin1Char(' ') + unit_;
}

int KValueField::labelColumnWidth() const {
    // Measured in the role it is painted in; a longer label elides inside the capped column.
    return std::min(px(Size::ValueFieldLabelColumn),
                    QFontMetrics(kit::font(TypeRole::UiSmall)).horizontalAdvance(label_));
}

QRectF KValueField::labelRect() const {
    if (label_.isEmpty()) {
        return {};
    }
    return {compact_ ? static_cast<qreal>(px(Spacing::XS)) : 0.0, 0.0,
            static_cast<qreal>(labelColumnWidth()), static_cast<qreal>(height())};
}

QRectF KValueField::cellRect() const {
    const qreal left = compact_ || label_.isEmpty() ? 0.0 : labelColumnWidth() + px(Spacing::S);
    const auto margin = px(Spacing::ControlInset);
    return QRectF(left, 0.0, std::max(0.0, width() - left), height())
        .adjusted(margin, margin, -margin, -margin);
}

QRectF KValueField::cellTextRect() const {
    const int padding = compact_ ? px(Spacing::XS) : kCellPaddingX;
    const int prefix = compact_ && !label_.isEmpty() ? labelColumnWidth() + px(Spacing::XXS) : 0;
    return cellRect().adjusted(padding + prefix, 0.0,
                               -padding - (stepper_ ? px(Size::PropertiesStepperWidth) : 0), 0.0);
}

Color KValueField::borderToken() const {
    if (editing_ && inline_->hasRefusal())
        return Color::Error;
    return borderForInteraction(isEnabled(), editing_ || hasKeyboardFocus(*this), hovered_);
}

QColor KValueField::cellBorderColor() const {
    if (editing_ && inline_->hasRefusal())
        return color(Color::Error);
    // Borderless at rest unless outlined: the resting token resolves to nothing drawn at all, so
    // the cell is a plain Field rectangle until hover or focus gives it its one border.
    return fieldColors(isEnabled(), editing_ || hasKeyboardFocus(*this), hovered_, outlinedAtRest_)
        .border;
}

State KValueField::visualState() const {
    if (!isEnabled()) {
        return State::Disabled;
    }
    if (hovered_) {
        return State::Hover;
    }
    if (editing_ || hasKeyboardFocus(*this)) {
        return State::Focused;
    }
    return State::Normal;
}

QSize KValueField::sizeHint() const {
    if (compact_) {
        const auto specialWidth =
            QFontMetrics(kit::font(TypeRole::Value)).horizontalAdvance(specialValueText_) +
            px(Spacing::XS) * 2;
        return {std::max(px(Size::PropertiesFieldWidth), specialWidth), px(Size::Control)};
    }
    const QFontMetrics valueMetrics(kit::font(TypeRole::Value));
    // Sized for the widest number the range can produce, not for the number currently in it: the
    // field must not resize as digits change.
    const QString widest =
        integer_ ? (valueFieldExactText(integerMinimum_).size() >
                            valueFieldExactText(integerMaximum_).size()
                        ? valueFieldExactText(integerMinimum_)
                        : valueFieldExactText(integerMaximum_))
                 : valueFieldNumber(std::max(std::abs(minimum_), std::abs(maximum_)), decimals_);
    int cellWidth = valueMetrics.horizontalAdvance(widest) + px(Spacing::S) * 2;
    if (!unit_.isEmpty()) {
        cellWidth += valueMetrics.horizontalAdvance(unit_) + px(Spacing::XS);
    }
    cellWidth =
        std::max(cellWidth, valueMetrics.horizontalAdvance(specialValueText_) + px(Spacing::S) * 2);
    const int labelWidth = label_.isEmpty() ? 0 : labelColumnWidth() + px(Spacing::S);
    // Exactly the control height -- no ring margin, for the same reason cellRect() reserves none.
    return {labelWidth + cellWidth, px(Size::Control)};
}

QSize KValueField::minimumSizeHint() const {
    if (compact_)
        return {minimumWidth(), px(Size::Control)};
    // sizeHint() above is this field's PREFERRED width, sized for the widest number its own range
    // can produce so the cell never resizes as digits change. minimumSizeHint() is the FLOOR this
    // cell may shrink to when the panel hosting it is narrower than every field's combined
    // preferred width: Size::ValueCellMin, so a narrow panel degrades every cell in it to the same
    // legible width instead of each row committing to its own.
    const int labelWidth = label_.isEmpty() ? 0 : labelColumnWidth() + px(Spacing::S);
    return {labelWidth + px(Size::ValueCellMin), px(Size::Control)};
}

void KValueField::layOutEditor() {
    // The editor IS the cell: exactly its rectangle, so the Field fill and the one hairline this
    // widget paints stay the cell's own outline rather than being joined by a second, smaller
    // rectangle with its own background and corners. The cell's padding reaches the editor as text
    // margins instead of as geometry, and the unit's column is reserved on the right because the
    // unit keeps being painted while editing -- together that is what holds the number's x fixed
    // across entering and leaving edit.
    editor_->setGeometry(cellRect().toRect());
    const QFontMetrics valueMetrics(kit::font(TypeRole::Value));
    const int unitColumn =
        unit_.isEmpty() ? 0 : valueMetrics.horizontalAdvance(unit_) + px(Spacing::XS);
    const auto text = cellTextRect();
    editor_->setTextMargins(
        static_cast<int>(text.left() - cellRect().left()) - kLineEditTextMargin, 0,
        static_cast<int>(cellRect().right() - text.right()) + unitColumn - kLineEditTextMargin, 0);
}

void KValueField::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const State state = visualState();

    // The one text-entry recipe (kit::fieldColors): a Field well that reads differently from every
    // button. A field in a chrome row or on a node card keeps a resting outline.
    const QRectF cell = cellRect();
    fillRoundedSurface(painter, cell, color(Color::Field), cellBorderColor(), Radius::Small);
    if (!label_.isEmpty()) {
        painter.setFont(kit::font(TypeRole::UiSmall));
        painter.setPen(inkForState(Color::Muted, state));
        const QRectF labelBounds = labelRect();
        painter.drawText(labelBounds, Qt::AlignVCenter | Qt::AlignLeft,
                         QFontMetrics(painter.font())
                             .elidedText(label_, Qt::ElideRight,
                                         static_cast<int>(std::floor(labelBounds.width()))));
    }
    if (stepper_) {
        const int extent = px(Size::PropertiesStepperWidth);
        const auto tint = inkForState(Color::Muted, state);
        painter.drawPixmap(QRect(width() - extent, 0, extent, height() / 2),
                           iconPixmap(IconId::CaretUp, Size::IconSmall, tint, devicePixelRatioF(),
                                      iconWeight(IconRole::Chrome)));
        painter.drawPixmap(QRect(width() - extent, height() / 2, extent, height() / 2),
                           iconPixmap(IconId::CaretDown, Size::IconSmall, tint, devicePixelRatioF(),
                                      iconWeight(IconRole::Chrome)));
    }
    if (isNonDefault()) {
        // The differs-from-default dot, inside the cell's top-right corner, clear of the number.
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal mark = px(Size::NonDefaultMark);
        const qreal inset = px(Spacing::XXS);
        painter.setPen(Qt::NoPen);
        painter.setBrush(inkForState(Color::Accent, state));
        painter.drawEllipse(QRectF(cell.right() - inset - mark, cell.top() + inset, mark, mark));
        painter.restore();
    }

    // The value takes the monospaced role; the unit takes muted ink so it reads as a unit rather
    // than as part of the number.
    painter.setFont(kit::font(TypeRole::Value));
    const QFontMetrics valueMetrics(painter.font());
    const QRectF text = cellTextRect();
    QRectF unitRect;
    const auto resting = restingText();
    const bool showUnit = !unit_.isEmpty() && !mixed_ && (editing_ || resting.unit);
    if (showUnit) {
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
    painter.setPen(inkForState(mixed_ ? Color::Muted : Color::Foreground, state));
    const qreal valueRight = showUnit ? unitRect.left() - px(Spacing::XS) : text.right();
    // Clipped to the cell so nothing can paint over the sub-label or the border.
    painter.setClipRect(cell);
    const QRectF valueRect(text.left(), text.top(), valueRight - text.left(), text.height());
    painter.drawText(valueRect, Qt::AlignVCenter | Qt::AlignRight, resting.value);
    if (resting.rounded) {
        // A rounded display is underlined with a dotted Faint hairline: the number is a reading,
        // and hovering it (or opening the editor) shows the exact value.
        const qreal advance = valueMetrics.horizontalAdvance(resting.value);
        const qreal baseline = valueRect.center().y() +
                               (valueMetrics.ascent() - valueMetrics.descent()) / 2.0 +
                               px(Spacing::XXS);
        QPen pen(inkForState(Color::Faint, state));
        pen.setWidthF(snappedHairlineWidth(devicePixelRatioF()));
        pen.setStyle(Qt::DotLine);
        painter.setPen(pen);
        painter.drawLine(QPointF(valueRight - advance, baseline), QPointF(valueRight, baseline));
    }
}

} // namespace bloom::ui::kit
