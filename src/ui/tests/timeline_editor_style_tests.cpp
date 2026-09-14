
// Task T1: the timeline's AE-style layer stack and lane region. This file owns the layer-row
// chrome, the two-region geometry, and the transport restyle; timeline_ruler_tests.cpp owns the
// ruler's own tick-density/scrub contract and the keyframe panel's gestures. Offscreen, matching
// every other widget test in this suite.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
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
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QRect>
#include <QScrollBar>
#include <QSettings>
#include <QString>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QVariant>
#include <QWheelEvent>
#include <QWidget>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <variant>

namespace {

using namespace std::chrono_literals;

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

[[nodiscard]] bool near(const QColor& left, const QColor& right, const int tolerance) {
    return std::abs(left.red() - right.red()) <= tolerance &&
           std::abs(left.green() - right.green()) <= tolerance &&
           std::abs(left.blue() - right.blue()) <= tolerance;
}

bloom::runtime::TaskSchedulerConfig testSchedulerConfig() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 16,
            .blockingIoQueueCapacity = 4,
            .terminalHistoryCapacity = 32,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

bloom::document::CompositionFormat smallFormat() {
    const auto format = bloom::document::CompositionFormat::create(4, 3);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

[[nodiscard]] bloom::core::RationalTime time(const std::int64_t numerator,
                                             const std::int64_t denominator = 1) {
    const auto value = bloom::core::RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

bloom::document::NewProject makeTestProject(std::string projectName) {
    return bloom::document::makeNewProject(std::move(projectName), "Main", time(10), smallFormat());
}

// Mirrors timeline_ruler_tests.cpp's own PipelineFixture/SessionFixture exactly -- no shared test
// fixture header exists in this suite (every widget test file in src/ui/tests owns a private copy),
// so this duplication follows the established per-file idiom rather than inventing a new cross-file
// dependency.
struct PipelineFixture final {
    bloom::runtime::NodeDefinitionRegistry definitions;
    bloom::runtime::SnapshotCompiler compiler;
    bloom::runtime::CpuCompositionEvaluator evaluator;
    bloom::runtime::CpuReferenceDisplayPreparer displayPreparer;
    bloom::runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    bloom::ui::PreviewPreparationFunction pipeline;

    PipelineFixture() : compiler(definitions) {
        if (!bloom::runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
        pipeline = bloom::ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                             qualifiedProcessorProvider);
    }
};

struct SessionFixture final {
    bloom::document::Document document;
    bloom::commands::CommandStack commands;
    bloom::ui::CompositionSession session;
    bloom::runtime::TaskScheduler scheduler;
    bloom::ui::TaskUiBridge bridge;
    PipelineFixture pipeline;
    bloom::ui::CompositionPreviewController controller;

    explicit SessionFixture(bloom::document::NewProject newProject)
        : document(std::move(newProject.project)), commands(document),
          session(document, commands, newProject.initialCompositionId),
          scheduler(testSchedulerConfig()), bridge(scheduler, nullptr, 1ms),
          controller(session, scheduler, bridge, pipeline.pipeline) {}
};

void finishFixture(SessionFixture& fixture) {
    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    QElapsedTimer timer;
    timer.start();
    while (!fixture.scheduler.isQuiescent() && timer.elapsed() < 4'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

// Forces the panel's real geometry to materialize (a fixed-width column, an expanding lane region,
// and a pooled set of row widgets only acquire real rects once the window actually lays itself
// out), the same "show a real top-level window, pump events" idiom playback_controller_tests.cpp
// already uses for its focus test. The width is comfortably wider than the layer column plus its
// gutter so the lane region is never degenerate.
void layoutEditor(QWidget& host, const int width = 1200, const int height = 420) {
    host.resize(width, height);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

[[nodiscard]] bool waitUntilReady(SessionFixture& fixture) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4'000) {
        if (fixture.controller.state().activity == bloom::ui::PreviewActivity::Ready) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return false;
}

void sendMouse(QWidget& widget, const QEvent::Type type, const qreal pixelX, const qreal pixelY) {
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const QPointF local(pixelX, pixelY);
    QMouseEvent event(type, local, widget.mapToGlobal(local), button, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
}

// The centre x of the Accent-colored run on scanline `row` of `image`, or -1 when there is none. A
// 1px playhead line gives a one-pixel run whose centre IS its x; the triangular head marker gives a
// wider run centred on the same x, which is exactly what "the marker heads the line" has to mean.
[[nodiscard]] int accentRunCenter(const QImage& image, const int row) {
    const QColor accent = bloom::ui::kit::color(bloom::ui::kit::Color::Accent);
    int first = -1;
    int last = -1;
    for (int x = 0; x < image.width(); ++x) {
        if (near(image.pixelColor(x, row), accent, 10)) {
            if (first < 0) {
                first = x;
            }
            last = x;
        }
    }
    return first < 0 ? -1 : (first + last) / 2;
}

// ---------------------------------------------------------------------------------------------
// The pinned geometry: ONE x origin for the ruler, the lanes, and the work-area strip, and that
// origin is the layer column's own width -- the owner's "the tick where the playhead is playing
// needs to be indented to the right and shows 0 after the layer properties section".
void testRulerAndLanesShareTheLaneRegionOrigin(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Ruler Origin Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);

    auto* stack = editor->layerStackForTest();
    auto* lanes = editor->laneRegionForTest();
    auto* ruler = editor->rulerForTest();
    auto* workArea = editor->findChild<ui::TimelineWorkAreaRow*>("timelineWorkAreaRow");
    expectations.expect(stack != nullptr && lanes != nullptr && ruler != nullptr &&
                            workArea != nullptr,
                        "the layer column, the lane region, the ruler and the work-area row all "
                        "exist");
    if (stack == nullptr || lanes == nullptr || ruler == nullptr || workArea == nullptr) {
        delete editor;
        finishFixture(fixture);
        return;
    }

    const int columnWidth = ui::TimelineEditor::layerColumnWidth();
    expectations.expect(stack->width() == columnWidth,
                        "the layer column paints at its own fixed width");
    expectations.expect(lanes->mapTo(editor, QPoint(0, 0)).x() == columnWidth,
                        "the lane region starts exactly at the layer column's right edge");
    expectations.expect(ruler->mapTo(editor, QPoint(0, 0)).x() == columnWidth,
                        "the RULER starts at the same x -- it never extends over the left column");
    expectations.expect(workArea->mapTo(editor, QPoint(0, 0)).x() == columnWidth,
                        "the work-area strip above them starts at the same x");
    expectations.expect(ruler->width() == lanes->width() && ruler->width() == workArea->width(),
                        "all three share one extent too, so one frame is one x for all of them");
    expectations.expect(ruler->width() > 0, "the lane region is not degenerate (test sanity)");

    // Frame 0's own label: inside the ruler it sits at the ruler's left edge, and mapped into the
    // editor it lands at or after the layer column's width -- never over the column.
    const auto labels = ruler->majorTickLabelRectsForTest();
    expectations.expect(!labels.empty(), "the ruler labels at least one major tick");
    if (!labels.empty()) {
        const QRectF first = labels.front();
        expectations.expect(first.left() >= 0.0,
                            "the frame-0 label starts at the ruler's own left edge");
        expectations.expect(
            ruler->mapTo(editor, QPoint(static_cast<int>(first.left()), 0)).x() >= columnWidth,
            "the frame-0 label is indented past the layer column, never painted over it");
    }

    delete editor;
    finishFixture(fixture);
}

void testHeaderSplitInEditorArea(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Header composition with a deliberately long name"));
    ui::EditorRegistry registry;
    (void)registry.registerEditor({"bloom.timeline", "Timeline", [&](QWidget* parent) {
                                       return new ui::TimelineEditor(
                                           fixture.session, fixture.controller, nullptr, parent);
                                   }});
    (void)registry.registerEditor(
        {"bloom.probe", "Probe", [](QWidget* parent) { return new QWidget(parent); }});
    ui::EditorArea area(registry, "bloom.timeline");
    area.show();
    for (const int width : {1200, 900}) {
        area.resize(width, 400);
        QCoreApplication::processEvents();
        auto* editor = area.findChild<ui::TimelineEditor*>();
        auto* ruler = editor->rulerForTest();
        auto* lanes = editor->laneRegionForTest();
        auto* header = area.findChild<QWidget*>("editorHeader");
        auto* name = area.findChild<QWidget*>("timelineCompositionName");
        auto* fullscreen = area.findChild<QToolButton*>("maximizeAreaButton");
        const int laneX = lanes->mapTo(&area, QPoint()).x();
        expectations.expect(header->isAncestorOf(ruler), "the ruler belongs to the area header");
        expectations.expect(ruler->mapTo(&area, QPoint()).x() == laneX &&
                                ruler->width() == lanes->width(),
                            "header ruler and lanes share both origin and width");
        expectations.expect(ruler->mapTo(header, QPoint()).y() >= 0 &&
                                ruler->mapTo(header, QPoint(0, ruler->height())).y() <=
                                    header->height(),
                            "the ruler fits entirely inside the header");
        expectations.expect(name != nullptr && header->isAncestorOf(name) && name->isVisible(),
                            "the composition name is visible in the header");
        expectations.expect(fullscreen->mapTo(&area, QPoint(fullscreen->width(), 0)).x() <= laneX,
                            "fullscreen stays in the left header cell");
        auto* columns = editor->findChild<QWidget*>("timelineColumnHeaderRow");
        expectations.expect(columns->mapTo(editor, QPoint()).y() == 0,
                            "column headings are the body's first row");
        expectations.expect(editor->takeHeaderRightWidget() == nullptr &&
                                editor->takeHeaderMenuWidget() == nullptr,
                            "both header transfers are idempotent");
    }
    expectations.expect(area.setEditorId("bloom.probe"), "the split editor can be replaced");
    expectations.expect(area.findChild<ui::TimelineRuler*>() == nullptr,
                        "replacement destroys the transferred ruler");
    expectations.expect(area.setEditorId("bloom.timeline"), "the split editor can be restored");
    finishFixture(fixture);
}

void testTimeViewportGestures(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Zoom and scroll"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});
    ui::TimelineEditor editor(fixture.session, fixture.controller);
    editor.resize(1200, 400);
    editor.show();
    QCoreApplication::processEvents();
    auto* ruler = editor.rulerForTest();
    auto* lanes = editor.laneRegionForTest();
    (void)fixture.session.setCurrentTime(time(3));
    expectations.expect(fixture.session.toggleKeyframe("opacity"), "seed an animated key lane");
    editor.layerStackForTest()->expansionRequested(
        editor.layerStackForTest()->entries().front().layerId);
    QCoreApplication::processEvents();
    const auto originalTime = fixture.session.currentTime();
    const auto revision = fixture.session.snapshot().revision();
    const auto getAxis = [ruler](int width = -1) {
        const auto result = ruler->axisForWidth(width < 0 ? ruler->width() : width);
        if (!result.has_value()) {
            throw std::runtime_error("Missing timeline test axis");
        }
        return *result;
    };
    const auto before = getAxis();
    const qreal cursor = ruler->width() * 0.4;
    const double anchorTime = before.secondsForPixel(cursor);
    auto wheel = [](QWidget& widget, qreal x, QPoint angle, Qt::KeyboardModifiers modifiers) {
        QWheelEvent event(QPointF(x, 10), widget.mapToGlobal(QPoint(static_cast<int>(x), 10)), {},
                          angle, Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&widget, &event);
    };
    wheel(*lanes, cursor, QPoint(0, 120), Qt::ControlModifier);
    auto axis = getAxis();
    expectations.expect(axis.t1 - axis.t0 < before.t1 - before.t0,
                        "Ctrl+wheel over lanes zooms the shared axis");
    expectations.expect(std::abs(axis.secondsForPixel(cursor) - anchorTime) < 1e-10,
                        "zoom preserves the cursor time");
    ruler->zoomToRange(2.0, 5.0);
    wheel(*ruler, cursor, QPoint(0, -120), Qt::ShiftModifier);
    axis = getAxis();
    expectations.expect(axis.t0 > 2.0 && std::abs(axis.t1 - axis.t0 - 3.0) < 1e-10,
                        "Shift+wheel scrolls without changing the visible span");
    const double priorStart = axis.t0;
    wheel(*lanes, cursor, QPoint(120, 0), Qt::NoModifier);
    expectations.expect(getAxis().t0 < priorStart, "horizontal wheel scrolls the axis");
    ruler->zoomToRange(2.0, 5.0);
    auto* keys = editor.findChild<ui::TimelineKeyframePanel*>();
    const auto keyRows = keys->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    expectations.expect(!keyRows.isEmpty(), "an animated key row is present");
    if (!keyRows.isEmpty()) {
        auto* keyRow = keyRows.at(4);
        const auto keyAxis = getAxis(keyRow->width());
        sendMouse(*keyRow, QEvent::MouseButtonPress, keyAxis.pixelForTime(time(3)),
                  keyRow->height() / 2.0);
        expectations.expect(
            std::holds_alternative<ui::KeyframeSelection>(fixture.session.selection().primary),
            "keyframe hit testing follows the zoomed shared axis");
        sendMouse(*keyRow, QEvent::MouseButtonRelease, keyAxis.pixelForTime(time(3)),
                  keyRow->height() / 2.0);
    }
    const auto rects = ruler->majorTickLabelRectsForTest();
    for (std::size_t i = 1; i < rects.size(); ++i) {
        expectations.expect(rects[i - 1].right() < rects[i].left(),
                            "zoomed and scrolled tick labels do not overlap");
    }
    auto* navigator =
        dynamic_cast<ui::TimelineNavigator*>(editor.findChild<QWidget*>("timelineNavigator"));
    expectations.expect(navigator != nullptr &&
                            navigator->height() == ui::kit::px(ui::kit::Size::Control),
                        "the navigator uses the Control row height");
    if (navigator != nullptr) {
        const auto window = navigator->windowRect();
        sendMouse(*navigator, QEvent::MouseButtonPress, window.center().x(), window.center().y());
        sendMouse(*navigator, QEvent::MouseMove, window.center().x() + 30, window.center().y());
        expectations.expect(getAxis().t0 > 2.0, "dragging the navigator window pans");
        QAction applicationEscape(&editor);
        applicationEscape.setShortcut(QKeySequence(Qt::Key_Escape));
        editor.addAction(&applicationEscape);
        bool escapedApplication = false;
        QObject::connect(&applicationEscape, &QAction::triggered, &editor,
                         [&escapedApplication] { escapedApplication = true; });
        editor.activateWindow();
        navigator->setFocus();
        QCoreApplication::processEvents();
        QTest::keyClick(navigator, Qt::Key_Escape);
        expectations.expect(!escapedApplication,
                            "a navigator drag owns Escape before application commands");
        expectations.expect(std::abs(getAxis().t0 - 2.0) < 1e-10,
                            "Escape restores the original viewport");
        sendMouse(*navigator, QEvent::MouseButtonPress, window.right(), window.center().y());
        sendMouse(*navigator, QEvent::MouseButtonRelease, window.right() - 30, window.center().y());
        axis = getAxis();
        expectations.expect(axis.t0 == 2.0 && axis.t1 < 5.0,
                            "resizing the navigator end preserves the start");
        const auto resized = navigator->windowRect();
        sendMouse(*navigator, QEvent::MouseButtonPress, resized.left(), resized.center().y());
        sendMouse(*navigator, QEvent::MouseButtonRelease, resized.left() + 10,
                  resized.center().y());
        expectations.expect(getAxis().t0 > 2.0, "the navigator start is independently resizable");
    }
    ruler->zoomToRange(-100.0, 100.0);
    axis = getAxis();
    expectations.expect(axis.t0 == 0 && axis.t1 == before.duration.toSeconds(),
                        "ranges clamp to the full composition duration");
    ruler->zoomToRange(2.0, 2.000001);
    axis = getAxis();
    expectations.expect(axis.t1 - axis.t0 >= 1.0 / 24.0 - 1e-10,
                        "zoom clamps at one visible frame");
    expectations.expect(axis.pixelForTime(time(0)) < 0,
                        "offscreen times are clipped rather than pinned to the edge");
    ruler->zoomToFit();
    axis = getAxis();
    expectations.expect(axis.t0 == before.t0 && axis.t1 == before.t1,
                        "Zoom to Fit restores the entire duration");
    expectations.expect(fixture.session.currentTime() == originalTime &&
                            fixture.session.snapshot().revision() == revision,
                        "all navigation is presentation-only");
    finishFixture(fixture);
}

void testTimelineHeaderMenus(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Timeline menu commands"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});
    (void)fixture.session.addSolidLayer(QStringLiteral("B"), core::Color4d{0.2, 0.3, 0.4, 1.0});
    QWidget host;
    auto* appUndo = new QAction("Undo", &host);
    appUndo->setObjectName("undoAction");
    appUndo->setShortcut(QKeySequence::Undo);
    QObject::connect(appUndo, &QAction::triggered, &fixture.session, &ui::CompositionSession::undo);
    auto* appRedo = new QAction("Redo", &host);
    appRedo->setObjectName("redoAction");
    appRedo->setShortcut(QKeySequence::Redo);
    QObject::connect(appRedo, &QAction::triggered, &fixture.session, &ui::CompositionSession::redo);
    ui::EditorRegistry registry;
    (void)registry.registerEditor({"bloom.timeline", "Timeline", [&](QWidget* parent) {
                                       return new ui::TimelineEditor(
                                           fixture.session, fixture.controller, nullptr, parent);
                                   }});
    auto* area = new ui::EditorArea(registry, "bloom.timeline");
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(area);
    layoutEditor(host);
    auto* editor = area->findChild<ui::TimelineEditor*>();
    auto* edit = area->findChild<QMenu*>("timelineEditMenu");
    auto* header = area->findChild<QWidget*>("editorHeader");
    expectations.expect(edit != nullptr && edit->actions().size() == 4 &&
                            edit->actions()[0] == appUndo && edit->actions()[1] == appRedo &&
                            edit->actions()[3]->objectName() ==
                                QStringLiteral("timelineSplitLayerAction"),
                        "header Edit uses the application's actual Undo and Redo QActions");
    auto* addButton = area->findChild<QToolButton*>("addLayerButton");
    expectations.expect(addButton != nullptr && header->isAncestorOf(addButton) &&
                            addButton->menu()->objectName() == QStringLiteral("addLayerMenu"),
                        "the existing Add popup is in the header with its original names");
    auto* controls = editor->findChild<QWidget*>("timelineControls");
    for (const auto* button : controls->findChildren<QToolButton*>()) {
        expectations.expect(button->text() != QStringLiteral("Undo") &&
                                button->text() != QStringLiteral("Redo"),
                            "transport has no Undo or Redo buttons");
    }
    const auto action = [area](const char* name) {
        auto* result = area->findChild<QAction*>(QLatin1String(name));
        if (result == nullptr) {
            throw std::runtime_error(std::string("Missing action ") + name);
        }
        return result;
    };
    action("timelineSelectAllAction")->trigger();
    expectations.expect(fixture.session.selectedNodes().size() == 2,
                        "Select All selects every layer boundary in the shared session");
    action("timelineSelectNoneAction")->trigger();
    expectations.expect(fixture.session.selectedNodes().empty() &&
                            !fixture.session.selection().contextualLayer.has_value(),
                        "Select None clears the shared selection");
    expectations.expect(!action("timelineDeleteLayerAction")->isEnabled(),
                        "Delete Layer is disabled without a selected layer");
    host.activateWindow();
    editor->layerStackForTest()->setFocus();
    QCoreApplication::processEvents();
    QTest::keyClick(editor->layerStackForTest(), Qt::Key_A, Qt::ControlModifier);
    expectations.expect(fixture.session.selectedNodes().size() == 2,
                        "Ctrl+A selects all layers from the stack");
    const auto nodeIds = fixture.session.selectedNodes();
    QTest::keyClick(editor->layerStackForTest(), Qt::Key_Delete);
    expectations.expect(editor->layerStackForTest()->rowCount() == 0,
                        "Delete Layer removes all selected structured layer boundaries");
    appUndo->trigger();
    expectations.expect(editor->layerStackForTest()->rowCount() == 2,
                        "the app Undo restores every deleted layer in one transaction");
    bool restored = true;
    for (const auto id : nodeIds) {
        restored = restored && fixture.session.composition()->graph().findNode(id) != nullptr;
    }
    expectations.expect(restored, "undo restores the same stable boundary IDs");
    appRedo->trigger();
    expectations.expect(editor->layerStackForTest()->rowCount() == 0,
                        "the app Redo removes the restored boundaries again");
    appUndo->trigger();

    auto* ruler = editor->rulerForTest();
    action("timelineZoomInAction")->trigger();
    const auto zoom = ruler->axisForWidth(ruler->width());
    expectations.expect(zoom.has_value() && zoom->t0 > 0.0,
                        "View Zoom In uses the shared centered viewport zoom");
    ruler->setFocus();
    QCoreApplication::processEvents();
    QTest::keyClick(ruler, Qt::Key_0, Qt::ControlModifier);
    const auto fit = ruler->axisForWidth(ruler->width());
    expectations.expect(fit.has_value() && fit->t0 == 0.0 && fit->t1 == 10.0,
                        "View Zoom to Fit restores the full duration");
    expectations.expect(!action("timelineZoomToFitAction")->shortcut().isEmpty() &&
                            !action("timelineDeleteLayerAction")->shortcut().isEmpty() &&
                            !action("timelineSelectAllAction")->shortcut().isEmpty(),
                        "menus expose their keyboard shortcuts");
    (void)fixture.session.setCurrentTime(time(3));
    action("timelineTimecodeAction")->trigger();
    auto* readout = editor->findChild<QLabel*>("timelineTimeReadout");
    expectations.expect(readout->text().contains(QStringLiteral("00:00:03:00")) &&
                            readout->text().contains(QStringLiteral("3.000s")),
                        "timecode readout preserves the exact seconds alongside non-drop timecode");
    expectations.expect(QSettings().value("timeline/time-format").toString() ==
                            QStringLiteral("timecode"),
                        "the display format is persisted under timeline/time-format");
    for (const auto width : {180, 900}) {
        ruler->resize(width, ruler->height());
        const auto rects = ruler->majorTickLabelRectsForTest();
        for (std::size_t i = 1; i < rects.size(); ++i) {
            expectations.expect(rects[i - 1].right() < rects[i].left(),
                                "timecode tick labels never overlap at narrow or wide sizes");
        }
    }
    {
        ui::TimelineEditor restoredEditor(fixture.session, fixture.controller);
        expectations.expect(restoredEditor.findChild<QLabel*>("timelineTimeReadout")->text() ==
                                readout->text(),
                            "a new timeline restores the persisted time format");
    }
    action("timelineFramesAction")->trigger();
    expectations.expect(readout->text() == QStringLiteral("Frame 72 · 3.000s"),
                        "Frames restores the existing frame and exact-time readout");
    auto* bar = area->findChild<QWidget*>("timelineHeaderMenus");
    bar->setFixedWidth(ui::kit::px(ui::kit::Size::Control));
    QCoreApplication::processEvents();
    auto* overflow = area->findChild<QToolButton*>("timelineHeaderOverflowButton");
    expectations.expect(overflow->isVisible() && overflow->menu()->actions().size() == 4,
                        "a narrow header collapses all four menus into the overflow popup");
    finishFixture(fixture);
}

// The playhead is ONE 1px Accent line at ONE x, through the ruler and through every lane, with its
// single head marker in the work-area row above.
void testPlayheadSpansRulerAndEveryLane(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Playhead Span Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});
    (void)fixture.session.addSolidLayer(QStringLiteral("B"), core::Color4d{0.2, 0.3, 0.4, 1.0});

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);

    auto* lanes = editor->laneRegionForTest();
    auto* ruler = editor->rulerForTest();
    auto* workArea = editor->findChild<ui::TimelineWorkAreaRow*>("timelineWorkAreaRow");
    if (lanes == nullptr || ruler == nullptr || workArea == nullptr) {
        expectations.expect(false, "the time-axis widgets exist");
        delete editor;
        finishFixture(fixture);
        return;
    }

    // Mid-composition, so the line cannot accidentally coincide with either edge.
    expectations.expect(fixture.session.setCurrentTime(time(5)), "session time moves to 5s");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    const QImage rulerImage = ruler->grab().toImage();
    const QImage laneImage = lanes->grab().toImage();
    const QImage workAreaImage = workArea->grab().toImage();

    const int rulerX = accentRunCenter(rulerImage, rulerImage.height() - 2);
    const int rowHeight = ui::kTimelineRowHeight;
    const int firstLaneX = accentRunCenter(laneImage, rowHeight / 2);
    const int secondLaneX = accentRunCenter(laneImage, rowHeight + rowHeight / 2);
    const int belowLastLaneX = accentRunCenter(laneImage, 2 * rowHeight + rowHeight / 2);
    const int markerX = accentRunCenter(workAreaImage, workAreaImage.height() - 5);

    expectations.expect(rulerX >= 0, "the ruler paints the Accent playhead line");
    expectations.expect(firstLaneX >= 0 && secondLaneX >= 0,
                        "every lane paints the playhead line, not just the first");
    expectations.expect(belowLastLaneX >= 0,
                        "the line continues through the empty lane area below the last layer -- it "
                        "spans the whole region, not only the occupied rows");
    expectations.expect(rulerX == firstLaneX && firstLaneX == secondLaneX &&
                            secondLaneX == belowLastLaneX,
                        "the ruler and every lane put the line at exactly the same x");
    expectations.expect(markerX == rulerX,
                        "the head marker above the ruler sits at the same x as the line it heads");

    delete editor;
    finishFixture(fixture);
}

// Row height, flat rows, hairline separators.
void testRowsAreFlatThirtyTwoPixelRows(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Row Height Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});
    (void)fixture.session.addSolidLayer(QStringLiteral("B"), core::Color4d{0.2, 0.3, 0.4, 1.0});
    (void)fixture.session.addSolidLayer(QStringLiteral("C"), core::Color4d{0.2, 0.3, 0.4, 1.0});

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);

    auto* stack = editor->layerStackForTest();
    auto* lanes = editor->laneRegionForTest();
    if (stack == nullptr || lanes == nullptr) {
        expectations.expect(false, "the two halves exist");
        delete editor;
        finishFixture(fixture);
        return;
    }

    expectations.expect(ui::kTimelineRowHeight == 32,
                        "the row pitch is the 32px the design mock specifies");
    expectations.expect(stack->rowCount() == 3, "all three layers have rows");
    expectations.expect(stack->rowTop(1) - stack->rowTop(0) == 32 &&
                            lanes->rowTop(1) - lanes->rowTop(0) == 32,
                        "both halves step by exactly 32px per row");
    expectations.expect(stack->rowTop(0) == lanes->rowTop(0),
                        "row 0 starts at the same y in both halves");

    const auto rows = editor->findChildren<QWidget*>(QStringLiteral("timelineLayerRow"));
    expectations.expect(!rows.isEmpty(), "the column materializes real row widgets");
    for (auto* row : rows) {
        if (!row->isVisible()) {
            continue;
        }
        expectations.expect(row->height() == 32, "every visible row widget is 32px tall");
        expectations.expect(row->width() == ui::TimelineEditor::layerColumnWidth(),
                            "every row spans the whole layer column");
    }

    // Flat, not striped: two adjacent UNSELECTED rows paint the same background. ADAPTED (task
    // FIX1, item E): a new layer lands on TOP, so the row addSolidLayer selected is row 0 and the
    // honest unselected pair is rows 1 and 2.
    const QImage laneImage = lanes->grab().toImage();
    const QColor background = ui::kit::color(ui::kit::Color::Background);
    const int sampleX = laneImage.width() - 4;
    expectations.expect(near(laneImage.pixelColor(sampleX, 32 + 1), background, 2) &&
                            near(laneImage.pixelColor(sampleX, 64 + 1), background, 2),
                        "unselected lanes are FLAT Background -- no alternating stripe");

    // The hairline separator closes each row, in Border.
    const QColor border = ui::kit::color(ui::kit::Color::Border);
    expectations.expect(near(laneImage.pixelColor(sampleX, 31), border, 6),
                        "a Border hairline closes the row at its last pixel row");

    delete editor;
    finishFixture(fixture);
}

