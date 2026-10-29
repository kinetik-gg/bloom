#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QColor>
#include <QPoint>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <limits>
#include <optional>

class QLineEdit;
class QTimer;

namespace bloom::ui::kit {

class KInlineEdit;
class KTextField;

// How a numeric field presents one parameter, in DISPLAYED units: the hard range a typed value
// must fall inside, the soft range a slider beside it spans, the per-pixel step, the decimals, the
// unit and the factor between the stored and the displayed number (100 for a 0..1 opacity shown as
// a percentage). bloom::ui::configureValueField() fills one from a document parameter presentation.
struct ValueFieldPresentation {
    double minimum = std::numeric_limits<double>::lowest();
    double maximum = std::numeric_limits<double>::max();
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    double step = 1.0;
    int decimals = 2;
    QString unit;
    double displayScale = 1.0;
    bool integer = false;

    friend bool operator==(const ValueFieldPresentation&, const ValueFieldPresentation&) = default;
};

// A labelled numeric cell: the parameter's name, a field carrying the number in the monospaced
// Value role, and an optional unit suffix.
//
// The number is monospaced on purpose. Every numeric, unit, hex, and timecode surface in Bloom
// uses the Value role, so a column of them stays aligned and a digit changing does not reflow the
// text beside it.
//
// Interaction is After Effects' (ui-grammar.md, "Live Value Editing"). Press and drag
// horizontally anywhere on the control -- the label is a scrub handle too -- and the value follows
// the pointer at one step per pixel, ten times that with Shift and a tenth with Ctrl. The platform
// drag threshold is a dead zone, so crossing it never jumps the value, and changing a modifier
// mid-drag re-bases the gesture where the pointer is. Where the platform lets Bloom move the
// pointer (X11, Windows, macOS) it wraps at the screen edge so a scrub never runs out of travel.
// A press does not take keyboard focus, so frame stepping and the transport keys keep working
// after a scrub or a click.
//
// Press and release without travelling past the threshold and the cell becomes a text field with
// its contents selected. It takes the field's own unit, either decimal separator, arithmetic
// (+ - * / and parentheses) and relative entries (+=, -=, *=, /=); see value_expression.hpp.
// Return, Tab, Escape and focus loss all commit (kit::KInlineEdit: Escape confirms everywhere;
// Undo is the discard). Text that does not parse or falls outside the range is refused -- never
// clamped, never zero: Return keeps the editor open with an Error border and the reason, and
// Escape or focus loss closes it on the last valid value. Tab and Shift+Tab travel to the next
// field and open it for typing; a digit typed into a keyboard-focused field starts an edit with it.
// Up/Down and Page Up/Page Down step a focused field; the wheel steps a field being edited, or a
// focused one with a modifier held, and a burst of wheel steps is one edit.
class KValueField final : public QWidget {
    Q_OBJECT

  public:
    explicit KValueField(QWidget* parent = nullptr);

    // Compact properties mode places the label inside the field.
    void setCompact(bool compact);
    [[nodiscard]] bool isCompact() const noexcept { return compact_; }
    // Whether the resting cell draws its Border (a field in a chrome row or on a node card) or
    // rests borderless (a property cell, which already reads as a field). Off by default.
    void setOutlinedAtRest(bool outlined);
    [[nodiscard]] bool isOutlinedAtRest() const noexcept { return outlinedAtRest_; }
    void setStepper(bool stepper);
    [[nodiscard]] bool hasStepper() const noexcept { return stepper_; }

    void setLabel(const QString& label);
    [[nodiscard]] QString label() const;

    // A short suffix such as "px", "%", or "fps", drawn inside the cell after the number in the
    // muted ink so it reads as a unit rather than as part of the value. Typed text may carry it.
    void setUnit(const QString& unit);
    [[nodiscard]] QString unit() const;

    // Replaces the minimum value and its unit while the cell is at rest (for example, Automatic
    // for a zero budget). Editing still exposes the numeric value. Empty text restores numbers.
    void setSpecialValueText(const QString& text);
    [[nodiscard]] QString specialValueText() const;

    // The whole presentation at once. Does nothing to a field with a live edit.
    void setPresentation(const ValueFieldPresentation& presentation);
    [[nodiscard]] ValueFieldPresentation presentation() const;

    void setRange(double minimum, double maximum);
    [[nodiscard]] double minimum() const noexcept;
    [[nodiscard]] double maximum() const noexcept;
    // The range a slider beside this field spans; the hard range when none was set.
    void setSoftRange(double minimum, double maximum);
    [[nodiscard]] double softMinimum() const noexcept { return softMinimum_.value_or(minimum_); }
    [[nodiscard]] double softMaximum() const noexcept { return softMaximum_.value_or(maximum_); }

