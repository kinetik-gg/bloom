#pragma once
// The node editor built exactly as the application builds it -- NodeGraphEditor over a real
// CompositionSession, with no adapter on NodeGraphicsScene::setSubmit() -- plus the pointer and
// keyboard gestures an artist actually performs on it, and the production compile/evaluate pair
// that says what the viewer would show. Shared by the task FIX1 pins so each of them states a
// gesture and a consequence rather than re-deriving the harness.
#include "node_editor_add.hpp"
#include "node_editor_items.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/search_popup.hpp>
#include <bloom/ui/node_editor.hpp>

#include <QApplication>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTest>

#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bloom::ui::production_test {
inline int failures = 0;
inline void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct App final {
    document::NewProject initial =
        document::makeNewProject("FIX1", "Main", core::RationalTime::fromInteger(10));
    document::Document document{std::move(initial.project)};
    commands::CommandStack stack{document};
    CompositionSession session{document, stack, initial.initialCompositionId};
    NodeGraphEditor editor{session};

    App() {
        editor.resize(1400, 900);
        editor.show();
        QCoreApplication::processEvents();
        editor.graphView()->zoomToActualSize();
        editor.graphView()->setTransform(QTransform::fromTranslate(40, 40));
        editor.graphView()->setFocus();
    }
    [[nodiscard]] std::optional<document::NodeId> nodeOfType(std::string_view typeId) const {
        for (const auto& node : session.composition()->graph().nodes())
            if (node.typeId == typeId)
                return node.id;
        return std::nullopt;
    }
    [[nodiscard]] node_editor::NodeItem* card(document::NodeId id) {
        return dynamic_cast<node_editor::NodeItem*>(editor.graphScene()->findNodeItem(id));
    }
    [[nodiscard]] node_editor::SocketItem* namedSocket(document::NodeId id, const QString& name,
                                                       bool input) {
        auto* item = card(id);
        if (item == nullptr)
            return nullptr;
        for (auto* socket : item->sockets())
            if (socket->input.has_value() == input && socket->name == name)
                return socket;
        return nullptr;
    }
    [[nodiscard]] node_editor::SocketItem* outputSocket(document::NodeId id) {
        auto* item = card(id);
        if (item == nullptr)
            return nullptr;
        for (auto* socket : item->sockets())
            if (socket->output.has_value())
                return socket;
        return nullptr;
    }
    void mouse(QEvent::Type type, QPointF point, Qt::MouseButton button, Qt::MouseButtons buttons) {
        auto* view = editor.graphView();
        const QPoint viewport = view->mapFromScene(point);
        QMouseEvent event(type, viewport, view->viewport()->mapToGlobal(viewport), button, buttons,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(view->viewport(), &event);
    }
    void press(QPointF point) {
        mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    }
    void move(QPointF point) { mouse(QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton); }
    void release(QPointF point) {
        mouse(QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    }
    void drag(QPointF start, QPointF end) {
        press(start);
        move(end);
        release(end);
    }
    document::NodeId addByType(std::string_view typeId, document::Vec2d position) {
        commands::Transaction transaction("Add Node", session.snapshot().revision());
        transaction.emplace<node_editor::AddEditorNode>(session.compositionId(),
                                                        std::string(typeId), position);
        const auto result = session.executeNodeTransaction(std::move(transaction));
        const auto id = result.outputId<document::NodeId>("editorNode");
        if (!id)
            throw std::runtime_error("addByType failed");
        QCoreApplication::processEvents();
        return *id;
    }
    // Tab -> type -> Enter, the owner's own add gesture.
    std::optional<document::NodeId> addThroughSearch(const QString& query) {
        const auto before = session.composition()->graph().nodes().size();
        QTest::keyClick(editor.graphView(), Qt::Key_Tab);
        QCoreApplication::processEvents();
        auto* popup = editor.findChild<kit::KSearchPopup*>();
        if (popup == nullptr || !popup->isVisible())
            return std::nullopt;
        auto* field = popup->findChild<QLineEdit*>();
        field->setText(query);
        QTest::keyClick(field, Qt::Key_Return);
        QCoreApplication::processEvents();
        if (session.composition()->graph().nodes().size() == before)
            return std::nullopt;
        const auto* selected = session.selectedNode();
        return selected == nullptr ? std::nullopt : std::optional(selected->id);
    }
};

inline const document::ParameterRecord* parameterFor(const App& app, document::NodeId node,
                                                     std::string_view role) {
    const auto* record = app.session.composition()->graph().findNode(node);
    if (record == nullptr)
        return nullptr;
    for (const auto& binding : record->parameters)
        if (binding.role == role)
            return app.session.composition()->parameters().find(binding.parameterId);
    return nullptr;
}

inline const document::DriverBindingSource* driverFor(const App& app, document::NodeId node,
                                                      std::string_view role) {
    const auto* parameter = parameterFor(app, node, role);
    return parameter == nullptr ? nullptr
                                : std::get_if<document::DriverBindingSource>(&parameter->source);
}

struct Composited final {
    bool ok = false;
    bool anyCoverage = false;
    std::array<float, 4> centerPixel{};
};

// One frame, through the production compiler and the CPU reference evaluator.
inline Composited composite(const App& app,
                            core::RationalTime time = core::RationalTime::fromInteger(0)) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const runtime::SnapshotCompileRequest request{app.session.snapshot(),
                                                  app.session.compositionId()};
    const runtime::CancellationToken cancellation;
    const auto compiled = compiler.compile(request, cancellation);
    if (compiled.status != runtime::SnapshotCompileStatus::Compiled || !compiled.plan) {
        for (const auto& diagnostic : compiled.diagnostics)
            std::cerr << "DIAG: compile " << diagnostic.summary << " | " << diagnostic.detail
                      << '\n';
        return {};
    }
    const runtime::CpuCompositionEvaluator evaluator;
    const runtime::EvaluationRequest evaluation{
        .time = time,
        .output = compiled.plan->output(),
        .resolution = runtime::CompositionFormatResolution{},
        .quality = runtime::EvaluationQuality::Reference,
        .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
        .pixelStorageByteLimit = std::size_t{1} << 28U};
    const auto result = evaluator.evaluate(compiled.plan, evaluation, cancellation);
    if (result.status() != runtime::EvaluationStatus::Evaluated || !result.frame()) {
        for (const auto& diagnostic : result.diagnostics())
            std::cerr << "DIAG: evaluate " << diagnostic.summary << '\n';
        return {};
    }
    Composited out;
    out.ok = true;
    const auto& image = result.frame()->processImage();
    if (const auto read = image.read(0, 0))
        out.centerPixel = {read.value()->red(), read.value()->green(), read.value()->blue(),
                           read.value()->alpha()};
    for (const auto& pixel : image.pixels())
        if (pixel.alpha() > 0.0F)
            out.anyCoverage = true;
    return out;
}

[[nodiscard]] inline bool closeTo(const float value, const double expected) {
    return std::abs(static_cast<double>(value) - expected) < 1e-4;
}
} // namespace bloom::ui::production_test
