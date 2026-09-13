// Task FIX1 item F: click a card, press Delete, and watch the node go -- through whatever widget
// the click actually left the keyboard focus on, because that is where the real application's key
// press lands and assuming the view has it is exactly how a keyboard binding passes a test and
// fails an artist.
//
// The failing state is the ordinary one after any card edit: kit::KValueField::endEdit() keeps
// focus on the FIELD once the artist commits with Enter, so the scene's focus item is a hosted
// proxy and the canvas declined its own Delete binding -- even though a value field, a colour chip,
// a switch and a dropdown have no use for the key at all.
#include "node_production_harness.hpp"

#include <bloom/ui/kit/value_field.hpp>

#include <QGraphicsProxyWidget>
#include <QKeyEvent>
#include <QSignalSpy>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::production_test;

namespace {
// The key press an artist makes: delivered to the focus widget the click left behind, not to a
// widget the test chose.
void pressKeyAtFocus(App& a, const Qt::Key key) {
    auto* target = QApplication::focusWidget();
    if (target == nullptr)
        target = a.editor.graphView();
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &release);
    QCoreApplication::processEvents();
}

[[nodiscard]] QPointF centreOf(const QWidget* widget) {
    return widget->graphicsProxyWidget()->mapToScene(
        widget->graphicsProxyWidget()->boundingRect().center());
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        App a;
        const auto solid = a.addByType(document::kSolidSourceNodeType, {-400, 0});
        const auto text = a.addByType(document::kTextSourceNodeType, {-400, 300});
        QCoreApplication::processEvents();

        // --- A plain header click, then Delete. --------------------------------------------------
        {
            auto* card = a.card(solid);
            expect(card != nullptr, "the solid card exists");
            if (card == nullptr)
                return 1;
            const auto before = a.session.composition()->graph().nodes().size();
            const QPointF header = card->scenePos() + QPointF(24, 12);
            a.press(header);
            a.release(header);
            QCoreApplication::processEvents();
            expect(a.session.selectedNodes().contains(solid), "clicking the card selects it");
            pressKeyAtFocus(a, Qt::Key_Delete);
            expect(a.session.composition()->graph().nodes().size() == before - 1 &&
                       a.session.composition()->graph().findNode(solid) == nullptr,
                   "Delete removes the selected node after a click");
            expect(a.session.undo() && a.session.composition()->graph().findNode(solid) != nullptr,
                   "and one undo restores it");
            QCoreApplication::processEvents();
        }

        // --- THE REPORTED STATE: a card edit just committed, so a hosted control holds the focus.
        // -
        {
            const auto layer = a.addByType(document::kLayerOutputNodeType, {200, 0});
            QCoreApplication::processEvents();
            auto* row = qobject_cast<kit::KValueField*>(a.editor.graphScene()->nodeFieldForTest(
                layer, QStringLiteral("nodePositionXEditor")));
            expect(row != nullptr && row->graphicsProxyWidget() != nullptr,
                   "the Layer card hosts a value row");
            if (row != nullptr && row->graphicsProxyWidget() != nullptr) {
                // Click it (the editor opens with the number selected), type, commit with Enter.
                const QPointF cell = centreOf(row);
                a.press(cell);
                a.release(cell);
                QCoreApplication::processEvents();
                expect(row->lineEdit() != nullptr && row->lineEdit()->isVisible(),
                       "a click on a value row opens its text editor");
                if (row->lineEdit() != nullptr) {
                    row->lineEdit()->setText(QStringLiteral("48"));
                    QTest::keyClick(row->lineEdit(), Qt::Key_Return);
                    QCoreApplication::processEvents();
                }
                expect(row->lineEdit() != nullptr && !row->lineEdit()->isVisible(),
                       "committing with Enter closes the editor and leaves the FIELD focused");
                expect(a.session.selectedNodes().contains(layer),
                       "the card the artist just edited is the selected one");

                const auto before = a.session.composition()->graph().nodes().size();
                pressKeyAtFocus(a, Qt::Key_Delete);
                expect(a.session.composition()->graph().nodes().size() == before - 1,
                       "and Delete now removes it: a value field with no editor open has no use "
                       "for the key");
                expect(a.session.undo(), "one undo restores it");
                QCoreApplication::processEvents();
                a.editor.graphScene()->clearFocus();
            }
        }

        // --- An ACTIVE text editor still keeps the key. ------------------------------------------
        {
            auto* field = a.editor.graphScene()->nodeFieldForTest(
                text, QStringLiteral("nodeTextContentEditor"));
            expect(field != nullptr, "the text card hosts a string field");
            if (field != nullptr && field->graphicsProxyWidget() != nullptr) {
                const QPointF cell = centreOf(field);
                a.press(cell);
                a.release(cell);
                QCoreApplication::processEvents();
                const auto before = a.session.composition()->graph().nodes().size();
                pressKeyAtFocus(a, Qt::Key_Delete);
                expect(a.session.composition()->graph().nodes().size() == before,
                       "Delete inside an active text editor never deletes the node");
                a.editor.graphScene()->clearFocus();
                a.editor.graphView()->setFocus();
                QCoreApplication::processEvents();
            }
        }

        // --- Backspace is the same binding.
        // -------------------------------------------------------
        {
            a.session.selectNode(text);
            QCoreApplication::processEvents();
            pressKeyAtFocus(a, Qt::Key_Backspace);
            expect(a.session.composition()->graph().findNode(text) == nullptr,
                   "Backspace removes the selected node too");
            expect(a.session.undo() && a.session.composition()->graph().findNode(text) != nullptr,
                   "and one undo restores it");
            QCoreApplication::processEvents();
        }

        // --- A protected node refuses, out loud.
        // --------------------------------------------------
        {
            const auto stack = a.nodeOfType(document::kLayerStackNodeType);
            expect(stack.has_value(), "the composition holds a Merge node");
            if (stack.has_value()) {
                a.session.selectNode(*stack);
                QCoreApplication::processEvents();
                QSignalSpy refusals(&a.session, &CompositionSession::commandRejected);
                const auto before = a.session.composition()->graph().nodes().size();
                pressKeyAtFocus(a, Qt::Key_Delete);
                expect(a.session.composition()->graph().nodes().size() == before,
                       "Merge cannot be deleted");
                expect(!refusals.isEmpty(), "and the refusal reaches the artist as a status line");
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
