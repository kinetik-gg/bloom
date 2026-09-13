#include <bloom/ui/kit/fonts.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QApplication>
#include <QColor>
#include <QFontInfo>
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
    expectations.expect(field.cellRect().top() == 0.0 &&
                            field.cellRect().height() == static_cast<qreal>(field.height()),
                        "and the cell spans the control's whole height, so nothing is left over to "
                        "read as a darker band above or below it");
    expectations.expect(field.cellRect().right() == static_cast<qreal>(field.width()),
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
        expectations.expect(std::abs(*painted - *edited) <= 2,
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
    expectations.expect(unlabelled.cellRect().left() == 0.0,
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
    expectations.expect(field.value() == 51.0, "once focused the wheel steps the value");
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

    scrub(field, 40);
    expectations.expect(field.value() == 40.0, "forty pixels to the right is forty steps, got " +
                                                   std::to_string(field.value()));
    expectations.expect(!field.isScrubbing(), "the gesture ends on release");
    expectations.expect(!field.isEditing(), "a drag does not open the text editor");

    field.setValue(0.0);
    scrub(field, -25);
    expectations.expect(field.value() == -25.0, "dragging left subtracts steps");

    field.setValue(0.0);
    field.setSingleStep(0.5);
    scrub(field, 10);
    expectations.expect(field.value() == 5.0, "one pixel is one singleStep(), not one unit");

    field.setSingleStep(1.0);
    field.setValue(0.0);
    scrub(field, 10, Qt::ShiftModifier);
    expectations.expect(field.value() == 100.0, "Shift scrubs ten times as fast");

    field.setValue(0.0);
    scrub(field, 100, Qt::ControlModifier);
    expectations.expect(field.value() == 10.0, "Ctrl scrubs a tenth as fast");

    field.setValue(0.0);
    scrub(field, 10, Qt::ShiftModifier | Qt::ControlModifier);
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

void testAClickEntersTextEditAndEscCancels(Expectations& expectations) {
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

    const QPointF centre(field.cellRect().center());
    pressAt(field, centre);
    expectations.expect(!field.isEditing(), "the editor waits for the release, not the press");
    releaseAt(field, centre);
    expectations.expect(field.isEditing(), "a click with no travel enters text edit");
    expectations.expect(field.lineEdit()->isVisible(), "the text editor is actually shown");
    expectations.expect(field.lineEdit()->selectedText() == QStringLiteral("20.0"),
                        "entering text edit selects the whole number, got '" +
                            field.lineEdit()->selectedText().toStdString() + '\'');
    expectations.expect(field.borderToken() == kit::Color::Accent,
                        "an editing cell is active, so it wears the accent border");

    QSignalSpy changed(&field, &kit::KValueField::valueChanged);
    field.lineEdit()->setText(QStringLiteral("42.5"));
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(field.lineEdit(), &escape);
    QCoreApplication::processEvents();
    expectations.expect(!field.isEditing(), "Esc leaves text edit");
    expectations.expect(field.value() == 20.0, "Esc cancels: the typed value is discarded");
    expectations.expect(changed.count() == 0, "and nothing is committed");

    pressAt(field, centre);
    releaseAt(field, centre);
    expectations.expect(field.isEditing(), "clicking again re-enters text edit");
    field.lineEdit()->setText(QStringLiteral("42.5"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(field.lineEdit(), &enter);
    QCoreApplication::processEvents();
    expectations.expect(!field.isEditing(), "Enter leaves text edit");
    expectations.expect(field.value() == 42.5, "Enter commits what was typed");
    expectations.expect(changed.count() == 1, "committing reports the change exactly once");

    pressAt(field, centre);
    releaseAt(field, centre);
    field.lineEdit()->setText(QStringLiteral("13.5"));
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QCoreApplication::sendEvent(field.lineEdit(), &tab);
    QCoreApplication::processEvents();
    expectations.expect(!field.isEditing(), "Tab leaves text edit");
    expectations.expect(field.value() == 13.5, "Tab commits too");

    pressAt(field, centre);
    releaseAt(field, centre);
    field.lineEdit()->setText(QStringLiteral("not a number"));
    QKeyEvent enterAgain(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(field.lineEdit(), &enterAgain);
    QCoreApplication::processEvents();
    expectations.expect(field.value() == 13.5,
                        "unparseable text is refused the way Esc is, never silently zero");
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
    QKeyEvent leftOverride(QEvent::ShortcutOverride, Qt::Key_Left, Qt::NoModifier);
    QCoreApplication::sendEvent(&field, &leftOverride);
    expectations.expect(leftOverride.isAccepted(),
                        "a focused field claims Left so window shortcuts stay inert");

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
    testTheFieldDoesNotResizeAsDigitsChange(expectations);
    testGeometryPlacesTheLabelAndCell(expectations);
    testTheCellIsTheWholeControlAndEnteringEditDoesNotMoveTheValue(expectations);
    testKeysStepTheValue(expectations);
    testTheWheelIsIgnoredUntilTheFieldIsFocused(expectations);
    testTheStateMachineAndDisabledField(expectations);
    testAFocusedFieldClaimsEditingKeyOverrides(expectations);
    testDraggingScrubsTheValue(expectations);
    testAClickEntersTextEditAndEscCancels(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
