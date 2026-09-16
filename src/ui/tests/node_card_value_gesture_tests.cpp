#include <QElapsedTimer>
// Task FIX2, deliverable 1: the owner's report that "values in nodes parameters are not workable",
// reproduced through the gestures an artist actually performs -- a real press/move/release on the
// view's viewport and real key events on the view, over the production NodeGraphEditor with no
// adapter on NodeGraphicsScene::setSubmit().
//
// Every case below states one gesture and the consequence the document and the card must show. The
// existing node tests reached the same parameters by calling KValueField::setValue() directly,
// which is why none of them could see any of the failures this file pins: the whole distance
// between "the widget's setter works" and "the gesture works" is the pointer and key plumbing
// between the viewport and the hosted control.
#include "node_production_harness.hpp"

#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/color_picker.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QGraphicsProxyWidget>
#include <QLineEdit>
#include <QListView>

#include <array>
#include <limits>
#include <string>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::production_test;

namespace {

// The scene point at a hosted control's own local point. A gesture aims at the CELL, exactly where
// an artist aims, rather than at a card-relative guess that a relayout could invalidate.
[[nodiscard]] QPointF scenePointIn(const QWidget* widget, const QPointF local) {
    const auto* proxy = widget == nullptr ? nullptr : widget->window()->graphicsProxyWidget();
    return proxy == nullptr ? QPointF{} : proxy->mapToScene(widget->mapTo(widget->window(), local));
}

[[nodiscard]] QPointF controlCenter(const QWidget* widget) {
    return widget == nullptr
               ? QPointF{}
               : scenePointIn(widget, QPointF(widget->width() / 2.0, widget->height() / 2.0));
}

void click(App& app, const QWidget* widget) {
    const QPointF point = controlCenter(widget);
    app.press(point);
    app.release(point);
    QCoreApplication::processEvents();
}

// One scrub: press on the cell, travel `dx` widget pixels to the right in several steps, release.
void scrub(App& app, const QWidget* widget, const int dx) {
    const QPointF start = controlCenter(widget);
    app.press(start);
    for (int step = 1; step <= 4; ++step) {
        app.move(start + QPointF(dx * step / 4.0, 0.0));
        QCoreApplication::processEvents();
    }
    app.release(start + QPointF(dx, 0.0));
    QCoreApplication::processEvents();
}

void type(App& app, const QString& text) {
    QTest::keyClicks(app.editor.graphView(), text);
    QCoreApplication::processEvents();
}

void key(App& app, const Qt::Key code) {
    QTest::keyClick(app.editor.graphView(), code);
    QCoreApplication::processEvents();
}

[[nodiscard]] kit::KValueField* field(App& app, const document::NodeId node, const QString& name) {
    return qobject_cast<kit::KValueField*>(app.editor.graphScene()->nodeFieldForTest(node, name));
}

// The n-th control of `name` on a card: a Math node carries several "nodeOperandEditor" fields, and
// a gesture that wants the second operand has to be able to say so.
[[nodiscard]] QList<QWidget*> controls(App& app, const document::NodeId node, const QString& name) {
    QList<QWidget*> found;
    auto* card = app.card(node);
    if (card == nullptr) {
        return found;
    }
    for (const auto* child : card->childItems()) {
        const auto* proxy = qgraphicsitem_cast<const QGraphicsProxyWidget*>(child);
        if (proxy != nullptr && proxy->widget() != nullptr) {
            if (proxy->widget()->objectName() == name)
                found.push_back(proxy->widget());
            found.append(proxy->widget()->findChildren<QWidget*>(name));
        }
    }
    return found;
}

[[nodiscard]] std::optional<double> scalarOf(const App& app, const document::NodeId node,
                                             const std::string_view role) {
    const auto* parameter = parameterFor(app, node, role);
    const auto* constant = parameter == nullptr
                               ? nullptr
                               : std::get_if<document::ConstantValueSource>(&parameter->source);
    const auto* held = constant == nullptr ? nullptr : std::get_if<double>(&constant->value);
    return held == nullptr ? std::nullopt : std::optional(*held);
}

[[nodiscard]] std::optional<document::Vec2d> vec2Of(const App& app, const document::NodeId node,
                                                    const std::string_view role) {
    const auto* parameter = parameterFor(app, node, role);
    const auto* constant = parameter == nullptr
                               ? nullptr
                               : std::get_if<document::ConstantValueSource>(&parameter->source);
    const auto* held =
        constant == nullptr ? nullptr : std::get_if<document::Vec2d>(&constant->value);
    return held == nullptr ? std::nullopt : std::optional(*held);
}

[[nodiscard]] std::optional<std::int64_t> integerOf(const App& app, const document::NodeId node,
                                                    const std::string_view role) {
    const auto* parameter = parameterFor(app, node, role);
    const auto* constant = parameter == nullptr
                               ? nullptr
                               : std::get_if<document::ConstantValueSource>(&parameter->source);
    const auto* held = constant == nullptr ? nullptr : std::get_if<std::int64_t>(&constant->value);
    return held == nullptr ? std::nullopt : std::optional(*held);
}

[[nodiscard]] std::optional<core::Color4d> colorOf(const App& app, const document::NodeId node,
                                                   const std::string_view role) {
    const auto* parameter = parameterFor(app, node, role);
    const auto* constant = parameter == nullptr
                               ? nullptr
                               : std::get_if<document::ConstantValueSource>(&parameter->source);
    const auto* held = constant == nullptr ? nullptr : std::get_if<core::Color4d>(&constant->value);
    return held == nullptr ? std::nullopt : std::optional(*held);
}

// --- (a) click a value cell, type a number, Enter ----------------------------------------------
void clickTypeEnterOnALayerCard(App& app, const document::NodeId layer) {
    auto* positionX = field(app, layer, QStringLiteral("nodePositionXEditor"));
    expect(positionX != nullptr, "the Layer card carries a Position X cell");
    if (positionX == nullptr) {
        return;
    }
    click(app, positionX);
    expect(positionX->isEditing(),
           "clicking a Layer card's Position X cell opens its inline editor");
    expect(positionX->lineEdit() != nullptr && positionX->lineEdit()->isVisible(),
           "the inline editor is actually shown on the card");
    type(app, QStringLiteral("250"));
    expect(positionX->lineEdit() != nullptr &&
               positionX->lineEdit()->text() == QStringLiteral("250"),
           "typed digits reach the inline editor rather than the canvas");
    key(app, Qt::Key_Return);
    expect(!positionX->isEditing(), "Enter closes the inline editor");
    const auto stored = vec2Of(app, layer, document::kPositionParameterRole);
    expect(stored.has_value() && closeTo(static_cast<float>(stored->x), 250.0),
           "Enter commits the typed number to the document");
    expect(closeTo(static_cast<float>(positionX->value()), 250.0),
           "the card shows the value the document now holds");
}

// --- (b) drag-scrub the cell -------------------------------------------------------------------
void scrubOnALayerCard(App& app, const document::NodeId layer) {
    auto* opacity = field(app, layer, QStringLiteral("nodeOpacityEditor"));
    expect(opacity != nullptr, "the Layer card carries an Opacity cell");
    if (opacity == nullptr) {
        return;
    }
    const double before = opacity->value();
    const std::size_t historyBefore = app.stack.size();
    const auto documentBefore = scalarOf(app, layer, document::kOpacityParameterRole);
    scrub(app, opacity, -30);
    expect(opacity->value() < before, "scrubbing left lowers the Opacity cell's value");
    const auto stored = scalarOf(app, layer, document::kOpacityParameterRole);
    expect(stored.has_value() && closeTo(static_cast<float>(*stored * 100.0), opacity->value()),
           "the document holds exactly what the scrubbed cell shows");
    const std::size_t landed = app.stack.size() - historyBefore;
    expect(landed == 1, "one scrub gesture lands exactly one undoable command");
    expect(app.session.undo(), "that one command is one undo step");
    expect(scalarOf(app, layer, document::kOpacityParameterRole) == documentBefore,
           "and undoing it puts the whole gesture back");
    expect(app.session.redo(), "redo restores the scrubbed value");

    // A scrub abandoned with Esc puts the cell back and writes nothing at all.
    const double armed = opacity->value();
    const std::size_t armedHistory = app.stack.size();
    const QPointF start = controlCenter(opacity);
    app.press(start);
    app.move(start + QPointF(-40.0, 0.0));
    QCoreApplication::processEvents();
    expect(opacity->isScrubbing(), "the pointer has travelled far enough to be scrubbing");
    expect(opacity->value() < armed, "the cell follows the pointer while the scrub is in flight");
    expect(app.stack.size() == armedHistory,
           "nothing is written to the document while the pointer is moving");
    QTest::keyClick(opacity, Qt::Key_Escape);
    QCoreApplication::processEvents();
    app.release(start + QPointF(-40.0, 0.0));
    QCoreApplication::processEvents();
    expect(closeTo(static_cast<float>(opacity->value()), armed),
           "Esc puts back exactly the value the press started from");
    expect(app.stack.size() == armedHistory, "an abandoned scrub lands no command at all");
}

// --- (c) the operand dropdown and the color chip ------------------------------------------------
void operandDropdownCommits(App& app, const document::NodeId math) {
    const auto selectors = controls(app, math, QStringLiteral("nodeOperandSelector"));
    expect(!selectors.isEmpty(), "the Math card carries its operation dropdown");
    if (selectors.isEmpty()) {
        return;
    }
    auto* dropdown = qobject_cast<kit::KDropdown*>(selectors.front());
    expect(dropdown != nullptr, "the operation control is a KDropdown");
    if (dropdown == nullptr) {
        return;
    }
    const auto storedBefore = integerOf(app, math, document::kOperationParameterRole);
    const int target = dropdown->currentIndex() == 0 ? 1 : 0;
    click(app, dropdown);
    expect(dropdown->isPopupVisible(),
           "clicking the Math card's operation dropdown opens its popup");
    if (!dropdown->isPopupVisible()) {
        return;
    }
    auto* view = dropdown->popupView();
    const QRect row = view->visualRect(view->model()->index(target, 0));
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(row.center()),
                        view->viewport()->mapToGlobal(row.center()), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(view->viewport(), &release);
    QCoreApplication::processEvents();
    const auto storedAfter = integerOf(app, math, document::kOperationParameterRole);
    expect(storedAfter.has_value() && storedAfter != storedBefore,
           "picking an operation in the popup commits it to the document");
    expect(dropdown->currentIndex() == target, "the card shows the operation it committed");
}

void colorChipCommits(App& app, const document::NodeId solid) {
    auto* chip = qobject_cast<kit::KColorChip*>(
        app.editor.graphScene()->nodeFieldForTest(solid, QStringLiteral("nodeColorChip")));
    expect(chip != nullptr, "the Solid card carries its color chip");
    if (chip == nullptr) {
        return;
    }
    QElapsedTimer preparation;
    preparation.start();
    while (!app.session.colorConverter() && preparation.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QTest::qWait(1);
    }
    QTest::qWait(20);
    click(app, chip);
    expect(chip->isPickerOpen(), "clicking the Solid card's color chip opens its picker");
    // The picker is the chip's own popup; choosing a colour in it is what the chip reports.
    chip->picker()->setColor(kit::KColor::fromRgba(0.25F, 0.5F, 0.75F, 1.0F));
    QCoreApplication::processEvents();
    const auto stored = colorOf(app, solid, document::kSolidColorParameterRole);
    expect(stored.has_value() && closeTo(static_cast<float>(stored->red), 0.050876) &&
               closeTo(static_cast<float>(stored->green), 0.214041),
           "choosing a color in the picker writes the Solid card's own parameter");
    expect(chip->color().space == kit::ColorSpace::Reference &&
               closeTo(chip->color()
                           .converted(kit::ColorSpace::Display, app.session.colorConverter())
                           .value_or(kit::KColor{})
                           .red,
                       0.25),
           "and the chip shows what it committed");
    chip->closePicker();
    // Offscreen there is no window manager to reactivate the window under a dismissed popup, so
    // the canvas is reactivated and refocused here rather than the next gesture finding no active
    // window at all. Harness state only -- nothing about the card is being reset.
    app.editor.activateWindow();
    QCoreApplication::processEvents();
    app.editor.graphView()->setFocus();
    QCoreApplication::processEvents();
}

// --- (d) Tab moves between cells ---------------------------------------------------------------
void tabMovesBetweenCells(App& app, const document::NodeId layer) {
    auto* positionX = field(app, layer, QStringLiteral("nodePositionXEditor"));
    auto* positionY = field(app, layer, QStringLiteral("nodePositionYEditor"));
    expect(positionX != nullptr && positionY != nullptr,
           "the Layer card carries both Position cells");
    if (positionX == nullptr || positionY == nullptr) {
        return;
    }
    click(app, positionX);
    type(app, QStringLiteral("40"));
    key(app, Qt::Key_Tab);
    const auto stored = vec2Of(app, layer, document::kPositionParameterRole);
    expect(stored.has_value() && closeTo(static_cast<float>(stored->x), 40.0),
           "Tab commits the cell it leaves");
    expect(positionY->hasFocus(), "Tab moves to the next cell on the same card");
}

// --- (e) Esc cancels ---------------------------------------------------------------------------
void escapeCancels(App& app, const document::NodeId layer) {
    auto* rotation = field(app, layer, QStringLiteral("nodeRotationEditor"));
    expect(rotation != nullptr, "the Layer card carries a Rotation cell");
    if (rotation == nullptr) {
        return;
    }
    const double before = rotation->value();
    click(app, rotation);
    type(app, QStringLiteral("77"));
    key(app, Qt::Key_Escape);
    expect(!rotation->isEditing(), "Esc closes the inline editor");
    expect(closeTo(static_cast<float>(rotation->value()), before),
           "Esc leaves the cell's value exactly as it was");
    const auto stored = scalarOf(app, layer, document::kRotationParameterRole);
    expect(stored.has_value() && closeTo(static_cast<float>(*stored), before),
           "Esc writes nothing to the document");
}

// --- (f) editing a value node's own value drives the downstream parameter ----------------------
void valueNodeDrivesDownstream(App& app, const document::NodeId layer) {
    const auto scalar = app.addThroughSearch(QStringLiteral("Scalar"));
    expect(scalar.has_value(), "Tab -> Scalar -> Enter adds a Scalar value node");
    if (!scalar.has_value()) {
        return;
    }
    auto* from = app.outputSocket(*scalar);
    auto* toOpacity =
        app.namedSocket(layer, QString::fromUtf8(document::kOpacityParameterRole), true);
    expect(from != nullptr && toOpacity != nullptr, "both ends of the driver link exist");
    if (from == nullptr || toOpacity == nullptr) {
        return;
    }
    app.press(from->scenePos());
    app.move(toOpacity->scenePos());
    app.release(toOpacity->scenePos());
    QCoreApplication::processEvents();
    const auto* driver = driverFor(app, layer, document::kOpacityParameterRole);
    expect(driver != nullptr && driver->sourceNodeId == *scalar,
           "the drag binds the Scalar node as the opacity's driver");
    if (driver == nullptr) {
        return;
    }
    const auto editors = controls(app, *scalar, QStringLiteral("nodeOperandEditor"));
    expect(editors.size() == 1, "the Scalar card carries exactly one value cell");
    if (editors.isEmpty()) {
        return;
    }
    auto* value = qobject_cast<kit::KValueField*>(editors.front());
    click(app, value);
    expect(value != nullptr && value->isEditing(),
           "clicking the Scalar card's own value cell opens its editor");
    type(app, QStringLiteral("0.4"));
    key(app, Qt::Key_Return);
    const auto stored = scalarOf(app, *scalar, document::kValueParameterRole);
    expect(stored.has_value() && closeTo(static_cast<float>(*stored), 0.4),
           "the typed number reaches the Scalar node's own parameter");
    const auto composited = composite(app);
    expect(composited.ok, "the graph still composites with the driver in place");
    expect(composited.ok && closeTo(composited.centerPixel[3], 0.4),
           "the driven opacity is what the composited frame carries");
}

// --- The other card kinds the owner edits ------------------------------------------------------
// A Text card's own three rows, a Solid card's colour row, and the literal value nodes' own cells,
// each driven by the SAME click/type/Enter gesture. A per-kind sweep rather than one card, because
// "values are not workable" was reported of the cards in general and each kind builds its rows
// through a different branch of NodeItem::ensureFields().
void typedNumberReaches(App& app, const document::NodeId node, const QString& objectName,
                        const std::string_view role, const QString& typed, const double expected,
                        const char* what) {
    auto* cell = field(app, node, objectName);
    expect(cell != nullptr, what);
    if (cell == nullptr) {
        return;
    }
    click(app, cell);
    expect(cell->isEditing(), what);
    type(app, typed);
    key(app, Qt::Key_Return);
    const auto stored = scalarOf(app, node, role);
    expect(stored.has_value() && closeTo(static_cast<float>(*stored), expected), what);
    expect(closeTo(static_cast<float>(cell->value()), expected), what);
}

void textCardRows(App& app) {
    expect(addDefaultTextLayer(app.session), "a text layer exists to edit");
    // Reactivate after earlier popup gestures: offscreen has no window manager to restore it.
    app.editor.activateWindow();
    QCoreApplication::processEvents();
    app.editor.graphView()->setFocus();
    QCoreApplication::processEvents();
    const auto textNode = app.nodeOfType(document::kTextSourceNodeType);
    expect(textNode.has_value(), "the text layer built a Text card");
    if (!textNode.has_value()) {
        return;
    }
    typedNumberReaches(app, *textNode, QStringLiteral("nodeTextSizeEditor"),
                       document::kTextSizeParameterRole, QStringLiteral("96"), 96.0,
                       "click -> type -> Enter on the Text card's Size cell commits it");
    auto* content = qobject_cast<QLineEdit*>(app.editor.graphScene()->nodeFieldForTest(
        *textNode, QStringLiteral("nodeTextContentEditor")));
    expect(content != nullptr, "the Text card carries its content row");
    if (content == nullptr) {
        return;
    }
    click(app, content);
    expect(content->hasFocus(), "clicking the Text card's content row focuses its editor");
    QTest::keyClicks(app.editor.graphView(), QStringLiteral("HELLO"));
    QCoreApplication::processEvents();
    key(app, Qt::Key_Return);
    const auto* parameter = parameterFor(app, *textNode, document::kTextParameterRole);
    const auto* constant = parameter == nullptr
                               ? nullptr
                               : std::get_if<document::ConstantValueSource>(&parameter->source);
    const auto* held = constant == nullptr ? nullptr : std::get_if<std::string>(&constant->value);
    expect(held != nullptr && held->find("HELLO") != std::string::npos,
           "typed text on the Text card reaches the document");

    // The commit an artist actually makes most often: type, then click somewhere else. A QLineEdit
    // emits editingFinished from its own focusOutEvent, so this only lands if the row ever held
    // focus in the first place.
    click(app, content);
    QTest::keyClicks(app.editor.graphView(), QStringLiteral("WORLD"));
    QCoreApplication::processEvents();
    app.press(QPointF(-400.0, -400.0));
    app.release(QPointF(-400.0, -400.0));
    QCoreApplication::processEvents();
    const auto* after = parameterFor(app, *textNode, document::kTextParameterRole);
    const auto* afterConstant =
        after == nullptr ? nullptr : std::get_if<document::ConstantValueSource>(&after->source);
    const auto* afterHeld =
        afterConstant == nullptr ? nullptr : std::get_if<std::string>(&afterConstant->value);
    expect(afterHeld != nullptr && afterHeld->find("WORLD") != std::string::npos,
           "clicking away from the Text card's content row commits what was typed into it");
}

// A Math card's OPERAND cells, not only its operation dropdown: the generic editor built from the
// registry's declared kind is what every one of the forty value nodes uses.
void mathOperandCells(App& app, const document::NodeId math) {
    const auto editors = controls(app, math, QStringLiteral("nodeOperandEditor"));
    expect(editors.size() >= 2, "the Math card carries its operand cells");
    if (editors.size() < 2) {
        return;
    }
    auto* first = qobject_cast<kit::KValueField*>(editors.front());
    click(app, first);
    expect(first != nullptr && first->isEditing(), "clicking a Math operand cell opens its editor");
    type(app, QStringLiteral("3"));
    key(app, Qt::Key_Return);
    expect(closeTo(static_cast<float>(scalarOf(app, math, document::kFirstOperandPortName)
                                          .value_or(std::numeric_limits<double>::quiet_NaN())),
                   3.0),
           "a Math operand cell commits the number typed into it");
    auto* second = qobject_cast<kit::KValueField*>(editors[1]);
    click(app, second);
    type(app, QStringLiteral("4"));
    key(app, Qt::Key_Return);
    expect(closeTo(static_cast<float>(scalarOf(app, math, document::kSecondOperandPortName)
                                          .value_or(std::numeric_limits<double>::quiet_NaN())),
                   4.0),
           "the second Math operand cell commits independently of the first");
}

// A card edit must not rearrange the selection. An artist who has several cards selected and reads
// a value off one of them is not asking for the other selections to be dropped.
void editingAValueKeepsTheSelection(App& app, const document::NodeId layer,
                                    const document::NodeId other) {
    app.session.selectNodes({layer, other}, layer);
    QCoreApplication::processEvents();
    expect(app.session.selectedNodes().size() == 2, "two cards are selected before the edit");
    auto* rotation = field(app, layer, QStringLiteral("nodeRotationEditor"));
    if (rotation == nullptr) {
        return;
    }
    click(app, rotation);
    type(app, QStringLiteral("15"));
    key(app, Qt::Key_Return);
    expect(closeTo(static_cast<float>(
                       scalarOf(app, layer, document::kRotationParameterRole).value_or(0.0)),
                   15.0),
           "the edit lands while several cards are selected");
    expect(app.session.selectedNodes().size() == 2,
           "editing one card's value leaves the rest of the selection alone");
    expect(app.session.selectedNodes().contains(other),
           "the other selected card is still selected after the edit");
}

// A live edit is frozen at its original time and revision; changing either cancels it.
void aTimeChangeCancelsAnUnfinishedEdit(App& app, const document::NodeId layer) {
    auto* anchorX = field(app, layer, QStringLiteral("nodeAnchorXEditor"));
    if (anchorX == nullptr) {
        return;
    }
    const auto before = vec2Of(app, layer, document::kAnchorParameterRole);
    click(app, anchorX);
    type(app, QStringLiteral("12"));
    // Something else edits the document while the cell is open.
    (void)app.session.setCurrentTime(core::RationalTime::fromInteger(2));
    QCoreApplication::processEvents();
    app.addByType(document::kScalarValueNodeType, {-900.0, 900.0});
    QCoreApplication::processEvents();
    expect(!anchorX->isEditing() && !app.session.valueEditActive(),
           "changing time cancels the old field interaction");
    expect(vec2Of(app, layer, document::kAnchorParameterRole) == before,
           "time/revision invalidation does not commit the unfinished value");
}

void valueNodeCards(App& app) {
    struct Case final {
        std::string_view typeId;
        int components;
        const char* what;
    };
    static constexpr std::array kCases{
        Case{document::kScalarValueNodeType, 1, "the Scalar card's own cell commits"},
        Case{document::kVector2ValueNodeType, 2, "the Vector 2 card's own cells commit"},
        Case{document::kIntegerValueNodeType, 1, "the Integer card's own cell commits"},
    };
    double x = -1800.0;
    for (const auto& testCase : kCases) {
        const auto node = app.addByType(testCase.typeId, {x, 40.0});
        x += 260.0;
        const auto editors = controls(app, node, QStringLiteral("nodeOperandEditor"));
        expect(static_cast<int>(editors.size()) == testCase.components, testCase.what);
        if (editors.isEmpty()) {
            continue;
        }
        auto* first = qobject_cast<kit::KValueField*>(editors.front());
        click(app, first);
        expect(first != nullptr && first->isEditing(), testCase.what);
        type(app, QStringLiteral("7"));
        key(app, Qt::Key_Return);
        const auto* parameter = parameterFor(app, node, document::kValueParameterRole);
        const auto* constant = parameter == nullptr
                                   ? nullptr
                                   : std::get_if<document::ConstantValueSource>(&parameter->source);
        expect(constant != nullptr, testCase.what);
        if (constant == nullptr) {
            continue;
        }
        const bool holds = std::visit(
            [](const auto& held) {
                using Held = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<Held, double>) {
                    return held == 7.0;
                } else if constexpr (std::is_same_v<Held, std::int64_t>) {
                    return held == 7;
                } else if constexpr (std::is_same_v<Held, document::Vec2d>) {
                    return held.x == 7.0;
                } else {
                    return false;
                }
            },
            constant->value);
        expect(holds, testCase.what);
    }

