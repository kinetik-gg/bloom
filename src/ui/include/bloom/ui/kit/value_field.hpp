#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QColor>
#include <QPoint>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QWidget>

class QLineEdit;

namespace bloom::ui::kit {

// A labelled numeric cell: the parameter's name, a bordered field carrying the number in the
// monospaced Value role, and an optional unit suffix.
//
// The number is monospaced on purpose. Every numeric, unit, hex, and timecode surface in Bloom
// uses the Value role, so a column of them stays aligned and a digit changing does not reflow the
// text beside it.
//
// Interaction is After Effects' (task F1, item F4). There are no stepper chevrons. Press and drag
// horizontally anywhere on the control -- the label is a scrub handle too -- and the value follows
// the pointer at one step per pixel, ten times that with Shift and a tenth with Ctrl. Press and
// release without travelling past QApplication::startDragDistance() and the cell becomes a text
// field with its contents selected: Enter or Tab commits what was typed, Esc puts the old value
// back. The arrow and page keys still step, and the wheel still steps a focused field.
class KValueField final : public QWidget {
    Q_OBJECT

  public:
    explicit KValueField(QWidget* parent = nullptr);

    void setLabel(const QString& label);
    [[nodiscard]] QString label() const;

    // A short suffix such as "px", "%", or "fps", drawn inside the cell after the number in the
    // muted ink so it reads as a unit rather than as part of the value.
    void setUnit(const QString& unit);
    [[nodiscard]] QString unit() const;

    void setRange(double minimum, double maximum);
    [[nodiscard]] double minimum() const noexcept;
    [[nodiscard]] double maximum() const noexcept;

    void setSingleStep(double step);
    [[nodiscard]] double singleStep() const noexcept;

    void setDecimals(int decimals);
    [[nodiscard]] int decimals() const noexcept;

    [[nodiscard]] double value() const noexcept;
    void setValue(double value);

    void stepBy(int steps);

    // The number exactly as painted, at the configured precision, without the unit.
    [[nodiscard]] QString displayedValue() const;

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

    [[nodiscard]] State visualState() const;

    // The single border the cell paints, by the kit-wide focus/hover rule
    // (kit::borderForInteraction). Editing counts as active, exactly as focus does. The cell is
    // borderless at rest, so a Border result is painted transparent -- see cellBorderColor().
    [[nodiscard]] Color borderToken() const;

    // borderToken() resolved to the color actually stroked: transparent at rest, because this cell
    // reads as a field without an outline until the pointer or focus reaches it.
    [[nodiscard]] QColor cellBorderColor() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void valueChanged(double value);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void commitValue(double value);
    void beginEdit();
    // `keep` commits what was typed; otherwise the value the field had before editing stays.
    void endEdit(bool keep);
    void layOutEditor();

    QString label_;
    QString unit_;
    double minimum_ = 0.0;
    double maximum_ = 100.0;
    double step_ = 1.0;
    double value_ = 0.0;
    int decimals_ = 2;
    bool hovered_ = false;

    // The scrub gesture. `pressed_` covers the whole press; `scrubbing_` turns on only once the
    // pointer has travelled past the platform drag threshold, which is exactly what separates a
    // scrub from the click that opens the text editor.
    bool pressed_ = false;
    bool scrubbing_ = false;
    QPoint pressPosition_;
    double pressValue_ = 0.0;

    QLineEdit* editor_ = nullptr;
    bool editing_ = false;
};

} // namespace bloom::ui::kit