// ONE scrollbar moves both halves by exactly the same amount: shared scroll is structural here.
void testOneScrollbarMovesBothHalvesTogether(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Shared Scroll Test"));
    for (int index = 0; index < 20; ++index) {
        (void)fixture.session.addSolidLayer(QStringLiteral("L%1").arg(index),
                                            core::Color4d{0.2, 0.3, 0.4, 1.0});
    }

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host, 1200, 260);

    auto* stack = editor->layerStackForTest();
    auto* lanes = editor->laneRegionForTest();
    auto* bar = editor->verticalScrollBarForTest();
    if (stack == nullptr || lanes == nullptr || bar == nullptr) {
        expectations.expect(false, "the two halves and their shared scrollbar exist");
        delete editor;
        finishFixture(fixture);
        return;
    }

    expectations.expect(stack->rowCount() == 20, "all twenty layers have rows");
    expectations.expect(bar->maximum() > 0,
                        "twenty 32px rows do not fit the viewport, so the scrollbar has range");
    expectations.expect(stack->contentHeight() == 20 * 32 && lanes->contentHeight() == 20 * 32,
                        "both halves agree on the content height");

    // The twentieth layer is the selected one, so the panel has already scrolled it into view --
    // start from a known offset rather than assuming zero.
    bar->setValue(0);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    expectations.expect(stack->rowTop(5) == 5 * 32 && lanes->rowTop(5) == 5 * 32,
                        "at offset zero both halves put row 5 at its unscrolled y");

    const int offset = std::min(bar->maximum(), 3 * 32);
    expectations.expect(offset == 3 * 32, "the fixture really can scroll three rows (sanity)");
    bar->setValue(offset);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    expectations.expect(stack->rowTop(5) == 5 * 32 - offset && lanes->rowTop(5) == 5 * 32 - offset,
                        "ONE scrollbar value moved BOTH halves by exactly the same amount");
    expectations.expect(stack->rowTop(5) == lanes->rowTop(5),
                        "the two halves stay in step after scrolling -- shared scroll, not synced "
                        "scroll");

    expectations.expect(stack->rowTop(3) == 0 && lanes->rowTop(3) == 0,
                        "row 3 is now the row at the very top of both halves");
    const auto rows = editor->findChildren<QWidget*>(QStringLiteral("timelineLayerRow"));
    bool sawRowAtTop = false;
    for (auto* row : rows) {
        if (row->isVisible() && row->y() == 0) {
            sawRowAtTop = true;
        }
    }
    expectations.expect(sawRowAtTop,
                        "a pooled row widget really moved to the top -- the left column scrolled, "
                        "it did not merely report a new offset");

    delete editor;
    finishFixture(fixture);
}

