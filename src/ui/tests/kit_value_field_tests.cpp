#include <bloom/ui/kit/fonts.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QApplication>
#include <QColor>
#include <QFontInfo>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPalette>
#include <QRegion>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <source_location>
#include <string>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using namespace bloom::ui;

void pressAt(QWidget& widget, const QPointF& point,
             const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent press(QEvent::MouseButtonPress, point, widget.mapToGlobal(point), Qt::LeftButton,
                      Qt::LeftButton, modifiers);
    QCoreApplication::sendEvent(&widget, &press);
    QCoreApplication::processEvents();
}

void moveTo(QWidget& widget, const QPointF& point,
            const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent move(QEvent::MouseMove, point, widget.mapToGlobal(point), Qt::NoButton,
                     Qt::LeftButton, modifiers);
    QCoreApplication::sendEvent(&widget, &move);
    QCoreApplication::processEvents();
}

void releaseAt(QWidget& widget, const QPointF& point,
               const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent release(QEvent::MouseButtonRelease, point, widget.mapToGlobal(point),
                        Qt::LeftButton, Qt::NoButton, modifiers);
    QCoreApplication::sendEvent(&widget, &release);
    QCoreApplication::processEvents();
}

// A press, a horizontal drag well past the platform drag threshold, and a release.
void scrub(kit::KValueField& field, const int pixels,
           const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const QPointF start(field.cellRect().center());
    pressAt(field, start, modifiers);
    moveTo(field, start + QPointF(static_cast<qreal>(pixels), 0.0), modifiers);
    releaseAt(field, start + QPointF(static_cast<qreal>(pixels), 0.0), modifiers);
}

void testTheCellCarriesAMonospacedNumberAndAUnit(Expectations& expectations) {
    kit::KValueField field;
    field.setLabel(QStringLiteral("Opacity"));
    field.setUnit(QStringLiteral("%"));
    field.setRange(0.0, 100.0);
    field.setDecimals(1);
    field.setValue(42.5);
    field.resize(field.sizeHint());
    QCoreApplication::processEvents();

    expectations.expect(field.label() == QStringLiteral("Opacity"), "the label survives");
    expectations.expect(field.unit() == QStringLiteral("%"), "the unit survives");
    expectations.expect(field.displayedValue() == QStringLiteral("42.5"),
                        "the number prints at the configured precision, with no unit mixed in");

    field.setDecimals(3);
    expectations.expect(field.displayedValue() == QStringLiteral("42.500"),
                        "precision is the field's, not the value's");

    // Every numeric surface in Bloom uses the monospaced Value role, so a column of them stays
    // aligned and a changing digit does not reflow the text beside it.
    expectations.expect(QFontInfo(kit::font(kit::TypeRole::Value)).fixedPitch(),
                        "the value role really is a fixed-pitch face");
    expectations.expect(
        QFontInfo(kit::font(kit::TypeRole::Value)).family().startsWith(kit::monospaceFontFamily()),
        "and it is the bundled monospaced family");
}

