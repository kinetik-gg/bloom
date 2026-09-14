#pragma once
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <memory>
#include <stdexcept>

namespace bloom::ui::test {
struct WindowFixture {
    QTemporaryDir settingsDirectory{QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR "/settings-XXXXXX")};
    runtime::TaskScheduler scheduler;
    ProjectHost projectHost{scheduler};
    CompositionSession session{*projectHost.liveDocumentAndStack().first,
                               *projectHost.liveDocumentAndStack().second,
                               projectHost.lowestCompositionId()};
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler{definitions};
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer display;
    runtime::QualifiedDisplayProcessorProvider qualified;
    TaskUiBridge bridge{scheduler};
    std::unique_ptr<CompositionPreviewController> preview;
    std::unique_ptr<FrameExportController> exporter;
    EditorRegistry registry;
    std::unique_ptr<MainWindow> window;

    WindowFixture() {
        if (!settingsDirectory.isValid())
            throw std::runtime_error("Fixture settings directory failed");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            throw std::runtime_error("Node definitions failed");
        definitions.freeze();
        if (!session.addSolidLayer("Background", {0.035, 0.045, 0.075, 1.0}) ||
            !session.addTextLayer("Bloom grammar", "Hello, Bloom!", 100.0) ||
            !session.setSelectedPosition(720.0, 480.0))
            throw std::runtime_error("Fixture commands failed");
        preview = std::make_unique<CompositionPreviewController>(
            session, scheduler, bridge,
            makeCompositionPreviewPipeline(compiler, evaluator, display, qualified));
        exporter = std::make_unique<FrameExportController>(session, scheduler, bridge, compiler,
                                                           projectHost.publicationCoordinator(),
                                                           projectHost.artifactCoordinator());
        if (!registerFoundationEditors(registry, session, *preview))
            throw std::runtime_error("Editor registration failed");
        window = std::make_unique<MainWindow>(registry, session, projectHost, *exporter, nullptr,
                                              preview.get());
        window->resize(1920, 1200);
        window->show();
        QElapsedTimer timer;
        timer.start();
        while (preview->state().activity != PreviewActivity::Ready && timer.elapsed() < 15000)
            QTest::qWait(10);
        if (preview->state().activity != PreviewActivity::Ready)
            throw std::runtime_error("Fixture preview did not become ready");
        QTest::qWait(100);
        clearInteraction();
    }
    void clearInteraction() {
        if (auto* focus = QApplication::focusWidget())
            focus->clearFocus();
        for (auto* widget : window->findChildren<QWidget*>()) {
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(widget, &leave);
        }
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(window.get(), &leave);
        QApplication::processEvents();
    }
    ~WindowFixture() {
        window.reset();
        preview->beginShutdown();
        bridge.beginShutdown();
        QElapsedTimer timer;
        timer.start();
        while (!scheduler.isQuiescent() && timer.elapsed() < 15000) {
            QTest::qWait(5);
        }
    }
};
} // namespace bloom::ui::test