// The clip bar: rounded, on its lane, spanning the composition range (there is no trim feature, so
// that IS the honest extent), in the documented data-type color.
void testClipBarSpansTheCompositionRangeInItsDataTypeColor(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Clip Bar Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});
    (void)fixture.session.addTextLayer(QStringLiteral("B"), QStringLiteral("Text"));

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);

    auto* lanes = editor->laneRegionForTest();
    if (lanes == nullptr) {
        expectations.expect(false, "the lane region exists");
        delete editor;
        finishFixture(fixture);
        return;
    }

    const QImage laneImage = lanes->grab().toImage();
    // ADAPTED (task S3): task T1 painted ONE data-type color for both kinds and disclosed that as a
    // blocked sub-item, because text was not a rendering layer kind yet. It is now, so the two
    // kinds are distinguishable on the lane. ADAPTED again (task FIX1, item E): a new layer lands
    // on TOP, so the Text layer added second is row 0 (DataClip) and the Solid is row 1
    // (DataComposition). See layerClipColorToken() for why those two roles and not the others.
    const std::array<QColor, 2> expectedByRow{ui::kit::color(ui::kit::Color::DataClip),
                                              ui::kit::color(ui::kit::Color::DataComposition)};
    expectations.expect(expectedByRow[0] != expectedByRow[1],
                        "the two layer kinds really do get different clip colors");
    for (int row = 0; row < 2; ++row) {
        const QColor expected = expectedByRow[static_cast<std::size_t>(row)];
        const auto bar = lanes->clipBarRect(row);
        expectations.expect(bar.has_value(), "the row has a clip bar rect");
        if (!bar.has_value()) {
            continue;
        }
        expectations.expect(bar->left() == 0 && bar->right() >= lanes->width() - 2,
                            "the bar spans the whole composition range on its lane -- no trim "
                            "feature exists, so a partial bar would be a fiction");
        expectations.expect(bar->height() == 32 - 2 * ui::kit::px(ui::kit::Spacing::XS),
                            "the bar is inset inside its 32px lane rather than filling it edge to "
                            "edge");
        const int sampleY = bar->top() + bar->height() / 2;
        const int sampleX = bar->left() + bar->width() / 2;
        expectations.expect(near(laneImage.pixelColor(sampleX, sampleY), expected, 6),
                            "the bar paints its own layer kind's documented data-type color");
    }

    delete editor;
    finishFixture(fixture);
}

