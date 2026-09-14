#pragma once
// Every composition surface at once, built exactly as the application builds them, over ONE
// CompositionSession: the node canvas, the Properties panel, and the Timeline. Shared by task
// FIX2's two pins -- the timeline row's own controls, and the one-truth synchronization sweep --
// for the same reason node_production_harness.hpp is shared: a fixture that exists to ask "do the
// surfaces agree" cannot be a private copy per file without the copies being free to disagree.
//
// Gestures go through QTest's WINDOW-level mouse helpers on purpose. QTest::mouseClick(QWidget*)
// maps the point onto the widget's own window and hands it to Qt's real delivery path, so the
// receiver is picked by QWidget::childAt() exactly as it is under a live pointer -- which is the
// only way a hit-testing defect can show up in a test at all.
#include <bloom/commands/command_stack.hpp>
#include <bloom/core/blend_mode.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QListView>
#include <QScrollBar>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::ui::surface_test {

inline int failures = 0;
inline void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct Pipeline final {
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    PreviewPreparationFunction prepare;

    Pipeline() : compiler(definitions) {
        if (!runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
        prepare = makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                 qualifiedProcessorProvider);
    }
};

[[nodiscard]] inline runtime::TaskSchedulerConfig schedulerConfig() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 16,
            .blockingIoQueueCapacity = 4,
            .terminalHistoryCapacity = 32,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

struct Surfaces final {
    document::NewProject initial =
        document::makeNewProject("FIX2", "Main", core::RationalTime::fromInteger(10));
    document::Document document{std::move(initial.project)};
    commands::CommandStack stack{document};
    CompositionSession session{document, stack, initial.initialCompositionId};
    runtime::TaskScheduler scheduler{schedulerConfig()};
    TaskUiBridge bridge{scheduler, nullptr, std::chrono::milliseconds{1}};
    Pipeline pipeline;
    CompositionPreviewController controller{session, scheduler, bridge, pipeline.prepare};

    // One shown top-level window carrying all three panels, because a gesture has to travel a real
    // window to be delivered the way a real one is.
    QWidget window;
    NodeGraphEditor* nodes = nullptr;
    PropertiesEditor* properties = nullptr;
    TimelineEditor* timeline = nullptr;

    Surfaces() {
        auto* layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        nodes = new NodeGraphEditor(session, &window);
        properties = new PropertiesEditor(session, &window);
        timeline = new TimelineEditor(session, controller, &window);
        layout->addWidget(nodes);
        layout->addWidget(properties);
        layout->addWidget(timeline);
        window.resize(1400, 1100);
        window.show();
        QCoreApplication::processEvents();
        nodes->graphView()->zoomToActualSize();
        QCoreApplication::processEvents();
    }

    ~Surfaces() {
        controller.beginShutdown();
        bridge.beginShutdown();
        QElapsedTimer timer;
        timer.start();
        while (!scheduler.isQuiescent() && timer.elapsed() < 4'000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    }

    Surfaces(const Surfaces&) = delete;
    Surfaces& operator=(const Surfaces&) = delete;
    Surfaces(Surfaces&&) = delete;
    Surfaces& operator=(Surfaces&&) = delete;

    [[nodiscard]] TimelineLayerStack* layerStack() const { return timeline->layerStackForTest(); }

    // The pooled row widget currently drawing `index`, found by the geometry the column itself
    // assigns rather than by pool order, so a scrolled column answers honestly.
    [[nodiscard]] QWidget* layerRow(const int index) const {
        auto* column = layerStack();
        if (column == nullptr) {
            return nullptr;
        }
        for (auto* row : column->findChildren<QWidget*>(QStringLiteral("timelineLayerRow"))) {
            if (row->isVisible() && row->y() == column->rowTop(index)) {
                return row;
            }
        }
        return nullptr;
    }

    [[nodiscard]] kit::KDropdown* blendingDropdown(const int index) const {
        auto* row = layerRow(index);
        return row == nullptr
                   ? nullptr
                   : row->findChild<kit::KDropdown*>(QStringLiteral("layerBlendingDropdown"));
    }

    [[nodiscard]] std::optional<document::NodeId> nodeOfType(std::string_view typeId) const {
        for (const auto& node : session.composition()->graph().nodes()) {
            if (node.typeId == typeId) {
                return node.id;
            }
        }
        return std::nullopt;
    }
};

// A real click: mapped onto the WINDOW and handed to QTest's QPA overload, never sent straight at
// the widget. QTest::mouseClick(QWidget*) posts the event to that widget with qApp->notify(), which
// skips Qt's own receiver picking entirely -- so a widget that a live pointer could never reach
// would still answer, and a hit-testing defect would be invisible. Going through the window means
// QWidgetWindow::handleMouseEvent picks the receiver with QWidget::childAt(), exactly as it does
// under a real pointer.
inline void click(QWidget* widget, const QPoint& point = {}) {
    auto* handle = widget->window()->windowHandle();
    if (handle == nullptr) {
        return;
    }
    const QPoint local = point.isNull() ? widget->rect().center() : point;
    QTest::mouseClick(handle, Qt::LeftButton, Qt::NoModifier,
                      handle->mapFromGlobal(widget->mapToGlobal(local)));
    QCoreApplication::processEvents();
}

// Picks row `index` out of an open dropdown popup, the way releasing on it does.
inline void pickPopupRow(kit::KDropdown& dropdown, const int index) {
    auto* view = dropdown.popupView();
    if (view == nullptr || view->model() == nullptr) {
        return;
    }
    const QRect row = view->visualRect(view->model()->index(index, 0));
    click(view->viewport(), row.center());
}

} // namespace bloom::ui::surface_test
