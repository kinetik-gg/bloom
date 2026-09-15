#pragma once
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/node_layout.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <functional>
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
    std::unique_ptr<AssetController> assets;
    std::unique_ptr<CompositionPreviewController> preview;
    std::unique_ptr<FrameExportController> exporter;
    EditorRegistry registry;
    std::unique_ptr<MainWindow> window;

    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks) -- QApplication owns its style.
    explicit WindowFixture(const std::function<void(WindowFixture&)>& author = {}) {
        QCoreApplication::setApplicationVersion("0.1.0");
        // Match apps/bloom/main.cpp: the application installs this proxy after the theme.
        QApplication::setStyle(new kit::AltUnderlineProxyStyle());
        if (!settingsDirectory.isValid())
            throw std::runtime_error("Fixture settings directory failed");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            throw std::runtime_error("Node definitions failed");
        definitions.freeze();
        assets = std::make_unique<AssetController>(session, projectHost, scheduler, bridge);
        if (author)
            author(*this);
        else if (!session.addSolidLayer("Background", {0.035, 0.045, 0.075, 1.0}) ||
                 !session.addTextLayer("Bloom grammar", "Hello, Bloom!", 100.0) ||
                 !session.setSelectedPosition(720.0, 480.0))
            throw std::runtime_error("Fixture commands failed");
        // Author the sample graph through the same command as a user arrangement. Production
        // preserves saved positions, including the compact legacy defaults from layer creation.
        std::map<document::NodeId, document::Vec2d> positions;
        for (const auto& [id, layout] :
             document::defaultNodeLayout(session.composition()->graph().nodes()))
            positions.emplace(id,
                              document::Vec2d{layout.position.x * 1.5, layout.position.y * 2.2});
        commands::Transaction arrange("Arrange sample graph", session.snapshot().revision());
        arrange.emplace<commands::MoveNodes>(session.compositionId(), std::move(positions));
        if (!session.executeTransaction(std::move(arrange)).succeeded())
            throw std::runtime_error("Fixture arrangement failed");
        // A pinned budget: the status bar prints it, and the production default follows the
        // machine's memory, which would make every whole-window golden machine-dependent.
        preview = std::make_unique<CompositionPreviewController>(
            session, scheduler, bridge,
            makeCompositionPreviewPipeline(compiler, evaluator, display, qualified),
            CompositionPreviewSettings{},
            std::make_shared<PreviewFrameCache>(kMinimumPreviewFrameCacheByteBudget));
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
        // Colour swatches show display colour once the converter is up; capture only after it
        // is, or the same window renders two different Properties panels from run to run.
        while (!session.colorConverter() && timer.elapsed() < 15000)
            QTest::qWait(10);
        if (!session.colorConverter())
            throw std::runtime_error("Fixture colour converter did not become ready");
        QTest::qWait(100);
        auto* stack = window->findChild<TimelineLayerStack*>();
        for (const auto& layer : session.composition()->graph().layerOutputs()) {
            if (layer.name == "Background") {
                session.selectLayer(layer.layerId);
                Q_EMIT stack->expansionRequested(layer.layerId);
                break;
            }
        }
        QTest::qWait(50);
        clearInteraction();
    }
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
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
        assets->cancel();
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