// The selected row is a SurfaceRaised ROW FILL across both halves -- never the accent-outlined
// cells the owner rejected.
void testSelectedRowIsASurfaceRaisedFillNotAnAccentOutline(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Selection Fill Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});
    (void)fixture.session.addSolidLayer(QStringLiteral("B"), core::Color4d{0.2, 0.3, 0.4, 1.0});

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);

    auto* stack = editor->layerStackForTest();
    auto* lanes = editor->laneRegionForTest();
    if (stack == nullptr || lanes == nullptr) {
        expectations.expect(false, "the two halves exist");
        delete editor;
        finishFixture(fixture);
        return;
    }
    // addSolidLayer selects what it adds, and ADAPTED (task FIX1, item E) a new layer lands on TOP,
    // so row 0 is selected and row 1 is not.
    expectations.expect(stack->currentRow() == 0, "row 0 is the selected row (test precondition)");

    const QColor raised = ui::kit::color(ui::kit::Color::SurfaceRaised);
    const QColor background = ui::kit::color(ui::kit::Color::Background);
    const QColor accent = ui::kit::color(ui::kit::Color::Accent);

    const QImage laneImage = lanes->grab().toImage();
    // Sampled near the right edge (away from the playhead sitting at time 0 on the left edge) and
    // in the lane's own 2px vertical inset, ABOVE the clip bar -- the bar spans the whole
    // composition range, so every x inside the bar's own band shows the bar, not the row fill.
    const int sampleX = laneImage.width() - 3;
    const QColor selectedLane = laneImage.pixelColor(sampleX, 1);
    const QColor unselectedLane = laneImage.pixelColor(sampleX, 32 + 1);
    expectations.expect(near(selectedLane, raised, 4),
                        "the selected row's lane is a SurfaceRaised fill");
    expectations.expect(near(unselectedLane, background, 4),
                        "an unselected row's lane stays Background");
    expectations.expect(!near(selectedLane, accent, 24),
                        "the selected row is NOT accent-filled or accent-outlined");

    const auto rows = editor->findChildren<QWidget*>(QStringLiteral("timelineLayerRow"));
    bool sawRaisedRow = false;
    bool sawPlainRow = false;
    for (auto* row : rows) {
        if (!row->isVisible()) {
            continue;
        }
        const QImage rowImage = row->grab().toImage();
        // Between the toggle strip and the Name cell: no glyph, no text, just the row's own fill.
        const QColor fill = rowImage.pixelColor(rowImage.width() - 3, 16);
        if (near(fill, raised, 4)) {
            sawRaisedRow = true;
        }
        if (near(fill, background, 4)) {
            sawPlainRow = true;
        }
        expectations.expect(!near(fill, accent, 24),
                            "no row paints an Accent fill or edge in the left column either");
    }
    expectations.expect(sawRaisedRow && sawPlainRow,
                        "the left column shows exactly the same selected/unselected fills as the "
                        "lane beside it, so one row reads as one row");

    delete editor;
    finishFixture(fixture);
}

// The four reserved toggle columns, and the honesty rule they exist under.
void testToggleColumnsCommitLayerFlags(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Toggle Honesty Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);

    auto* stack = editor->layerStackForTest();
    auto* headers = editor->findChild<ui::TimelineColumnHeaders*>("timelineColumnHeaders");
    if (stack == nullptr || headers == nullptr) {
        expectations.expect(false, "the layer column and its column headers exist");
        delete editor;
        finishFixture(fixture);
        return;
    }

    // The cell table is shared by the headers and the rows, so one x answers for both.
    const int toggleWidth =
        ui::kit::px(ui::kit::Size::IconMedium) + ui::kit::px(ui::kit::Spacing::XS);
    const int padding = ui::kit::px(ui::kit::Spacing::XS);
    static constexpr std::array<const char*, 3> kFragments{"visibility", "solo", "lock"};
    for (int index = 0; index < 3; ++index) {
        const int x = padding + index * toggleWidth + toggleWidth / 2;
        const QString headerTip = ui::TimelineColumnHeaders::toolTipAtX(x);
        const QString rowTip = stack->toolTipAt(QPoint(x, 16));
        expectations.expect(headerTip == rowTip,
                            "a toggle column explains itself identically in the header and in a "
                            "row -- one table, one reason");
        expectations.expect(!headerTip.isEmpty(), "a toggle column explains itself");
        expectations.expect(
            headerTip.contains(QLatin1StringView(kFragments[static_cast<std::size_t>(index)]),
                               Qt::CaseInsensitive),
            "a toggle column names its command");
    }

    const auto layerId = stack->entries().front().layerId;
    const auto before = fixture.commands.size();
    for (int index = 0; index < 3; ++index)
        QTest::mouseClick(stack, Qt::LeftButton, Qt::NoModifier,
                          QPoint(padding + index * toggleWidth + toggleWidth / 2, 16));
    const auto* layer = fixture.session.composition()->graph().findLayer(layerId);
    expectations.expect(layer && !layer->enabled && layer->solo && layer->locked &&
                            fixture.commands.size() == before + 3,
                        "eye, solo and lock each commit once to the bound layer");
    (void)fixture.session.undo();
    expectations.expect(!fixture.session.composition()->graph().findLayer(layerId)->locked,
                        "lock undoes once");
    (void)fixture.session.redo();
    expectations.expect(fixture.session.composition()->graph().findLayer(layerId)->locked,
                        "lock redoes once");

    delete editor;
    finishFixture(fixture);
}

// The Blending row authors the layer it DRAWS, not the selection: a row is re-pointed on every
// scroll step and must never have to move the selection to change a layer's blending. With two
// layers selected as one, picking a mode in the second row must reach the second layer.
void testBlendingRowAuthorsTheLayerItDraws(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Blending Commit Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("Top"), core::Color4d{0.2, 0.3, 0.4, 1.0});
    (void)fixture.session.addSolidLayer(QStringLiteral("Bottom"),
                                        core::Color4d{0.4, 0.3, 0.2, 1.0});

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);

    const auto entries = fixture.session.composition()->graph().layerStack().entries();
    const auto rows = editor->findChildren<ui::kit::KDropdown*>("layerBlendingDropdown");
    expectations.expect(entries.size() == 2 && rows.size() >= 2,
                        "two stacked layers give two pooled rows, each with its own dropdown");
    if (entries.size() != 2 || rows.size() < 2) {
        delete editor;
        finishFixture(fixture);
        return;
    }
    // The pool is filled top-down from the first visible row, so rows[i] draws entries[i].
    const auto secondLayer = entries[1].layerId;
    fixture.session.selectLayer(entries[0].layerId);

    int multiplyRow = -1;
    for (int index = 0; index < rows[1]->count(); ++index) {
        if (rows[1]->itemData(index).value<std::int64_t>() ==
            core::blendModeStoredValue(core::BlendMode::Multiply)) {
            multiplyRow = index;
        }
    }
    expectations.expect(multiplyRow >= 0, "the vocabulary includes Multiply");
    if (multiplyRow < 0) {
        delete editor;
        finishFixture(fixture);
        return;
    }

    rows[1]->setCurrentIndex(multiplyRow);
    expectations.expect(fixture.session.blendModeForLayer(secondLayer) == core::BlendMode::Multiply,
                        "the second row authors the second layer, not the selected one");
    expectations.expect(fixture.session.blendModeForLayer(entries[0].layerId) ==
                            core::kDefaultBlendMode,
                        "and leaves the selected layer's own blending alone");
    expectations.expect(fixture.session.canUndo() && fixture.session.undo() &&
                            fixture.session.blendModeForLayer(secondLayer) ==
                                core::kDefaultBlendMode,
                        "the edit is one undoable command");
    // Rebuilding off snapshotChanged is what makes undo visible in the row itself rather than only
    // in the document.
    const auto refreshed = editor->findChildren<ui::kit::KDropdown*>("layerBlendingDropdown");
    expectations.expect(refreshed.size() >= 2 &&
                            refreshed[1]->currentText() ==
                                ui::blendModeDisplayName(core::kDefaultBlendMode),
                        "and the row follows the undone document");

    delete editor;
    finishFixture(fixture);
}

