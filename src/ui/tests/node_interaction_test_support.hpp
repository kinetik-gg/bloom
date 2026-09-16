#pragma once
#include "node_editor_interactions.hpp"
#include <QApplication>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/kit/search_popup.hpp>
#include <iostream>
#include <stdexcept>

namespace bloom::ui::test {
inline int failures = 0;
inline void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}
struct Fixture final {
    document::Document document;
    commands::CommandStack stack;
    CompositionSession session;
    NodeGraphEditor editor;
    commands::CommandResult last;
    explicit Fixture(document::NewProject initial = document::makeNewProject(
                         "N3", "Main", core::RationalTime::fromInteger(10)))
        : document(std::move(initial.project)), stack(document),
          session(document, stack, initial.initialCompositionId), editor(session) {
        // Adapter supplied by the fixture because the package's session ownership fence forbids
        // exposing its private submission path. It uses real commands and public rebind; production
        // integration remains explicitly blocked pending that single ownership exception.
        editor.graphScene()->setSubmit([this](commands::Transaction&& transaction) {
            const auto selection = session.selectedNodes();
            const auto primary =
                session.selectedNode() ? session.selectedNode()->id : document::NodeId{};
            const auto composition = session.compositionId();
            last = stack.execute(std::move(transaction));
            if (last.changed()) {
                session.rebind(document, stack, composition);
                if (!selection.empty())
                    session.selectNodes(selection, primary);
            }
            if (!last.succeeded()) {
                const auto message =
                    last.operationFailures.empty()
                        ? QStringLiteral("Edit refused")
                        : QString::fromStdString(last.operationFailures.front().issue.message);
                Q_EMIT session.commandRejected(message);
            }
            return last;
        });
        editor.resize(1200, 800);
        editor.show();
        QCoreApplication::processEvents();
        editor.graphView()->zoomToActualSize();
        editor.graphView()->setTransform(QTransform::fromTranslate(40, 40));
        editor.graphView()->setFocus();
    }
    NodeGraphicsScene* scene() { return editor.graphScene(); }
    NodeGraphicsView* view() { return editor.graphView(); }
    template <class Op, class... Args> commands::CommandResult edit(Args&&... args) {
        commands::Transaction transaction("Fixture edit", session.snapshot().revision());
        transaction.emplace<Op>(session.compositionId(), std::forward<Args>(args)...);
        return scene()->submit(std::move(transaction));
    }
    document::NodeId add(std::string_view type, QPointF point) {
        const auto result =
            edit<commands::AddNode>(std::string(type), document::Vec2d{point.x(), point.y()});
        const auto id = result.outputId<document::NodeId>(commands::kAddNodeOutput);
        if (!id)
            throw std::runtime_error("fixture AddNode");
        return *id;
    }
    node_editor::NodeItem* card(document::NodeId id) {
        return dynamic_cast<node_editor::NodeItem*>(scene()->findNodeItem(id));
    }
    node_editor::SocketItem* socket(document::NodeId id, bool input) {
        for (auto* socket : card(id)->sockets())
            if (socket->input.has_value() == input)
                return socket;
        throw std::runtime_error("fixture socket");
    }
    void mouse(QEvent::Type type, QPointF point, Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const QPoint viewport = view()->mapFromScene(point);
        QMouseEvent event(type, viewport, view()->viewport()->mapToGlobal(viewport), button,
                          buttons, modifiers);
        QCoreApplication::sendEvent(view()->viewport(), &event);
    }
    void press(QPointF point, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
               Qt::MouseButton button = Qt::LeftButton) {
        mouse(QEvent::MouseButtonPress, point, button, button, modifiers);
    }
    void move(QPointF point, Qt::MouseButtons buttons = Qt::LeftButton,
              Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        mouse(QEvent::MouseMove, point, Qt::NoButton, buttons, modifiers);
    }
    void release(QPointF point, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                 Qt::MouseButton button = Qt::LeftButton) {
        mouse(QEvent::MouseButtonRelease, point, button, Qt::NoButton, modifiers);
    }
    void drag(QPointF start, QPointF end, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
              Qt::MouseButton button = Qt::LeftButton) {
        press(start, modifiers, button);
        move(end, button, modifiers);
        release(end, modifiers, button);
    }
    void click(QPointF point, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        press(point, modifiers);
        release(point, modifiers);
    }
    void key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::keyClick(view(), key, modifiers);
    }
};
void testLayoutSelectionAndSockets();
void testConnectionsCutAndInsertion();
// Task FOLLOW-1: the Merge card's second, audio-typed stack pill.
void testMergeAudioPill();
void testSearchKeyboardAndMenus();
// Task NODES-1: header menus, grid snapping, link style, and the footer.
void testHeaderMenusGridSnappingLinkStyleAndFooter();
} // namespace bloom::ui::test