void testSpecialMinimumTextKeepsNumericEditing(Expectations& expectations) {
    kit::KValueField field;
    field.setRange(0, 256);
    field.setUnit(QStringLiteral("GiB"));
    field.setSpecialValueText(QStringLiteral("Automatic"));
    field.resize(field.sizeHint());
    field.show();
    QCoreApplication::processEvents();
    expectations.expect(field.value() == 0 && field.displayedValue() == QStringLiteral("Automatic"),
                        "special minimum text leaves the numeric value intact");
    pressAt(field, field.cellRect().center());
    releaseAt(field, field.cellRect().center());
    expectations.expect(field.isEditing() && field.lineEdit()->text() == QStringLiteral("0"),
                        "the special minimum still edits as a number");
    field.lineEdit()->setText(QStringLiteral("8"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(field.lineEdit(), &enter);
    expectations.expect(!field.isEditing() && field.value() == 8 &&
                            field.displayedValue() == QStringLiteral("8.00"),
                        "numeric editing leaves Automatic for the selected budget");
    field.setValue(0);
    field.setSpecialValueText({});
    expectations.expect(field.displayedValue() == QStringLiteral("0.00"),
                        "clearing special text restores the existing numeric presentation");
}

// Owner report (Properties > Transform): Position Y painted "Y <.22 px", its number cut at the
// cell's left edge while X's shorter "960 px" fit. A resting value must fit its column.
void testANarrowCompactCellFitsItsValue(Expectations& expectations) {
    kit::KValueField field;
    field.setCompact(true);
    field.setLabel(QStringLiteral("Y"));
    field.setUnit(QStringLiteral("px"));
    field.setRange(-1.0e9, 1.0e9);
    field.setDecimals(1);
    field.resize(kit::px(kit::Size::PropertiesFieldWidth), kit::px(kit::Size::Control));
    const QFontMetrics metrics(kit::font(kit::TypeRole::Value));
    const auto fitsColumn = [&] {
        const auto text = field.paintedText();
        return metrics.horizontalAdvance(text) <= field.cellTextRect().width();
    };
    field.setValue(9.5);
    expectations.expect(field.paintedText() == QStringLiteral("9.5 px"),
                        "a short value keeps its unit and its configured places, got " +
                            field.paintedText().toStdString());
    field.setValue(1540.22);
    expectations.expect(fitsColumn() && field.paintedText().startsWith(QStringLiteral("154")),
                        "a long value drops the unit, then decimals, and keeps its leading digits");
    expectations.expect(field.displayedValue() == QStringLiteral("1540.2"),
                        "the field's value text keeps its configured places");
    field.setValue(42.5);
    expectations.expect(field.paintedText() == QStringLiteral("42.5 px") &&
                            !field.isDisplayRounded(),
                        "a value that fits shows its unit again");
    // Compact cells no longer trim trailing zeroes, so an X/Y/Z column keeps its decimal points
    // aligned.
    field.setValue(3.0);
    expectations.expect(field.displayedValue() == QStringLiteral("3.0"),
                        "a whole number keeps its configured places in a compact cell");
}

void testFiniteExtremesStayExactAndBounded(Expectations& expectations) {
    kit::KValueField field;
    field.setRange(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max());
    field.setValue(std::numeric_limits<double>::max());
    expectations.expect(field.value() == std::numeric_limits<double>::max() &&
                            field.displayedValue().size() < 32 && field.sizeHint().width() < 600,
                        "finite extremes stay exact with bounded scientific presentation");
    field.resize(field.sizeHint());
    field.show();
    QCoreApplication::processEvents();
    field.setValue(1.2345678901234567);
    const auto exact = field.value();
    pressAt(field, field.cellRect().center());
    releaseAt(field, field.cellRect().center());
    expectations.expect(field.lineEdit()->text().toDouble() == exact,
                        "opening a number editor retains full round-trip precision");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(field.lineEdit(), &enter);
    expectations.expect(field.value() == exact, "an unchanged Enter cannot round authored data");
    field.setValue(std::numeric_limits<double>::denorm_min());
    expectations.expect(field.displayedValue().toDouble() ==
                            std::numeric_limits<double>::denorm_min(),
                        "a finite subnormal is not presented as zero");
}

void testTheFieldDoesNotResizeAsDigitsChange(Expectations& expectations) {
    kit::KValueField field;
    field.setRange(0.0, 1000.0);
    field.setDecimals(2);
    field.setValue(0.0);
    const QSize narrowValue = field.sizeHint();
    field.setValue(999.99);
    expectations.expect(field.sizeHint() == narrowValue,
                        "the field is sized for the widest number its range can produce, so it "
                        "does not resize as digits change");
}

// Task S1, item 1. The cell is the whole control: no reserved strip above or below it, and the
// number does not move when the cell becomes a text field.
//
// Asserted off a real render rather than off the geometry that produced it, because the question is
// where the GLYPHS land -- a text margin that the line edit then lays out differently would pass
// any geometry-only assertion.
//
// Two design pixels of tolerance, and exactly why: the cell's padding and the unit's column reach
// the editor as text margins whose arithmetic puts its right-aligned run's right edge on the
// painter's own, but QLineEdit then lays that run out through QTextLine with two +1 fudges of its
// own (its natural text width and its right-alignment scroll offset), neither of which any public
// API exposes or lets a caller cancel. The contract this pins is therefore "the number does not
// move", not "the two text engines agree to the pixel".
[[nodiscard]] std::optional<int> firstGlyphColumn(kit::KValueField& field) {
    QImage canvas(field.size(), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    field.render(&canvas, QPoint(), QRegion(), QWidget::DrawChildren);
    const QRectF cell = field.cellRect();
    const QRectF text = field.cellTextRect();
    // The cell's interior only: its own hairline border is inked too, and so is the label column.
    const int top = static_cast<int>(cell.top()) + 2;
    const int bottom = static_cast<int>(cell.bottom()) - 2;
    const QColor ground = kit::color(kit::Color::Field);
    for (int x = static_cast<int>(text.left());
         x <= static_cast<int>(text.right()) && x < canvas.width(); ++x) {
        for (int y = top; y <= bottom && y < canvas.height(); ++y) {
            const QColor pixel = canvas.pixelColor(x, y);
            if (pixel != ground && pixel.alpha() != 0) {
                return x;
            }
        }
    }
    return std::nullopt;
}

void testTheCellIsTheWholeControlAndEnteringEditDoesNotMoveTheValue(Expectations& expectations) {
    kit::KValueField field;
    field.setUnit(QStringLiteral("%"));
    field.setRange(0.0, 100.0);
    field.setDecimals(1);
    field.setValue(42.5);
    field.resize(field.sizeHint());
    field.show();
    QCoreApplication::processEvents();

    expectations.expect(field.sizeHint().height() == kit::px(kit::Size::Control),
                        "the field's height is exactly the control height -- no focus-ring strip "
                        "is reserved in it, got " +
                            std::to_string(field.sizeHint().height()));
    expectations.expect(
        field.cellRect().top() == kit::px(kit::Spacing::ControlInset) &&
            field.cellRect().height() ==
                static_cast<qreal>(field.height() - 2 * kit::px(kit::Spacing::ControlInset)),
        "and the cell spans the control's whole height, so nothing is left over to "
        "read as a darker band above or below it");
    expectations.expect(field.cellRect().right() ==
                            static_cast<qreal>(field.width() - kit::px(kit::Spacing::ControlInset)),
                        "the cell also reaches the control's right edge");
    expectations.expect(
        field.cellTextRect().left() == field.cellRect().left() + kit::px(kit::Spacing::S) &&
            field.cellTextRect().right() == field.cellRect().right() - kit::px(kit::Spacing::S),
        "the text rectangle is the cell inset by exactly the cell's own padding");

    const auto painted = firstGlyphColumn(field);
    expectations.expect(painted.has_value(), "the resting cell paints its number");

    const QPointF centre(field.cellRect().center());
    pressAt(field, centre);
    releaseAt(field, centre);
    expectations.expect(field.isEditing(), "a click with no travel enters text edit");
    expectations.expect(field.lineEdit()->geometry() == field.cellRect().toRect(),
                        "the editor takes exactly the cell rectangle, so no second background or "
                        "second set of corners appears inside it");
    expectations.expect(!field.lineEdit()->hasFrame(), "and it has no frame of its own");
    expectations.expect(field.lineEdit()->palette().color(QPalette::Base).alpha() == 0,
                        "its background is transparent: the cell behind it is the only fill");
    expectations.expect(field.borderToken() == kit::Color::Accent,
                        "the one border the editing cell shows is the Accent hairline");
    field.lineEdit()->deselect();
    QCoreApplication::processEvents();
    const auto edited = firstGlyphColumn(field);
    expectations.expect(edited.has_value(), "the editing cell still shows a number");
    if (painted.has_value() && edited.has_value()) {
        // Tolerance: QLineEdit lays the run out through QTextLine with two +1 fudges no public API
        // cancels, and a different rasterizer (CI's runner fonts) widens that by another pixel; a
        // shift this small is invisible, while a real layout mismatch is a whole padding step.
        expectations.expect(std::abs(*painted - *edited) <= 4,
                            "the number's x is identical before and after entering edit (" +
                                std::to_string(*painted) + " vs " + std::to_string(*edited) + ')');
    }
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(field.lineEdit(), &escape);
    QCoreApplication::processEvents();
}

void testGeometryPlacesTheLabelAndCell(Expectations& expectations) {
    kit::KValueField field;
    field.setLabel(QStringLiteral("Opacity"));
    field.resize(field.sizeHint());
    QCoreApplication::processEvents();

    expectations.expect(field.labelRect().isValid() && field.labelRect().left() == 0.0,
                        "the label claims the leading column");
    expectations.expect(field.cellRect().left() > field.labelRect().right(),
                        "the cell begins after the label, not on top of it");
    expectations.expect(field.cursor().shape() == Qt::SizeHorCursor,
                        "the control advertises the scrub with a horizontal-resize cursor");

    kit::KValueField unlabelled;
    unlabelled.resize(unlabelled.sizeHint());
    expectations.expect(unlabelled.labelRect().isNull(),
                        "a field with no label claims no label column");
    expectations.expect(unlabelled.cellRect().left() == kit::px(kit::Spacing::ControlInset),
                        "and its cell starts at the control's edge");
}

void testKeysStepTheValue(Expectations& expectations) {
    kit::KValueField field;
    field.setRange(0.0, 100.0);
    field.setSingleStep(0.5);
    field.setValue(10.0);
    field.resize(field.sizeHint());
    QCoreApplication::processEvents();

    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QCoreApplication::sendEvent(&field, &up);
    expectations.expect(field.value() == 10.5, "the up arrow steps too");
    QKeyEvent pageUp(QEvent::KeyPress, Qt::Key_PageUp, Qt::NoModifier);
    QCoreApplication::sendEvent(&field, &pageUp);
    expectations.expect(field.value() == 15.5, "a page key steps ten at a time");

    field.setValue(100.0);
    field.stepBy(1);
    expectations.expect(field.value() == 100.0, "stepping past the maximum clamps");
    field.setValue(0.0);
    field.stepBy(-1);
    expectations.expect(field.value() == 0.0, "stepping past the minimum clamps");
}

void testTheWheelIsIgnoredUntilTheFieldIsFocused(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    field.setRange(0.0, 100.0);
    field.setSingleStep(1.0);
    field.setValue(50.0);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();
    field.clearFocus();
    QCoreApplication::processEvents();

    const auto sendWheel = [&field] {
        QWheelEvent wheel(QPointF(field.width() / 2.0, field.height() / 2.0),
                          field.mapToGlobal(QPointF(field.width() / 2.0, field.height() / 2.0)),
                          QPoint(0, 0), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&field, &wheel);
        QCoreApplication::processEvents();
    };

    sendWheel();
    expectations.expect(field.value() == 50.0,
                        "a wheel over an unfocused field belongs to whatever is scrolling behind "
                        "it, and must not silently change a parameter");

    field.setFocus(Qt::MouseFocusReason);
    QCoreApplication::processEvents();
    sendWheel();
    expectations.expect(field.value() == 50.0,
                        "a focused field at rest still leaves a plain wheel to the panel");
}

void testTheStateMachineAndDisabledField(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    field.setRange(0.0, 100.0);
    field.setValue(10.0);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();

    field.clearFocus();
    QCoreApplication::processEvents();
    expectations.expect(field.visualState() == kit::State::Normal,
                        "an unfocused, unhovered field rests");
    field.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(field.visualState() == kit::State::Focused, "focus is its own state");

    field.setEnabled(false);
    expectations.expect(field.visualState() == kit::State::Disabled, "disabled outranks the rest");
    scrub(field, 20);
    expectations.expect(field.value() == 10.0, "a disabled field does not scrub at all");
    expectations.expect(!field.isEditing(), "and a click on it opens no editor");
    field.setEnabled(true);
}

// Task F1, item F4: After Effects' scrub. One pixel is one step, Shift is ten of them, Ctrl is a
// tenth, and the displacement is measured from the press so a drag that returns lands exactly
// where it started.
void testDraggingScrubsTheValue(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    field.setRange(-10'000.0, 10'000.0);
    field.setDecimals(2);
    field.setSingleStep(1.0);
    field.setValue(0.0);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();

    // The platform drag threshold is a dead zone: crossing it never jumps the value by the
    // threshold's worth of steps (lane B, finding 2).
    const int threshold = QApplication::startDragDistance();
    scrub(field, 40);
    expectations.expect(field.value() == 40.0 - threshold,
                        "forty pixels to the right is forty steps less the dead zone, got " +
                            std::to_string(field.value()));
    expectations.expect(!field.isScrubbing(), "the gesture ends on release");
    expectations.expect(!field.isEditing(), "a drag does not open the text editor");

    field.setValue(0.0);
    scrub(field, -25);
    expectations.expect(field.value() == -25.0 + threshold, "dragging left subtracts steps");

    field.setValue(0.0);
    field.setSingleStep(0.5);
    scrub(field, 10 + threshold);
    expectations.expect(field.value() == 5.0, "one pixel is one singleStep(), not one unit");

    field.setSingleStep(1.0);
    field.setValue(0.0);
    scrub(field, 10 + threshold, Qt::ShiftModifier);
    expectations.expect(field.value() == 100.0, "Shift scrubs ten times as fast");

    field.setValue(0.0);
    scrub(field, 100 + threshold, Qt::ControlModifier);
    expectations.expect(std::abs(field.value() - 10.0) < 1e-9, "Ctrl scrubs a tenth as fast");

    field.setValue(0.0);
    scrub(field, 10 + threshold, Qt::ShiftModifier | Qt::ControlModifier);
    expectations.expect(field.value() == 100.0, "Shift wins when both modifiers are held");
    expectations.expect(kit::KValueField::scrubScale(Qt::ShiftModifier) == 10.0 &&
                            kit::KValueField::scrubScale(Qt::ControlModifier) == 0.1 &&
                            kit::KValueField::scrubScale(Qt::NoModifier) == 1.0,
                        "the scale rule is stated once and is what the gesture uses");

    // Measured from the press: out and back lands exactly where it started.
    field.setValue(7.0);
    const QPointF start(field.cellRect().center());
    pressAt(field, start);
    moveTo(field, start + QPointF(60.0, 0.0));
    moveTo(field, start);
    releaseAt(field, start);
    expectations.expect(field.value() == 7.0,
                        "a scrub that wanders back to the press lands on the original value");

    // A drag shorter than the platform threshold is not a scrub at all.
    field.setValue(3.0);
    const int under = std::max(0, QApplication::startDragDistance() - 1);
    pressAt(field, start);
    moveTo(field, start + QPointF(static_cast<qreal>(under), 0.0));
    expectations.expect(!field.isScrubbing(), "travel inside the drag threshold is not a scrub");
    expectations.expect(field.value() == 3.0, "and it changes nothing");
    releaseAt(field, start + QPointF(static_cast<qreal>(under), 0.0));
}

void testAClickEntersTextEditAndEscapeCommits(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    field.setRange(0.0, 100.0);
    field.setDecimals(1);
    field.setValue(20.0);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();

    const auto key = [&field](const Qt::Key code) {
        QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier);
        QCoreApplication::sendEvent(field.lineEdit(), &press);
        QCoreApplication::processEvents();
    };
    const auto open = [&field] {
        const QPointF centre(field.cellRect().center());
        pressAt(field, centre);
        releaseAt(field, centre);
    };

    const QPointF centre(field.cellRect().center());
    pressAt(field, centre);
    expectations.expect(!field.isEditing(), "the editor waits for the release, not the press");
    releaseAt(field, centre);
    expectations.expect(field.isEditing(), "a click with no travel enters text edit");
    expectations.expect(field.lineEdit()->isVisible(), "the text editor is actually shown");
    expectations.expect(field.displayedValue() == QStringLiteral("20.0") &&
                            field.lineEdit()->selectedText() == field.lineEdit()->text() &&
                            field.lineEdit()->text() == QStringLiteral("20"),
                        "entering text edit selects the exact shortest value while the resting "
                        "display keeps its configured precision, got '" +
                            field.lineEdit()->selectedText().toStdString() + '\'');
    expectations.expect(field.borderToken() == kit::Color::Accent,
                        "an editing cell is active, so it wears the accent border");

    // Owner decision 2026-10-29: Escape confirms everywhere. Undo is the discard.
    QSignalSpy changed(&field, &kit::KValueField::valueChanged);
    QSignalSpy finished(&field, &kit::KValueField::editFinished);
    QSignalSpy cancelled(&field, &kit::KValueField::editCancelled);
    field.lineEdit()->setText(QStringLiteral("42.5"));
    key(Qt::Key_Escape);
    expectations.expect(!field.isEditing(), "Escape leaves text edit");
    expectations.expect(field.value() == 42.5, "Escape commits what was typed");
    expectations.expect(changed.count() == 1 && finished.count() == 1 && cancelled.count() == 0,
                        "as one finished edit, never a cancel");

    open();
    field.lineEdit()->setText(QStringLiteral("13.5"));
    key(Qt::Key_Return);
    expectations.expect(!field.isEditing() && field.value() == 13.5, "Return commits too");

    // Refused text never silently reverts: Return keeps the editor open with an Error border and
    // the reason; Escape then closes it on the last valid value.
    open();
    field.lineEdit()->setText(QStringLiteral("not a number"));
    key(Qt::Key_Return);
    expectations.expect(field.isEditing() && field.borderToken() == kit::Color::Error &&
                            !field.refusal().isEmpty() &&
                            field.lineEdit()->property("kitError").toBool(),
                        "unparseable text keeps the editor open with an Error border and a reason");
    expectations.expect(field.value() == 13.5, "and never becomes zero");
    key(Qt::Key_Escape);
    expectations.expect(!field.isEditing() && field.value() == 13.5 && field.refusal().isEmpty(),
                        "Escape on refused text closes the editor and keeps the last valid value");

    open();
    field.lineEdit()->setText(QStringLiteral("101"));
    key(Qt::Key_Return);
    expectations.expect(field.isEditing() && field.value() == 13.5 &&
                            field.refusal().contains(QStringLiteral("100")),
                        "an out-of-range typed value is refused with the range, never clamped");
    key(Qt::Key_Tab);
    expectations.expect(field.isEditing(), "Tab with refused text keeps the editor open as well");
    field.lineEdit()->setText(QStringLiteral("50%"));
    key(Qt::Key_Return);
    expectations.expect(field.isEditing() && field.refusal().contains(QStringLiteral("%")),
                        "a unit the field does not take is refused by name");
    field.lineEdit()->setText(QStringLiteral("(10 + 5) * 2"));
    key(Qt::Key_Return);
    expectations.expect(!field.isEditing() && field.value() == 30.0,
                        "arithmetic is evaluated, got " + std::to_string(field.value()));
    open();
    field.lineEdit()->setText(QStringLiteral("+= 2,5"));
    key(Qt::Key_Return);
    expectations.expect(field.value() == 32.5,
                        "a relative entry with a comma decimal applies to the value the edit "
                        "started from");
    expectations.expect(cancelled.count() == 0, "no key in the editor ever cancels");
}

} // namespace

void testAFocusedFieldClaimsEditingKeyOverrides(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    field.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    QKeyEvent upOverride(QEvent::ShortcutOverride, Qt::Key_Up, Qt::NoModifier);
    upOverride.ignore();
    QCoreApplication::sendEvent(&field, &upOverride);
    expectations.expect(upOverride.isAccepted(), "a focused field claims the keys that step it");
    // Lane B, finding 4: Left/Right/Home/End step the transport. A field at rest has no use for
    // them, so it no longer swallows them; only an open editor (a QLineEdit) claims them.
    for (const auto code : {Qt::Key_Left, Qt::Key_Right, Qt::Key_Home, Qt::Key_End}) {
        QKeyEvent transport(QEvent::ShortcutOverride, code, Qt::NoModifier);
        transport.ignore();
        QCoreApplication::sendEvent(&field, &transport);
        expectations.expect(!transport.isAccepted(),
                            "a field at rest leaves the transport keys to the window");
    }

    field.clearFocus();
    QKeyEvent unfocusedOverride(QEvent::ShortcutOverride, Qt::Key_Left, Qt::NoModifier);
    unfocusedOverride.ignore();
    QCoreApplication::sendEvent(&field, &unfocusedOverride);
    expectations.expect(!unfocusedOverride.isAccepted(),
                        "an unfocused field leaves window shortcuts alone");
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testTheCellCarriesAMonospacedNumberAndAUnit(expectations);
    testSpecialMinimumTextKeepsNumericEditing(expectations);
    testFiniteExtremesStayExactAndBounded(expectations);
    testTheFieldDoesNotResizeAsDigitsChange(expectations);
    testGeometryPlacesTheLabelAndCell(expectations);
    testANarrowCompactCellFitsItsValue(expectations);
    testTheCellIsTheWholeControlAndEnteringEditDoesNotMoveTheValue(expectations);
    testKeysStepTheValue(expectations);
    testTheWheelIsIgnoredUntilTheFieldIsFocused(expectations);
    testTheStateMachineAndDisabledField(expectations);
    testAFocusedFieldClaimsEditingKeyOverrides(expectations);
    testDraggingScrubsTheValue(expectations);
    testAClickEntersTextEditAndEscapeCommits(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