// ADAPTED (blend modes): this pinned BOTH dropdowns as disabled placeholders carrying one honest
// value. Blending is a real Layer Output parameter with a real command behind it now, so what is
// pinned for it is the opposite property -- enabled, carrying the whole implemented vocabulary in
// core::kBlendModes order, starting at the layer's authored mode. Parent is still a placeholder and
// its half of the case is unchanged; the row-height assertion covers both exactly as before.
void testBlendingAndParentAreDisabledKDropdowns(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Blending Parent Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);

    auto* blending = editor->findChild<ui::kit::KDropdown*>("layerBlendingDropdown");
    auto* parent = editor->findChild<ui::kit::KDropdown*>("layerParentDropdown");
    expectations.expect(blending != nullptr && parent != nullptr,
                        "the row carries a Blending and a Parent KDropdown");
    if (blending == nullptr || parent == nullptr) {
        delete editor;
        finishFixture(fixture);
        return;
    }

    expectations.expect(blending->isEnabled() &&
                            blending->count() == static_cast<int>(bloom::core::kBlendModes.size()),
                        "Blending is enabled and offers every implemented blend mode");
    bool vocabularyInOrder = blending->count() == static_cast<int>(core::kBlendModes.size());
    for (std::size_t index = 0; index < core::kBlendModes.size() && vocabularyInOrder; ++index) {
        vocabularyInOrder = blending->itemText(static_cast<int>(index)) ==
                                ui::blendModeDisplayName(core::kBlendModes[index]) &&
                            blending->itemData(static_cast<int>(index)).value<std::int64_t>() ==
                                core::blendModeStoredValue(core::kBlendModes[index]);
    }
    expectations.expect(vocabularyInOrder,
                        "each row carries its mode's shared display name and stored value, in "
                        "core::kBlendModes order");
    expectations.expect(blending->currentText() ==
                            ui::blendModeDisplayName(core::kDefaultBlendMode),
                        "a newly created layer's row starts at its authored Normal");
    expectations.expect(!blending->toolTip().isEmpty(), "Blending explains what it does");
    expectations.expect(!parent->isEnabled() && parent->currentText() == QStringLiteral("None"),
                        "Parent shows the one honest value \"None\", disabled");
    expectations.expect(!parent->toolTip().isEmpty(), "Parent's disabled state is explained");
    expectations.expect(blending->height() <= 32 && parent->isHidden(),
                        "both fit inside the 32px row rather than forcing it taller");

    delete editor;
    finishFixture(fixture);
}

// No Kind column -- and kind is still readable, through the row's own tooltip.
void testKindHasNoColumnButStaysReadable(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Kind Readability Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});
    (void)fixture.session.addTextLayer(QStringLiteral("B"), QStringLiteral("Text"));

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);

    auto* stack = editor->layerStackForTest();
    if (stack == nullptr || stack->rowCount() != 2) {
        expectations.expect(false, "both layers have rows");
        delete editor;
        finishFixture(fixture);
        return;
    }

    // ADAPTED (task FIX1, item E): the Text layer added second is on TOP, so it is row 0.
    expectations.expect(stack->entries()[0].kind == QStringLiteral("Text") &&
                            stack->entries()[1].kind == QStringLiteral("Solid"),
                        "the stack still derives each layer's kind from project truth");
    // The Name cell's own x: past the four toggle cells.
    const int nameX =
        ui::kit::px(ui::kit::Spacing::XS) +
        4 * (ui::kit::px(ui::kit::Size::IconMedium) + ui::kit::px(ui::kit::Spacing::XS)) +
        ui::kit::px(ui::kit::Spacing::XS) + 8;
    expectations.expect(stack->toolTipAt(QPoint(nameX, 32 + 16)).contains(QStringLiteral("Solid")),
                        "the Solid row names its kind in the tooltip, so removing the Kind COLUMN "
                        "never removed the information");
    expectations.expect(stack->toolTipAt(QPoint(nameX, 16)).contains(QStringLiteral("Text")),
                        "the Text row does the same");

    delete editor;
    finishFixture(fixture);
}

// The primitive itself: the stack is no longer an item view, and its objectName survives the
// change.
void testLayerStackIsNoLongerAnItemView(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Primitive Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);

    expectations.expect(editor->findChildren<QTreeView*>().isEmpty(),
                        "the panel contains no QTreeView/QTreeWidget at all");
    expectations.expect(editor->findChildren<QTableView*>().isEmpty(),
                        "nor a QTableView/QTableWidget");
    expectations.expect(editor->findChild<QWidget*>("layerStackView") != nullptr,
                        "the layerStackView objectName still resolves -- same role, new primitive");
    expectations.expect(editor->findChild<QWidget*>("layerStackView") ==
                            editor->layerStackForTest(),
                        "and it resolves to the layer stack itself");

    delete editor;
    finishFixture(fixture);
}

// Hundreds of rows: the widget count stays bounded by the viewport, and moving the playhead never
// relayouts a single row.
void testManyRowsStayBoundedAndThePlayheadNeverRelayoutsThem(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Many Rows Test"));
    constexpr int kLayerCount = 120;
    for (int index = 0; index < kLayerCount; ++index) {
        (void)fixture.session.addSolidLayer(QStringLiteral("L%1").arg(index),
                                            core::Color4d{0.2, 0.3, 0.4, 1.0});
    }

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host, 1200, 300);

    auto* stack = editor->layerStackForTest();
    if (stack == nullptr) {
        expectations.expect(false, "the layer column exists");
        delete editor;
        finishFixture(fixture);
        return;
    }

    expectations.expect(stack->rowCount() == kLayerCount, "every layer has a row");
    const auto rows = editor->findChildren<QWidget*>(QStringLiteral("timelineLayerRow"));
    const int bound = stack->height() / 32 + 2;
    expectations.expect(rows.size() <= bound, "the row widget pool is bounded by the viewport (" +
                                                  std::to_string(rows.size()) + " widgets for " +
                                                  std::to_string(kLayerCount) + " layers, bound " +
                                                  std::to_string(bound) + ")");
    expectations.expect(rows.size() >= 2, "the pool is not empty either (test sanity)");
    expectations.expect(editor->findChildren<ui::kit::KDropdown*>().size() == 2 * rows.size(),
                        "two KDropdowns per pooled row -- not two per LAYER, which is the whole "
                        "reason the pool exists");

    // A fixed buffer rather than a growing container: the pool is bounded by the viewport, so the
    // count is known to be small, and main() in this suite must stay non-throwing.
    static constexpr int kMaxSampledRows = 64;
    std::array<QRect, kMaxSampledRows> before{};
    const int sampled = static_cast<int>(std::min<qsizetype>(rows.size(), kMaxSampledRows));
    for (int index = 0; index < sampled; ++index) {
        before[static_cast<std::size_t>(index)] = rows[index]->geometry();
    }
    expectations.expect(fixture.session.setCurrentTime(time(3)), "the playhead moves");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    expectations.expect(fixture.session.setCurrentTime(time(7)), "and moves again");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    bool unchanged = true;
    for (int index = 0; index < sampled; ++index) {
        if (rows[index]->geometry() != before[static_cast<std::size_t>(index)]) {
            unchanged = false;
        }
    }
    expectations.expect(unchanged,
                        "moving the playhead relayouts nothing -- it is one repaint of the lane "
                        "region, never a per-frame row relayout");

    delete editor;
    finishFixture(fixture);
}

// Dragging a lane scrubs, through the ruler's own scrub path and therefore its exact frame math.
void testDraggingALaneScrubsThroughTheRulerScrubPath(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Lane Scrub Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);

    auto* lanes = editor->laneRegionForTest();
    const auto* composition = fixture.session.composition();
    if (lanes == nullptr || composition == nullptr) {
        expectations.expect(false, "the lane region and the composition exist");
        delete editor;
        finishFixture(fixture);
        return;
    }

    const auto axis = ui::TimelineAxis::create(*composition, lanes->width());
    expectations.expect(axis.has_value(), "the lane region resolves the shared time axis");
    if (!axis.has_value()) {
        delete editor;
        finishFixture(fixture);
        return;
    }

    const int pressX = lanes->width() / 3;
    const auto pressTime =
        ui::frameTimeForIndex(axis->frameRate, axis->duration, axis->frameIndexForPixel(pressX));
    const int moveX = lanes->width() / 2;
    const auto moveTime =
        ui::frameTimeForIndex(axis->frameRate, axis->duration, axis->frameIndexForPixel(moveX));
    expectations.expect(pressTime.has_value() && moveTime.has_value() && *pressTime != *moveTime,
                        "the two sample pixels map to two different exact frame times (sanity)");
    if (!pressTime.has_value() || !moveTime.has_value()) {
        delete editor;
        finishFixture(fixture);
        return;
    }

    sendMouse(*lanes, QEvent::MouseButtonPress, pressX, 1.0);
    expectations.expect(fixture.session.currentTime() == *pressTime,
                        "pressing a lane lands on that pixel's EXACT frame time, the same mapping "
                        "the ruler scrub uses");
    sendMouse(*lanes, QEvent::MouseMove, moveX, 1.0);
    expectations.expect(fixture.session.currentTime() == *moveTime,
                        "dragging across the lanes keeps scrubbing");
    sendMouse(*lanes, QEvent::MouseButtonRelease, moveX, 1.0);
    expectations.expect(fixture.session.currentTime() == *moveTime,
                        "releasing leaves the scrubbed time in place");
    expectations.expect(waitUntilReady(fixture), "scrub-end still reaches a ready preview frame -- "
                                                 "the lane drag armed and disarmed the SAME "
                                                 "interactive cadence the ruler arms");

    delete editor;
    finishFixture(fixture);
}

// Clicking a row in the LEFT column selects that layer; clicking below the last row clears it.
void testClickingTheLeftColumnSelectsAndClears(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Row Selection Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});
    (void)fixture.session.addSolidLayer(QStringLiteral("B"), core::Color4d{0.2, 0.3, 0.4, 1.0});

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);

    auto* stack = editor->layerStackForTest();
    if (stack == nullptr || stack->rowCount() != 2) {
        expectations.expect(false, "both layers have rows");
        delete editor;
        finishFixture(fixture);
        return;
    }

    sendMouse(*stack, QEvent::MouseButtonPress, 200.0, 16.0);
    const auto* selectedLayer =
        std::get_if<document::LayerId>(&fixture.session.selection().primary);
    expectations.expect(stack->currentRow() == 0, "clicking row 0 makes it the current row");
    expectations.expect(selectedLayer != nullptr && *selectedLayer == stack->entries()[0].layerId,
                        "and selects that layer in the ONE shared session selection");

    sendMouse(*stack, QEvent::MouseButtonPress, 200.0, static_cast<qreal>(2 * 32 + 16));
    expectations.expect(stack->currentRow() == -1 &&
                            std::get_if<document::LayerId>(&fixture.session.selection().primary) ==
                                nullptr,
                        "clicking the empty area below the last row clears the selection, exactly "
                        "as clicking a QTreeWidget's blank area did");

    delete editor;
    finishFixture(fixture);
}