    // A Colour value node's chip is the one value row with no number in it; it commits through the
    // same generic operand path.
    const auto colorNode = app.addByType(document::kColorValueNodeType, {x, 40.0});
    auto* chip = qobject_cast<kit::KColorChip*>(app.editor.graphScene()->nodeFieldForTest(
        colorNode, QStringLiteral("nodeOperandColorChip")));
    expect(chip != nullptr, "the Colour card carries its chip");
    if (chip != nullptr) {
        chip->setColor(kit::KColor::fromRgba(0.1F, 0.2F, 0.3F, 1.0F));
        QCoreApplication::processEvents();
        const auto* parameter = parameterFor(app, colorNode, document::kValueParameterRole);
        const auto* constant = parameter == nullptr
                                   ? nullptr
                                   : std::get_if<document::ConstantValueSource>(&parameter->source);
        const auto* held =
            constant == nullptr ? nullptr : std::get_if<core::Color4d>(&constant->value);
        expect(held != nullptr && closeTo(static_cast<float>(held->red), 0.010023),
               "the Colour card's chip commits its value");
    }
}

// The canvas must hold still while a value is being edited: an artist typing into a cell whose card
// moves under the pointer cannot aim at the next one. This is the state the application actually
// starts in -- the canvas frames the graph on show and keeps re-framing until the artist moves the
// view themselves -- which is exactly the state the shared harness normalizes away.
void canvasHoldsStillDuringAnEdit(App& app, const document::NodeId layer) {
    auto* view = app.editor.graphView();
    auto* opacity = field(app, layer, QStringLiteral("nodeOpacityEditor"));
    if (opacity == nullptr) {
        return;
    }
    const QTransform before = view->transform();
    click(app, opacity);
    type(app, QStringLiteral("40"));
    key(app, Qt::Key_Return);
    expect(view->transform() == before,
           "committing a value does not move the canvas under the pointer");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        App app;
        expect(addDefaultSolidLayer(app.session), "a solid layer exists to edit");
        QCoreApplication::processEvents();
        const auto layerNode = app.nodeOfType(document::kLayerOutputNodeType);
        const auto solidNode = app.nodeOfType(document::kSolidSourceNodeType);
        if (!layerNode.has_value() || !solidNode.has_value()) {
            std::cerr << "FAIL: the new solid layer has no Layer/Solid cards\n";
            return 1;
        }
        clickTypeEnterOnALayerCard(app, *layerNode);
        scrubOnALayerCard(app, *layerNode);
        colorChipCommits(app, *solidNode);
        tabMovesBetweenCells(app, *layerNode);
        escapeCancels(app, *layerNode);

        const auto math = app.addByType(document::kScalarMathNodeType, {-900.0, 40.0});
        operandDropdownCommits(app, math);
        mathOperandCells(app, math);
        canvasHoldsStillDuringAnEdit(app, *layerNode);
        aTimeChangeCancelsAnUnfinishedEdit(app, *layerNode);
        editingAValueKeepsTheSelection(app, *layerNode, math);
        textCardRows(app);
        valueNodeCards(app);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    try {
        App second;
        expect(addDefaultSolidLayer(second.session), "a solid layer exists to drive");
        QCoreApplication::processEvents();
        const auto layerNode = second.nodeOfType(document::kLayerOutputNodeType);
        if (!layerNode.has_value()) {
            return 1;
        }
        valueNodeDrivesDownstream(second, *layerNode);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