    void setSingleStep(double step);
    [[nodiscard]] double singleStep() const noexcept;

    void setDecimals(int decimals);
    [[nodiscard]] int decimals() const noexcept;

    // The factor between the value a surface stores and the number this field shows: 100 for a
    // 0..1 factor shown as a percentage. value() is the shown number; storedValue() divides by it.
    void setDisplayScale(double scale);
    [[nodiscard]] double displayScale() const noexcept { return displayScale_; }
    [[nodiscard]] double storedValue() const noexcept { return value() / displayScale_; }

    [[nodiscard]] double value() const noexcept;
    // Projection, never an edit: a cell with a live gesture refuses it, because the gesture owns
    // the value until it ends (ADR 0017) and a surface repainting the cell from the document
    // underneath a live scrub would fight the pointer. A projection clears the mixed state.
    void setValue(double value);

    // Integer mode: int64 storage, whole steps and an exact parse (no value above 2^53 is ever
    // rounded through a double). value() and valueChanged() still report the number as a double
    // for surfaces that only read; integerValue() and integerValueChanged() are exact.
    void setIntegerMode(bool integer);
    [[nodiscard]] bool isIntegerMode() const noexcept { return integer_; }
    void setIntegerRange(std::int64_t minimum, std::int64_t maximum);
    [[nodiscard]] std::int64_t integerMinimum() const noexcept { return integerMinimum_; }
    [[nodiscard]] std::int64_t integerMaximum() const noexcept { return integerMaximum_; }
    [[nodiscard]] std::int64_t integerValue() const noexcept { return integerValue_; }
    void setIntegerValue(std::int64_t value);

    // The mixed state of a field that reflects several objects whose values differ: an em dash at
    // rest and an empty editor. Typing a value writes it to all of them; an empty entry changes
    // nothing. Cleared by setValue().
    void setMixed(bool mixed);
    [[nodiscard]] bool isMixed() const noexcept { return mixed_; }

    // The value a reset returns to. While the field differs from it, a small Accent dot marks the
    // cell's top-right corner; a double-click on the label or Reset in the field's context menu
    // (which also lists the widget's own actions()) resets it as one complete edit.
    void setDefaultValue(double value);
    void clearDefaultValue();
    [[nodiscard]] std::optional<double> defaultValue() const noexcept { return default_; }
    [[nodiscard]] bool isNonDefault() const noexcept;
    void setResetText(const QString& text);
    void resetToDefault();

    void stepBy(int steps);

    // The number at the configured precision, without the unit.
    [[nodiscard]] QString displayedValue() const;
    // What the resting cell actually paints at its current width: the number, then a space and the
    // unit when the unit is shown. A value too wide for its column drops the unit first, then
    // decimal places, then falls back to scientific notation with fewer significant digits, and
    // finally to an ellipsis -- digits are never clipped. The exact value stays in the editor and
    // in the tooltip, and a rounded display is underlined (isDisplayRounded()).
    [[nodiscard]] QString paintedText() const;
    [[nodiscard]] bool isDisplayRounded() const;
    // The exact number as the editor is seeded with it: the shortest text that reads back to the
    // same double (or the exact integer).
    [[nodiscard]] QString editText() const;

    [[nodiscard]] QRectF labelRect() const;
    // The cell is the WHOLE height of the control and the whole width left of the label column: it
    // reserves no focus-ring strip, because its focus affordance is its own single border drawn on
    // that very edge.
    [[nodiscard]] QRectF cellRect() const;
    // Where the number's glyphs live: the cell inset by its own horizontal padding. The painter and
    // the inline editor resolve the number's x through this one rectangle, which is why entering
    // edit does not move the value.
    [[nodiscard]] QRectF cellTextRect() const;

    // What one pixel of horizontal drag is worth, as a multiple of singleStep(): 1 normally, 10
    // with Shift, a tenth with Ctrl. Shift wins when both are held.
    [[nodiscard]] static double scrubScale(Qt::KeyboardModifiers modifiers) noexcept;

    // True between the moment a press passes the drag threshold and its release.
    [[nodiscard]] bool isScrubbing() const noexcept;