// Decision 5: the play/pause button's ICON swaps alongside its already-pinned text()/isChecked()
// contract (playback_controller_tests.cpp owns that contract byte-for-byte).
void testRangeRowsAndWorkAreaCommands(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Layer gestures"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{1, 0, 0, 1});
    (void)fixture.session.addSolidLayer(QStringLiteral("B"), core::Color4d{0, 0, 1, 1});
    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);
    auto* stack = editor->layerStackForTest();
    auto* lanes = editor->laneRegionForTest();
    const auto id = stack->entries().front().layerId;
    const auto second = stack->entries().back().layerId;
    const auto composition = fixture.session.compositionId();
    commands::Transaction trim("Trim", fixture.session.snapshot().revision());
    trim.emplace<commands::SetLayerRange>(composition, id, core::RationalTime::fromInteger(1),
                                          core::RationalTime::fromInteger(3));
    expectations.expect(fixture.session.executeTransaction(std::move(trim)).changed(),
                        "trim setup commits");
    const auto axis = ui::TimelineAxis::create(*fixture.session.composition(), lanes->width());
    if (!axis) {
        expectations.expect(false, "gesture axis");
        delete editor;
        finishFixture(fixture);
        return;
    }
    const auto x = [&](int secondValue) {
        return static_cast<int>(
            std::lround(axis->pixelForTime(core::RationalTime::fromInteger(secondValue))));
    };
    const auto before = fixture.commands.size();
    sendMouse(*lanes, QEvent::MouseButtonPress, x(2), 16);
    sendMouse(*lanes, QEvent::MouseMove, x(3), 16);
    expectations.expect(fixture.commands.size() == before,
                        "bar movement remains a session preview until release");
    sendMouse(*lanes, QEvent::MouseButtonRelease, x(3), 16);
    const auto* moved = fixture.session.composition()->graph().findLayer(id);
    expectations.expect(moved->inPoint == core::RationalTime::fromInteger(2) &&
                            moved->outPoint == core::RationalTime::fromInteger(4) &&
                            fixture.commands.size() == before + 1,
                        "body moves both frame-snapped endpoints in one transaction");
    (void)fixture.session.undo();
    expectations.expect(fixture.session.composition()->graph().findLayer(id)->inPoint ==
                            core::RationalTime::fromInteger(1),
                        "one undo restores both endpoints");
    sendMouse(*lanes, QEvent::MouseButtonPress, x(1), 16);
    sendMouse(*lanes, QEvent::MouseMove, x(0), 16);
    sendMouse(*lanes, QEvent::MouseButtonRelease, x(0), 16);
    expectations.expect(fixture.session.composition()->graph().findLayer(id)->inPoint ==
                                core::RationalTime{} &&
                            fixture.session.composition()->graph().findLayer(id)->outPoint ==
                                core::RationalTime::fromInteger(3),
                        "left trim handle changes only the in point");
    (void)fixture.session.undo();
    QTest::mouseClick(stack, Qt::LeftButton, Qt::NoModifier, QPoint(110, 16));
    QTest::mouseClick(stack, Qt::LeftButton, Qt::ControlModifier, QPoint(110, 48));
    expectations.expect(fixture.session.selectedNodes().size() == 2,
                        "Ctrl row selection shares the session selection set");
    QTest::mouseClick(stack, Qt::LeftButton, Qt::NoModifier, QPoint(110, 16));
    QTest::mouseDClick(stack, Qt::LeftButton, Qt::NoModifier, QPoint(110, 16));
    auto* rename = stack->findChild<QLineEdit*>("timelineLayerRenameEditor");
    expectations.expect(rename != nullptr, "double click opens inline rename");
    if (rename) {
        rename->setText("Renamed");
        QTest::keyClick(rename, Qt::Key_Return);
    }
    expectations.expect(fixture.session.composition()->graph().findLayer(id)->name == "Renamed",
                        "inline rename commits the layer name");
    (void)fixture.session.setCurrentTime(core::RationalTime::fromInteger(2));
    auto* split = editor->findChild<QAction*>("timelineSplitLayerAction");
    expectations.expect(split != nullptr, "split action exists");
    if (split)
        split->trigger();
    expectations.expect(stack->rowCount() == 3 &&
                            fixture.session.composition()->graph().findLayer(id)->outPoint ==
                                core::RationalTime::fromInteger(2),
                        "split creates an adjacent boundary and slot");
    (void)fixture.session.undo();
    (void)fixture.session.setCurrentTime(core::RationalTime::fromInteger(1));
    editor->findChild<QAction*>("timelineSetWorkAreaStartAction")->trigger();
    (void)fixture.session.setCurrentTime(core::RationalTime::fromInteger(4));
    editor->findChild<QAction*>("timelineSetWorkAreaEndAction")->trigger();
    expectations.expect(fixture.session.workArea() ==
                            document::WorkArea{core::RationalTime::fromInteger(1),
                                               core::RationalTime::fromInteger(4)},
                        "B/N actions author the shared work area");
    auto* strip = editor->findChild<ui::TimelineWorkAreaStrip*>("timelineWorkAreaStrip");
    if (strip) {
        const auto stripAxis =
            ui::TimelineAxis::create(*fixture.session.composition(), strip->width());
        if (stripAxis) {
            const auto left = static_cast<int>(
                std::lround(stripAxis->pixelForTime(core::RationalTime::fromInteger(1))));
            const auto next = static_cast<int>(
                std::lround(stripAxis->pixelForTime(core::RationalTime::fromInteger(2))));
            const auto history = fixture.commands.size();
            sendMouse(*strip, QEvent::MouseButtonPress, left, 1);
            sendMouse(*strip, QEvent::MouseMove, next, 1);
            expectations.expect(fixture.commands.size() == history,
                                "work-area handle previews until release");
            sendMouse(*strip, QEvent::MouseButtonRelease, next, 1);
            expectations.expect(
                fixture.session.workArea().start == core::RationalTime::fromInteger(2) &&
                    fixture.session.workArea().end == core::RationalTime::fromInteger(4),
                "work-area left handle preserves the exclusive end");
            (void)fixture.session.undo();
        }
        QTest::mouseDClick(strip, Qt::LeftButton, Qt::NoModifier, QPoint(strip->width() / 2, 1));
    }
    expectations.expect(!fixture.session.composition()->workArea(),
                        "double click clears the work area");
    sendMouse(*stack, QEvent::MouseButtonPress, 110, 16);
    sendMouse(*stack, QEvent::MouseMove, 110, 64);
    sendMouse(*stack, QEvent::MouseButtonRelease, 110, 64);
    expectations.expect(stack->entries().front().layerId == second,
                        "row drag commits stable-slot reorder");
    delete editor;
    finishFixture(fixture);
}

void testPlayPauseButtonIconSwapsWithState(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Play Pause Icon Test"));

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    auto* button = editor->findChild<QToolButton*>("playPauseButton");
    expectations.expect(button != nullptr, "the play/pause button is reachable by name");
    if (button == nullptr) {
        delete editor;
        finishFixture(fixture);
        return;
    }

    const auto playIcon = ui::kit::icon(ui::kit::IconId::Play, ui::kit::Size::IconMedium);
    const auto pauseIcon = ui::kit::icon(ui::kit::IconId::Pause, ui::kit::Size::IconMedium);
    const auto size =
        QSize(ui::kit::px(ui::kit::Size::IconMedium), ui::kit::px(ui::kit::Size::IconMedium));
    expectations.expect(button->icon().pixmap(size).toImage() == playIcon.pixmap(size).toImage(),
                        "the button starts showing the Play glyph");

    button->click();
    expectations.expect(button->isChecked() && button->text() == QStringLiteral("Pause"),
                        "the existing text()/isChecked() contract still flips on click");
    expectations.expect(button->icon().pixmap(size).toImage() == pauseIcon.pixmap(size).toImage(),
                        "clicking swaps the icon to Pause alongside the text");

    button->click();
    expectations.expect(button->icon().pixmap(size).toImage() == playIcon.pixmap(size).toImage(),
                        "clicking again swaps the icon back to Play");

    delete editor;
    finishFixture(fixture);
}

// Non-goal guard (decision 5): the loop indicator is a status glyph, never a clickable control.
void testLoopIndicatorIsNonInteractiveAndHonest(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Loop Indicator Test"));

    ui::TimelineEditor editor(fixture.session, fixture.controller);
    auto* indicator = editor.findChild<QLabel*>("timelineLoopIndicator");
    expectations.expect(indicator != nullptr, "the loop indicator exists");
    if (indicator == nullptr) {
        finishFixture(fixture);
        return;
    }
    expectations.expect(editor.findChild<QToolButton*>("timelineLoopIndicator") == nullptr,
                        "the loop indicator is a QLabel, never a clickable QToolButton");
    expectations.expect(!indicator->pixmap().isNull(), "the loop indicator carries a glyph");
    expectations.expect(indicator->toolTip().contains(QStringLiteral("loop"), Qt::CaseInsensitive),
                        "the tooltip honestly names the always-on looping behavior");

    finishFixture(fixture);
}

// task U8, issue #131, fix 7: the timeline's icon-only QToolButtons all size to controlExtent
// square, and (task T1) the whole transport cluster lives inside the LEFT column's own width.
void testTransportClusterIsSquareAndInsideTheLeftColumn(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Transport Cluster Test"));

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutEditor(host);

    for (const char* objectName :
         {"timelineStepBackButton", "playPauseButton", "timelineStepForwardButton"}) {
        auto* button = editor->findChild<QToolButton*>(QString::fromLatin1(objectName));
        expectations.expect(button != nullptr, std::string{objectName} + " is reachable by name");
        if (button == nullptr) {
            continue;
        }
        expectations.expect(button->width() == button->height(),
                            std::string{objectName} + " is square");
        expectations.expect(button->width() == ui::kit::px(ui::kit::Size::Control),
                            std::string{objectName} + " is exactly Size::Control square");
    }

    auto* controls = editor->findChild<QWidget*>("timelineControls");
    expectations.expect(
        controls != nullptr && controls->width() == ui::TimelineEditor::layerColumnWidth(),
        "the transport/readout cluster occupies exactly the LEFT column's width, so "
        "nothing in it overhangs the lane region");
    auto* readout = editor->findChild<QLabel*>("timelineTimeReadout");
    expectations.expect(readout != nullptr && controls != nullptr && readout->isVisible() &&
                            readout->mapTo(editor, QPoint(0, 0)).x() + readout->width() <=
                                ui::TimelineEditor::layerColumnWidth(),
                        "the frame/time readout fits inside that width too");

    delete editor;
    finishFixture(fixture);
}

void testPropertyRows(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Property rows"));
    (void)fixture.session.addSolidLayer("Solid", {0.2, 0.3, 0.4, 1});
    (void)fixture.session.addTextLayer("Text", "Bloom");
    ui::TimelineEditor editor(fixture.session, fixture.controller);
    editor.resize(1200, 700);
    editor.show();
    QCoreApplication::processEvents();
    auto* stack = editor.layerStackForTest();
    const auto layer = stack->entries().front().layerId;
    const auto revision = fixture.session.snapshot().revision();
    const int collapsedCount = stack->rowCount();
    stack->expansionRequested(layer);
    QCoreApplication::processEvents();
    expectations.expect(stack->rowCount() > collapsedCount, "chevron expands child rows");
    expectations.expect(fixture.session.snapshot().revision() == revision, "expansion is UI state");
    int positions = 0;
    for (std::size_t i = 0; i < stack->entries().size(); ++i) {
        const auto& entry = stack->entries()[i];
        if (entry.role == document::kPositionParameterRole)
            ++positions;
        expectations.expect(stack->rowTop(static_cast<int>(i)) ==
                                editor.laneRegionForTest()->rowTop(static_cast<int>(i)),
                            "every child shares its lane y");
    }
    expectations.expect(positions == 1, "Position is one parameter row");
    for (auto* row : editor.findChildren<QWidget*>("timelinePropertyRow")) {
        if (!row->isVisible() ||
            row->property("role").toString() !=
                QString::fromStdString(std::string(document::kPositionParameterRole)))
            continue;
        const auto id = document::ParameterId::fromRaw(row->property("parameterId").toULongLong());
        const auto before = fixture.session.effectiveVec2Value(id);
        const auto fields = row->findChildren<ui::kit::KValueField*>();
        expectations.expect(fields.size() == 2 && fields.front()->cellRect().width() >= 40,
                            "one vector row has two usable compact value cells");
        const auto history = fixture.commands.size();
        fields.front()->stepBy(1);
        const auto after = fixture.session.effectiveVec2Value(id);
        expectations.expect(before && after && after->x == before->x + 1 && after->y == before->y &&
                                fixture.commands.size() == history + 1,
                            "inline X commits the exact row parameter through its session setter");
    }
    for (auto* row : editor.findChildren<QWidget*>("timelinePropertyRow")) {
        if (!row->isVisible() || row->property("role").toString() != "color")
            continue;
        auto* chip = row->findChild<ui::kit::KColorChip*>("timelinePropertyColor");
        chip->colorChanged({0.25F, 0.5F, 0.75F, 1.0F});
        expectations.expect(fixture.commands.undoLabel() ==
                                std::optional<std::string_view>{"Set Text Color"},
                            "text color row uses the same setter and history label as Properties");
    }
    if (const auto path = qEnvironmentVariable("BLOOM_TIMELINE_TEST_IMAGE"); !path.isEmpty())
        expectations.expect(editor.grab().save(path), "save requested timeline inspection image");
    auto* panel = editor.findChild<QWidget*>("timelineKeyframePanel");
    auto* area = editor.findChild<QWidget*>("timelineKeyframeArea");
    expectations.expect(panel && area && editor.laneRegionForTest()->isAncestorOf(area),
                        "legacy names resolve inside the integrated lanes");
    stack->expansionRequested(layer);
    expectations.expect(stack->rowCount() == collapsedCount, "collapse restores layer rows");
    finishFixture(fixture);
}

void testIntegratedKeyGestures(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Lane gestures"));
    auto& session = fixture.session;
    (void)session.addSolidLayer("Keys", {0.2, 0.3, 0.4, 1});
    const auto opacity = session.parameterForSelection(document::kOpacityParameterRole)->id;
    const auto rotation = session.parameterForSelection(document::kRotationParameterRole)->id;
    expectations.expect(session.pasteKeyframes({{opacity, time(1), 0.2},
                                                {opacity, time(3), 0.5},
                                                {opacity, time(5), 0.8},
                                                {rotation, time(1), 10.0},
                                                {rotation, time(3), 30.0},
                                                {rotation, time(5), 50.0}},
                                               session.snapshot().revision()),
                        "seed keys on two parameters");
    ui::TimelineEditor editor(session, fixture.controller);
    editor.resize(1400, 700);
    editor.show();
    QCoreApplication::processEvents();
    const auto summary = editor.laneRegionForTest()->keySummaryTimes(0);
    expectations.expect(summary == std::vector<core::RationalTime>{time(1), time(3), time(5)},
                        "collapsed summary is the sorted union of all parameter keys");
    const auto summaryRevision = session.snapshot().revision();
    const auto summarySelection = session.selection();
    const auto summaryAxis =
        editor.rulerForTest()->axisForWidth(editor.laneRegionForTest()->width());
    if (!summaryAxis)
        throw std::runtime_error("Missing summary axis");
    sendMouse(*editor.laneRegionForTest(), QEvent::MouseButtonPress,
              summaryAxis->pixelForTime(time(3)), ui::kTimelineRowHeight / 2.0);
    sendMouse(*editor.laneRegionForTest(), QEvent::MouseButtonRelease,
              summaryAxis->pixelForTime(time(3)), ui::kTimelineRowHeight / 2.0);
    expectations.expect(editor.layerStackForTest()->rowCount() > 1 &&
                            editor.laneRegionForTest()->keySummaryTimes(0).empty(),
                        "clicking a summary expands its layer and exposes parameter lanes");
    expectations.expect(session.snapshot().revision() == summaryRevision &&
                            session.selection() == summarySelection,
                        "summary click changes only expansion");
    QCoreApplication::processEvents();
    auto* panel = editor.findChild<ui::TimelineKeyframePanel*>("timelineKeyframePanel");
    const auto axis = editor.rulerForTest()->axisForWidth(panel->width());
    if (!axis)
        throw std::runtime_error("Missing parameter lane axis");
    const auto yFor = [&](document::ParameterId parameter) {
        const auto& entries = editor.layerStackForTest()->entries();
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].parameterId == parameter)
                return editor.layerStackForTest()->rowTop(static_cast<int>(i)) +
                       ui::kTimelineRowHeight / 2;
        return -1;
    };
    const auto mouse = [&](QEvent::Type type, double seconds, int y,
                           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const QPointF point(axis->pixelForSeconds(seconds), y);
        QMouseEvent event(type, point, panel->mapToGlobal(point),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                          modifiers);
        QCoreApplication::sendEvent(panel, &event);
    };
    const auto click = [&](double seconds, int y,
                           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        mouse(QEvent::MouseButtonPress, seconds, y, modifiers);
        mouse(QEvent::MouseButtonRelease, seconds, y, modifiers);
    };
    const auto key = [&](int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent event(QEvent::KeyPress, code, modifiers);
        QCoreApplication::sendEvent(panel, &event);
    };
    click(1, yFor(opacity));
    click(3, yFor(rotation), Qt::ShiftModifier);
    expectations.expect(session.selection().keyframes.size() == 2,
                        "Shift-click extends session selection across parameter lanes");
    const auto selected = session.selection().keyframes;
    const auto history = fixture.commands.size();
    mouse(QEvent::MouseButtonPress, 1, yFor(opacity));
    mouse(QEvent::MouseMove, 2, yFor(opacity), Qt::ShiftModifier);
    expectations.expect(fixture.commands.size() == history, "drag previews without commands");
    mouse(QEvent::MouseButtonRelease, 2, yFor(opacity), Qt::ShiftModifier);
    expectations.expect(fixture.commands.size() == history + 1 &&
                            session.selection().keyframes == selected,
                        "multi-lane drag is one transaction and preserves selected IDs");
    const auto moved = session.selectedKeyframeData();
    expectations.expect(moved.size() == 2 && moved[0].time == time(2) && moved[1].time == time(4),
                        "drag moves all selected keys by one frame-grid delta");
    (void)session.undo();
    mouse(QEvent::MouseButtonPress, 0.5, yFor(rotation) - 10);
    mouse(QEvent::MouseMove, 5.5, yFor(opacity) + 10);
    mouse(QEvent::MouseButtonRelease, 5.5, yFor(opacity) + 10);
    expectations.expect(session.selection().keyframes.size() == 6,
                        "box-select spans parameter rows");
    std::vector<ui::KeyframeSelection> stretchKeys;
    for (const auto& selectedKey : session.selection().keyframes)
        if (selectedKey.curveId == selected.front().curveId)
            stretchKeys.push_back(selectedKey);
    session.selectKeyframes(stretchKeys);
    const auto stretchBefore = session.selectedKeyframeData();
    const auto stretchRevision = session.snapshot().revision();
    mouse(QEvent::MouseButtonPress, 5, yFor(opacity), Qt::AltModifier);
    mouse(QEvent::MouseMove, 9, yFor(opacity), Qt::AltModifier);
    mouse(QEvent::MouseButtonRelease, 9, yFor(opacity), Qt::AltModifier);
    const auto stretched = session.selectedKeyframeData();
    expectations.expect(stretched.size() == 3 && stretched[0].time == time(1) &&
                            stretched[1].time == time(5) && stretched[2].time == time(9),
                        "Alt-drag last key scales about the fixed first key");
    expectations.expect(session.snapshot().revision().value() == stretchRevision.value() + 1,
                        "stretch is one undo transaction");
    (void)session.undo();
    const auto restored = session.selectedKeyframeData();
    expectations.expect(restored.size() == 3 && restored[1].time == stretchBefore[1].time &&
                            session.selection().keyframes == stretchKeys,
                        "stretch undo restores exact times and IDs");
    mouse(QEvent::MouseButtonPress, 1, yFor(opacity), Qt::AltModifier);
    mouse(QEvent::MouseMove, 3, yFor(opacity), Qt::AltModifier);
    mouse(QEvent::MouseButtonRelease, 3, yFor(opacity), Qt::AltModifier);
    const auto compressed = session.selectedKeyframeData();
    expectations.expect(compressed.size() == 3 && compressed[0].time == time(3) &&
                            compressed[1].time == time(4) && compressed[2].time == time(5),
                        "Alt-drag first key scales about the fixed last key");
    (void)session.undo();
    session.selectKeyframe(selected[0].curveId, selected[0].keyframeId);
    mouse(QEvent::MouseButtonPress, 1, yFor(opacity), Qt::AltModifier);
    mouse(QEvent::MouseMove, 2, yFor(opacity), Qt::AltModifier);
    mouse(QEvent::MouseButtonRelease, 2, yFor(opacity), Qt::AltModifier);
    const auto duplicate = session.selectedKeyframeData();
    const auto duplicateKey = session.selection().keyframes.front();
    expectations.expect(duplicate.size() == 1 && duplicate[0].time == time(2) &&
                            session.selection().keyframes.front() != selected.front(),
                        "Alt-drag copies one key with a new identity");
    mouse(QEvent::MouseButtonDblClick, 2, yFor(opacity));
    expectations.expect(session.currentTime() == time(2), "double-click key moves playhead");
    key(Qt::Key_C, Qt::ControlModifier);
    (void)session.setCurrentTime(time(7));
    key(Qt::Key_V, Qt::ControlModifier);
    expectations.expect(session.selectedKeyframeData().front().time == time(7),
                        "Ctrl+C/Ctrl+V pastes on the same parameter at the playhead");
    const auto beforeDelete = fixture.commands.size();
    key(Qt::Key_Delete);
    expectations.expect(
        fixture.commands.size() == beforeDelete + 1 && session.selection().keyframes.empty(),
        "Delete removes selected keys in one transaction and prunes session selection");
    (void)session.undo();
    expectations.expect(session.composition()->animationCurves().find(selected.front().curveId) !=
                            nullptr,
                        "one undo restores deleted curve keys");
    session.selectKeyframes(selected);
    mouse(QEvent::MouseButtonPress, 1, yFor(opacity));
    mouse(QEvent::MouseMove, 1.123, yFor(opacity), Qt::ShiftModifier);
    mouse(QEvent::MouseButtonRelease, 1.123, yFor(opacity), Qt::ShiftModifier);
    const auto unsnapped = session.selectedKeyframeData();
    expectations.expect(unsnapped.size() == 2 && unsnapped[0].time == time(1123, 1000) &&
                            unsnapped[1].time == time(3123, 1000),
                        "Shift disables frame and magnetic snapping with one exact offset");
    key(Qt::Key_C, Qt::ControlModifier);
    (void)session.setCurrentTime(time(7011, 1000));
    key(Qt::Key_V, Qt::ControlModifier);
    const auto pastedSubframes = session.selectedKeyframeData();
    expectations.expect(pastedSubframes.size() == 2 &&
                            pastedSubframes[0].time == time(7011, 1000) &&
                            pastedSubframes[1].time == time(9011, 1000),
                        "clipboard preserves rational offsets relative to a subframe playhead");
    const auto cancelledRevision = session.snapshot().revision();
    mouse(QEvent::MouseButtonPress, 7.011, yFor(opacity));
    mouse(QEvent::MouseMove, 7.3, yFor(opacity));
    key(Qt::Key_Escape);
    mouse(QEvent::MouseButtonRelease, 7.3, yFor(opacity));
    expectations.expect(session.snapshot().revision() == cancelledRevision,
                        "Escape cancels a multi-key drag without a transaction");
    session.selectKeyframes(selected);
    bool pickedInterpolation = false;
    const auto menuRevision = session.snapshot().revision();
    QTimer::singleShot(0, panel, [&] {
        for (auto* menu : panel->findChildren<QMenu*>()) {
            for (auto* action : menu->actions())
                if (action->objectName() == "keyframeInterpolationAction" &&
                    action->text() == "Hold") {
                    pickedInterpolation = true;
                    action->trigger();
                    break;
                }
            menu->close();
        }
    });
    const QPoint contextPoint(static_cast<int>(std::lround(axis->pixelForSeconds(1.123))),
                              yFor(opacity));
    QContextMenuEvent context(QContextMenuEvent::Mouse, contextPoint,
                              panel->mapToGlobal(contextPoint));
    QCoreApplication::sendEvent(panel, &context);
    const auto interpolated = session.selectedKeyframeData();
    expectations.expect(pickedInterpolation &&
                            session.snapshot().revision().value() == menuRevision.value() + 1 &&
                            interpolated.size() == 2 &&
                            std::ranges::all_of(interpolated,
                                                [](const auto& item) {
                                                    return item.interpolation ==
                                                           document::KeyframeInterpolation::Hold;
                                                }),
                        "context menu edits every selected interpolation in one transaction");
    session.selectKeyframes({selected.front(), duplicateKey});
    mouse(QEvent::MouseButtonPress, 2, yFor(opacity), Qt::AltModifier);
    mouse(QEvent::MouseMove, 4, yFor(opacity), Qt::AltModifier);
    mouse(QEvent::MouseButtonRelease, 4, yFor(opacity), Qt::AltModifier);
    const auto fixedSubframe = session.selectedKeyframeData();
    expectations.expect(fixedSubframe.size() == 2 && fixedSubframe[0].time == time(1123, 1000) &&
                            fixedSubframe[1].time == time(4),
                        "stretch preserves the opposite endpoint's exact subframe time");
    (void)session.undo();
    mouse(QEvent::MouseButtonPress, 1.123, yFor(opacity));
    mouse(QEvent::MouseMove, 2.5, yFor(opacity));
    mouse(QEvent::MouseButtonRelease, 2.5, yFor(opacity));
    const auto spaced = session.selectedKeyframeData();
    expectations.expect(
        spaced.size() == 2 && spaced[0].time == time(5, 2) && spaced[1].time == time(3377, 1000),
        "frame snapping the dragged key preserves every selected key's exact relative offset");
    (void)session.undo();
    (void)session.setCurrentTime(time(4007, 1000));
    mouse(QEvent::MouseButtonPress, 1.123, yFor(opacity));
    mouse(QEvent::MouseMove, 4.01, yFor(opacity));
    mouse(QEvent::MouseButtonRelease, 4.01, yFor(opacity));
    const auto magnetic = session.selectedKeyframeData();
    expectations.expect(magnetic.size() == 2 && magnetic[0].time == time(4007, 1000) &&
                            magnetic[1].time == time(4884, 1000),
                        "magnetic snapping preserves an exact subframe playhead and group spacing");
    (void)session.undo();
    const auto lockedLayer = session.selection().contextualLayer;
    if (!lockedLayer)
        throw std::runtime_error("Missing keyframe contextual layer");
    commands::Transaction lock("Lock key target", session.snapshot().revision());
    lock.emplace<commands::SetLayerLocked>(session.compositionId(), *lockedLayer, true);
    (void)session.executeTransaction(std::move(lock));
    const auto lockedRevision = session.snapshot().revision();
    key(Qt::Key_Delete);
    expectations.expect(session.snapshot().revision() == lockedRevision &&
                            session.selection().keyframes.size() == 2,
                        "locked-layer keys reject a batch deletion without changing selection");
    finishFixture(fixture);
}