    // True while the cell is a text field. The line edit exists for the control's whole life and
    // is simply hidden when not editing.
    [[nodiscard]] bool isEditing() const noexcept;
    [[nodiscard]] QLineEdit* lineEdit() const noexcept;
    [[nodiscard]] KInlineEdit* inlineEdit() const noexcept { return inline_; }
    // The reason the editor currently refuses its text; empty when it refuses nothing.
    [[nodiscard]] QString refusal() const;
    [[nodiscard]] bool hasLiveEdit() const noexcept { return interactionActive_; }
    // Programmatic cancellation (the session dropped the edit, the field was disabled): the value
    // the gesture started from comes back and editCancelled() is emitted. Never a key.
    void cancelEdit();

    [[nodiscard]] State visualState() const;

    // The single border the cell paints, by the kit-wide focus/hover rule
    // (kit::borderForInteraction). Editing counts as active, exactly as focus does. A refused
    // entry is Error.
    [[nodiscard]] Color borderToken() const;

    // borderToken() resolved to the color actually stroked: transparent at rest unless the field
    // is outlined at rest.
    [[nodiscard]] QColor cellBorderColor() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    // Every change of the held value, the live ones a scrub produces included. It says what the
    // CELL now shows, which is not the same question as what should be written down -- see the
    // gesture boundary below.
    void valueChanged(double value);
    void integerValueChanged(qint64 value);
    // One complete edit: exactly one of editFinished()/editCancelled() follows editStarted(). A
    // scrub, a typed entry, a held key, a wheel burst and a reset are each one edit.
    void editStarted();
    void editFinished();
    void editCancelled();

    // The scrub gesture's own boundary (ADR 0017). scrubStarted() is emitted once, when the press
    // passes the drag threshold; exactly one of scrubFinished()/scrubCancelled() follows. Losing
    // the pointer grab or the window mid-scrub finishes it.
    void scrubStarted();
    void scrubFinished();
    void scrubCancelled();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct RestingText {
        QString value;
        bool unit = false;
        bool rounded = false;
    };
    [[nodiscard]] RestingText restingText() const;
    [[nodiscard]] QString numberText(int decimals) const;
    [[nodiscard]] int labelColumnWidth() const;
    [[nodiscard]] QString exactToolTip() const;
    // Parses and range-checks typed text. Empty when the text is acceptable.
    [[nodiscard]] QString refusalFor(const QString& text) const;
    bool applyTyped(const QString& text);
    void commitValue(double value);
    void commitInteger(std::int64_t value);
    void stepFrom(double base, std::int64_t integerBase, double steps);
    void beginEdit(const QString& seed = {});
    void endEdit(bool keep);
    void layOutEditor();
    void beginInteraction();
    void finishInteraction(bool keep);
    void finishScrub();
    void wrapPointer(const QPointF& global);
    void stepperRepeat();

    bool compact_ = false;
    bool outlinedAtRest_ = false;
    bool stepper_ = false;
    QString label_;
    QString unit_;
    QString specialValueText_;
    QString resetText_;
    double minimum_ = 0.0;
    double maximum_ = 100.0;
    std::optional<double> softMinimum_;
    std::optional<double> softMaximum_;
    double step_ = 1.0;
    double value_ = 0.0;
    double displayScale_ = 1.0;
    int decimals_ = 2;
    bool integer_ = false;
    std::int64_t integerMinimum_ = 0;
    std::int64_t integerMaximum_ = 100;
    std::int64_t integerValue_ = 0;
    bool mixed_ = false;
    std::optional<double> default_;
    bool hovered_ = false;

    // The scrub gesture. `pressed_` covers the whole press; `scrubbing_` turns on only once the
    // pointer has travelled past the platform drag threshold, which is exactly what separates a
    // scrub from the click that opens the text editor. Each segment of the scrub -- the first one,
    // and one more each time the modifiers change -- is measured from its own start, so a segment
    // that wanders back lands exactly on its base.
    bool pressed_ = false;
    bool scrubbing_ = false;
    bool firstSegment_ = true;
    QPointF pressPosition_;
    double segmentStartX_ = 0.0;
    double segmentBase_ = 0.0;
    std::int64_t segmentIntegerBase_ = 0;
    double segmentScale_ = 1.0;
    // Added to every pointer x while a scrub wraps the pointer around the screen.
    double wrapOffset_ = 0.0;

    KTextField* editor_ = nullptr;
    KInlineEdit* inline_ = nullptr;
    bool editing_ = false;
    bool interactionActive_ = false;
    bool stepping_ = false;
    bool keyStepping_ = false;
    int stepperDirection_ = 0;
    QTimer* stepperTimer_ = nullptr;
    QTimer* wheelIdle_ = nullptr;
    int wheelRemainder_ = 0;
    bool wheelGesture_ = false;
    double interactionBase_ = 0;
    std::int64_t interactionIntegerBase_ = 0;
};

} // namespace bloom::ui::kit