void testOutputMergeRows(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Merge Timeline"));
    (void)fixture.session.addSolidLayer(QStringLiteral("Nested layer"), core::Color4d{1, 0, 0, 1});
    const auto nested = fixture.session.composition()->graph().layerStack().nodeId();
    const auto endpoint = fixture.session.composition()->graph().compositionOutput();
    if (!endpoint) {
        finishFixture(fixture);
        return;
    }
    const auto output = endpoint->nodeId;
    commands::Transaction add("Add Merge", fixture.session.snapshot().revision());
    add.emplace<commands::AddNode>(fixture.session.compositionId(),
                                   std::string(document::kLayerStackNodeType), document::Vec2d{});
    const auto added = fixture.session.executeTransaction(std::move(add));
    const auto outer = added.outputId<document::NodeId>(commands::kAddNodeOutput);
    if (!outer) {
        expectations.expect(false, "new Merge authors");
        finishFixture(fixture);
        return;
    }
    commands::Transaction wire("Wire Merges", fixture.session.snapshot().revision());
    wire.emplace<commands::ConnectPorts>(fixture.session.compositionId(),
                                         document::OutputPortRef{nested, "image"},
                                         document::LayerStackInputRef{*outer, {}, "content"});
    wire.emplace<commands::ConnectPorts>(fixture.session.compositionId(),
                                         document::OutputPortRef{*outer, "image"},
                                         document::NodeInputRef{output, "image"});
    (void)fixture.session.executeTransaction(std::move(wire));
    ui::TimelineEditor editor(fixture.session, fixture.controller);
    const auto& rows = editor.layerStackForTest()->entries();
    expectations.expect(rows.size() == 1 && rows.front().imageNodeId == nested &&
                            rows.front().clipColor == ui::kit::Color::DataComposition &&
                            !rows.front().expanded,
                        "nested Merge appears as one collapsed composition-colored row");
    fixture.session.selectNode(*outer);
    ui::PropertiesEditor properties(fixture.session);
    const auto names = properties.findChildren<QLabel*>("mergeInputName");
    const auto modes = properties.findChildren<ui::kit::KDropdown*>("mergeInputBlendMode");
    expectations.expect(names.size() == 1 && names.front()->text() == rows.front().name &&
                            modes.size() == 1 && !modes.front()->isEnabled(),
                        "Merge Properties list ordered plain-image inputs read-only");
    commands::Transaction disable("Disable nested Merge", fixture.session.snapshot().revision());
    disable.emplace<commands::SetMergeEnabled>(fixture.session.compositionId(), nested, false);
    (void)fixture.session.executeTransaction(std::move(disable));
    expectations.expect(!fixture.session.composition()->graph().merge(nested)->enabled(),
                        "collapsed Merge has an authored enabled toggle");
    finishFixture(fixture);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("BloomTests");
    QCoreApplication::setApplicationName("TimelineEditorStyle");
    QTemporaryDir settingsDirectory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    Expectations expectations;
    try {
        testOutputMergeRows(expectations);
        testRulerAndLanesShareTheLaneRegionOrigin(expectations);
        testHeaderSplitInEditorArea(expectations);
        testTimeViewportGestures(expectations);
        testPropertyRows(expectations);
        testIntegratedKeyGestures(expectations);
        testTimelineHeaderMenus(expectations);
        testPlayheadSpansRulerAndEveryLane(expectations);
        testRowsAreFlatThirtyTwoPixelRows(expectations);
        testOneScrollbarMovesBothHalvesTogether(expectations);
        testClipBarSpansTheCompositionRangeInItsDataTypeColor(expectations);
        testSelectedRowIsASurfaceRaisedFillNotAnAccentOutline(expectations);
        testToggleColumnsCommitLayerFlags(expectations);
        testRangeRowsAndWorkAreaCommands(expectations);
        testBlendingAndParentAreDisabledKDropdowns(expectations);
        testBlendingRowAuthorsTheLayerItDraws(expectations);
        testKindHasNoColumnButStaysReadable(expectations);
        testLayerStackIsNoLongerAnItemView(expectations);
        testManyRowsStayBoundedAndThePlayheadNeverRelayoutsThem(expectations);
        testDraggingALaneScrubsThroughTheRulerScrubPath(expectations);
        testClickingTheLeftColumnSelectsAndClears(expectations);
        testPlayPauseButtonIconSwapsWithState(expectations);
        testLoopIndicatorIsNonInteractiveAndHonest(expectations);
        testTransportClusterIsSquareAndInsideTheLeftColumn(expectations);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: legacy text fixture: " << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
