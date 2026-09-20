#ifndef BLOOM_GRAMMAR_ARTIFACT_DIR
#define BLOOM_GRAMMAR_ARTIFACT_DIR "."
#endif
#include "window_fixture.hpp"
#undef BLOOM_GRAMMAR_ARTIFACT_DIR

#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/kit/split_handle.hpp>
#include <bloom/ui/licenses_window.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/window_status_bar.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QObject>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <array>
#include <cmath>
#include <exception>

#include <cstdlib>
#include <iostream>
#include <numeric>
#include <source_location>
#include <string>
#include <string_view>

// QDesktopServices::setUrlHandler() looks its receiver's slot up by name through the meta-object
// system, which requires a moc-processed QObject. Kept at file scope, outside the anonymous
// namespace below, because moc cannot reliably generate metadata for a Q_OBJECT class declared
// inside one.
class UrlCapture final : public QObject {
    Q_OBJECT

  public:
    int callCount = 0;
    QUrl lastUrl;

  public Q_SLOTS:
    void capture(const QUrl& url) {
        ++callCount;
        lastUrl = url;
    }
};

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

[[nodiscard]] bool registerStandInEditors(EditorRegistry& registry) {
    const auto addTestEditor = [&registry](std::string id, QString name) {
        return registry.registerEditor(
            {.id = std::move(id), .displayName = std::move(name), .create = [](QWidget* parent) {
                 return new QLabel("chrome test editor", parent);
             }});
    };
    return addTestEditor("bloom.viewer", "Compositor") && addTestEditor("bloom.nodes", "Nodes") &&
           addTestEditor("bloom.timeline", "Timeline") && addTestEditor("bloom.assets", "Assets") &&
           addTestEditor("bloom.properties", "Properties");
}

// A small fixture bundle so each test can build a fresh MainWindow without repeating the whole
// ProjectHost/CompositionSession/FrameExportController wiring inline -- mirrors tests/ui_smoke.cpp
// and main_window_readonly_placeholder_tests.cpp's own precedent for this construction.
struct Fixture final {
    EditorRegistry registry;
    bloom::runtime::TaskScheduler scheduler;
    ProjectHost projectHost{scheduler};
    CompositionSession compositionSession;
    bloom::runtime::NodeDefinitionRegistry nodeDefinitions;
    bloom::runtime::SnapshotCompiler snapshotCompiler{nodeDefinitions};
    TaskUiBridge taskUiBridge{scheduler};
    FrameExportController frameExportController;

    explicit Fixture(bool* ok)
        : compositionSession(*projectHost.liveDocumentAndStack().first,
                             *projectHost.liveDocumentAndStack().second,
                             projectHost.lowestCompositionId()),
          frameExportController(compositionSession, scheduler, taskUiBridge, snapshotCompiler,
                                projectHost.publicationCoordinator(),
                                projectHost.artifactCoordinator()) {
        nodeDefinitions.freeze();
        *ok = registerStandInEditors(registry);
    }
};

void testChromeModeFromSettingsReadsTheInjectedFile(Expectations& expectations) {
    QTemporaryDir directory;
    expectations.expect(directory.isValid(), "chrome mode: temp directory is available");
    if (!directory.isValid()) {
        return;
    }
    const auto path = QDir(directory.path()).filePath("settings.ini");

    {
        QSettings settings(path, QSettings::IniFormat);
        expectations.expect(chromeModeFromSettings(settings) == ChromeMode::Custom,
                            "chrome mode: an absent key defaults to Custom");
    }
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue("appearance/chrome", "native");
        settings.sync();
    }
    {
        const QSettings settings(path, QSettings::IniFormat);
        expectations.expect(chromeModeFromSettings(settings) == ChromeMode::Native,
                            "chrome mode: an injected \"native\" value reads as Native");
    }
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue("appearance/chrome", "custom");
        settings.sync();
    }
    {
        const QSettings settings(path, QSettings::IniFormat);
        expectations.expect(chromeModeFromSettings(settings) == ChromeMode::Custom,
                            "chrome mode: an injected \"custom\" value reads as Custom");
    }
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue("appearance/chrome", "nonsense");
        settings.sync();
    }
    {
        const QSettings settings(path, QSettings::IniFormat);
        expectations.expect(chromeModeFromSettings(settings) == ChromeMode::Custom,
                            "chrome mode: any other value defaults to Custom, not a crash");
    }
}

// task C1, item C1 (owner: "let OS handle the native window chrome for now" / "no need for
// custom minimize/maximize/close" / "remove the Bloom app title"): native (server-side) decoration
// is now the default AND ONLY mode MainWindow ever builds. There is no more ChromeMode-dependent
// branch inside MainWindow to exercise here -- the CSD kit::TitleBar and FramelessEdgeResizer stay
// compiled and get their OWN isolated coverage instead (kit_title_bar_tests.cpp,
// frameless_window_support_tests.cpp), mirroring kit_title_bar_tests.cpp's existing precedent of
// testing the retained-but-unused widget standalone rather than through MainWindow. The old
// "custom chrome" test that exercised a frameless MainWindow with an embedded TitleBar is gone: a
// frameless MainWindow with a TitleBar can no longer be built at all.
void testLegacyLayoutMigration(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "migration editors registered");
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost,
                      fixture.frameExportController);
    window.workspaceHost()->resetToSingleArea("bloom.viewer");
    auto legacy = QJsonDocument::fromJson(window.workspaceHost()->saveLayoutState()).object();
    legacy["schema"] = 1;
    QTemporaryDir directory;
    QSettings settings(directory.filePath("migration.ini"), QSettings::IniFormat);
    settings.setValue("workspace/compositing/layout",
                      QJsonDocument(legacy).toJson(QJsonDocument::Compact));
    expectations.expect(window.restoreApplicationState(settings) ==
                            WorkspaceLayoutRestoreResult::Restored,
                        "valid legacy layout migrates");
    expectations.expect(window.workspaceHost()->areaCount() == 5,
                        "legacy layout gains all default panels");
    const auto migrated = window.workspaceHost()->saveLayoutState();
    expectations.expect(QJsonDocument::fromJson(migrated).object().value("schema").toInt() == 2 &&
                            migrated.contains("bloom.assets"),
                        "version 2 pins Assets into the migrated default");
}

void testResetWorkspaceRestoresAndPersistsTheOwnerArrangement(Expectations& expectations) {
    try {
        bloom::ui::test::WindowFixture fixture;
        auto* resetAction =
            fixture.window->findChild<QAction*>(QStringLiteral("resetWorkspaceAction"));
        expectations.expect(resetAction != nullptr &&
                                resetAction->text() == QStringLiteral("Reset Workspace"),
                            "reset workspace: Window menu carries the Reset Workspace action");

        const auto visibleArea = [&fixture](const std::string_view editorId) {
            for (auto* area : fixture.window->workspaceHost()->findChildren<EditorArea*>()) {
                if (area->isVisible() && area->editorId() == editorId) {
                    return area;
                }
            }
            return static_cast<EditorArea*>(nullptr);
        };
        const auto visibleTimeline = [&fixture] {
            for (auto* timeline : fixture.window->findChildren<TimelineEditor*>()) {
                if (timeline->isVisible()) {
                    return timeline;
                }
            }
            return static_cast<TimelineEditor*>(nullptr);
        };
        // The Viewer/Nodes row is the top child of the Timeline area's own (vertical) splitter.
        const auto viewerNodesRow = [&]() -> QSplitter* {
            auto* timelineArea = visibleArea("bloom.timeline");
            auto* leftColumn = timelineArea != nullptr
                                   ? qobject_cast<QSplitter*>(timelineArea->parentWidget())
                                   : nullptr;
            return leftColumn != nullptr ? qobject_cast<QSplitter*>(leftColumn->widget(0))
                                         : nullptr;
        };

        auto* topRow = viewerNodesRow();
        auto* timeline = visibleTimeline();
        expectations.expect(
            topRow != nullptr && timeline != nullptr,
            "reset workspace: the default Viewer/Nodes row and timeline are visible");
        if (topRow == nullptr || timeline == nullptr || resetAction == nullptr) {
            return;
        }

        topRow->setSizes({500, 250});
        auto* handle = timeline->splitHandleForTest();
        expectations.expect(handle != nullptr, "reset workspace: the timeline divider exists");
        if (handle == nullptr) {
            return;
        }
        const int originalTimelineWidth = timeline->layerColumnWidthForTest();
        const QPointF press(handle->width() / 2.0, handle->height() / 2.0);
        QMouseEvent pressEvent(QEvent::MouseButtonPress, press,
                               handle->mapToGlobal(press.toPoint()), Qt::LeftButton, Qt::LeftButton,
                               Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &pressEvent);
        const QPointF moved(press.x() + 80.0, press.y());
        QMouseEvent moveEvent(QEvent::MouseMove, moved, handle->mapToGlobal(moved.toPoint()),
                              Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &moveEvent);
        QMouseEvent releaseEvent(QEvent::MouseButtonRelease, moved,
                                 handle->mapToGlobal(moved.toPoint()), Qt::LeftButton, Qt::NoButton,
                                 Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &releaseEvent);
        QCoreApplication::processEvents();
        expectations.expect(timeline->layerColumnWidthForTest() != originalTimelineWidth,
                            "reset workspace: dragging changes the timeline divider");

        resetAction->trigger();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        topRow = viewerNodesRow();
        timeline = visibleTimeline();
        expectations.expect(topRow != nullptr && timeline != nullptr,
                            "reset workspace: reset rebuilds the visible default panels");
        if (topRow == nullptr || timeline == nullptr) {
            return;
        }

        const auto share = [](const QList<int>& sizes, const int index) {
            const int total = std::accumulate(sizes.cbegin(), sizes.cend(), 0);
            return total > 0 ? static_cast<double>(sizes[index]) / total : 0.0;
        };
        const auto sizes = topRow->sizes();
        expectations.expect(std::abs(share(sizes, 0) - 0.505) <= 0.01 &&
                                std::abs(share(sizes, 1) - 0.495) <= 0.01,
                            "reset workspace: the Viewer/Nodes proportions are restored");
        const double timelineShare =
            timeline->width() > 0
                ? static_cast<double>(timeline->layerColumnWidthForTest()) / timeline->width()
                : 0.0;
        expectations.expect(std::abs(timelineShare - 0.37) <= 0.01,
                            "reset workspace: the timeline divider returns to 37%");

        auto* viewer = fixture.window->findChild<ViewerEditor*>();
        expectations.expect(viewer != nullptr && viewer->isVisible() &&
                                viewer->channelForTest() == ViewerChannel::Rgba &&
                                viewer->viewTransformForTest().fitToWindow,
                            "reset workspace: Viewer starts in RGBA and Fit");
        auto* nodes = fixture.window->findChild<NodeGraphEditor*>();
        expectations.expect(nodes != nullptr && nodes->isVisible() &&
                                nodes->graphView() != nullptr &&
                                !nodes->graphView()->viewAdjusted(),
                            "reset workspace: Nodes starts fitted");
        fixture.session.clearSelection();
        QCoreApplication::processEvents();
        auto* properties = fixture.window->findChild<PropertiesEditor*>();
        auto* documentSection =
            properties != nullptr
                ? properties->findChild<QWidget*>(QStringLiteral("propertiesDocumentSection"))
                : nullptr;
        expectations.expect(properties != nullptr && properties->isVisible() &&
                                documentSection != nullptr && documentSection->isVisible(),
                            "reset workspace: Properties shows Composition with no selection");

        QSettings settings;
        settings.sync();
        const auto saved = QJsonDocument::fromJson(
            settings.value(QStringLiteral("workspace/compositing/layout")).toByteArray());
        const auto root = saved.object().value(QStringLiteral("root")).toObject();
        const auto savedRootWeights = root.value(QStringLiteral("weights")).toArray();
        const auto rootChildren = root.value(QStringLiteral("children")).toArray();
        const auto leftColumnNode = rootChildren.at(0).toObject();
        const auto rightColumnNode = rootChildren.at(1).toObject();
        const auto savedLeftWeights = leftColumnNode.value(QStringLiteral("weights")).toArray();
        const auto savedRightWeights = rightColumnNode.value(QStringLiteral("weights")).toArray();
        const auto savedTopLeftWeights = leftColumnNode.value(QStringLiteral("children"))
                                             .toArray()
                                             .at(0)
                                             .toObject()
                                             .value(QStringLiteral("weights"))
                                             .toArray();
        expectations.expect(savedRootWeights.size() == 2 &&
                                std::abs(savedRootWeights.at(0).toDouble() - 0.80) <= 0.01 &&
                                std::abs(savedRootWeights.at(1).toDouble() - 0.20) <= 0.01 &&
                                savedLeftWeights.size() == 2 &&
                                std::abs(savedLeftWeights.at(0).toDouble() - 0.56) <= 0.01 &&
                                std::abs(savedLeftWeights.at(1).toDouble() - 0.44) <= 0.01 &&
                                savedRightWeights.size() == 2 &&
                                std::abs(savedRightWeights.at(0).toDouble() - 0.395) <= 0.01 &&
                                std::abs(savedRightWeights.at(1).toDouble() - 0.605) <= 0.01 &&
                                savedTopLeftWeights.size() == 2 &&
                                std::abs(savedTopLeftWeights.at(0).toDouble() - 0.505) <= 0.01 &&
                                std::abs(savedTopLeftWeights.at(1).toDouble() - 0.495) <= 0.01,
                            "reset workspace: persisted layout keys contain the restored weights");
        const int persistedTimelineWidth =
            settings.value(QStringLiteral("timeline/layer-column-width")).toInt();
        expectations.expect(
            settings.contains(QStringLiteral("timeline/layer-column-width")) &&
                std::abs(persistedTimelineWidth - timeline->layerColumnWidthForTest()) <= 1,
            "reset workspace: the timeline divider's persisted key matches its live value");
    } catch (const std::exception& error) {
        expectations.expect(false, std::string("reset workspace fixture: ") + error.what());
    }
}

void testMainWindowAlwaysBuildsNativeChrome(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "native chrome: fixture editors registered");
    if (!ok) {
        return;
    }

    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost,
                      fixture.frameExportController);
    expectations.expect(!window.windowFlags().testFlag(Qt::FramelessWindowHint),
                        "native chrome: stock OS decorations, never frameless");
    expectations.expect(window.findChild<QWidget*>(QStringLiteral("kinetikTitleBar")) == nullptr,
                        "native chrome: no Kinetik TitleBar is constructed at all");
    expectations.expect(window.menuBar() != nullptr && window.menuWidget() == window.menuBar(),
                        "native chrome: QMainWindow's own classic menu bar is in charge");

    // No "Bloom" app-title label anywhere in the client area (task C1, item C1): the only window
    // title left is the OS one, and it carries the document title alone.
    expectations.expect(window.findChild<QLabel*>(QStringLiteral("titleBarTitleLabel")) == nullptr,
                        "native chrome: the old title-bar label widget does not exist in this "
                        "window at all");
}

// task C1, item C1: the OS window title is exactly the document title -- "Untitled" or the file
// name -- with Qt's own "[*]" modified marker, and never a "Bloom — " prefix.
void testWindowTitleIsJustTheDocumentTitle(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "window title: fixture editors registered");
    if (!ok) {
        return;
    }

    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost,
                      fixture.frameExportController);
    expectations.expect(window.windowTitle() == QStringLiteral("Untitled[*]"),
                        "window title: a fresh project shows the bare document title, no Bloom "
                        "prefix (got \"" +
                            window.windowTitle().toStdString() + "\")");
    expectations.expect(!window.isWindowModified(), "window title: starts unmodified");
}

void testViewMenuItemsExistAndFire(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    if (!ok) {
        expectations.expect(false, "view menu: fixture editors registered");
        return;
    }
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost,
                      fixture.frameExportController);

    auto* fullScreenAction = window.findChild<QAction*>(QStringLiteral("viewFullScreenAction"));
    auto* maximizePanelAction =
        window.findChild<QAction*>(QStringLiteral("viewMaximizePanelAction"));
    expectations.expect(fullScreenAction != nullptr && maximizePanelAction != nullptr,
                        "view menu: Full Screen and Maximize Panel both exist");
    if (fullScreenAction == nullptr || maximizePanelAction == nullptr) {
        return;
    }

    // task C1, item C1: "Use Native Window Frame" is gone -- native chrome is the default and
    // only mode now, so the View menu no longer offers a chrome setting at all.
    expectations.expect(
        window.findChild<QAction*>(QStringLiteral("useNativeFrameAction")) == nullptr,
        "view menu: Use Native Window Frame has really been removed, not merely disabled");

    expectations.expect(!fullScreenAction->shortcut().isEmpty(),
                        "view menu: Full Screen carries the F11 shortcut");
    expectations.expect(!window.isFullScreen(), "view menu: starts out of full screen");
    fullScreenAction->trigger();
    expectations.expect(window.isFullScreen(), "view menu: Full Screen really toggles it");
    fullScreenAction->trigger();
    expectations.expect(!window.isFullScreen(), "view menu: and toggles back");

    // Maximize Panel routes to the exact same underlying signal the Window menu's own action
    // does (decision 2). The default compositing layout starts with five areas, so both actions
    // are enabled from construction; triggering it flips the SAME workspace maximize state the
    // Window menu's own action would.
    auto* windowMaximizeAction = window.findChild<QAction*>(QStringLiteral("maximizeAreaAction"));
    expectations.expect(windowMaximizeAction != nullptr, "view menu: the Window menu's own "
                                                         "Maximize Active Area action exists too");
    expectations.expect(maximizePanelAction->isEnabled(),
                        "view menu: Maximize Panel is enabled with the default multi-area layout");
    maximizePanelAction->trigger();
    expectations.expect(window.workspaceHost()->isAreaMaximized(),
                        "view menu: Maximize Panel really maximizes the active area");
    if (windowMaximizeAction != nullptr) {
        expectations.expect(windowMaximizeAction->isChecked(),
                            "view menu: the Window menu's action reflects the same state Maximize "
                            "Panel just set");
    }
    maximizePanelAction->trigger();
    expectations.expect(!window.workspaceHost()->isAreaMaximized(),
                        "view menu: triggering it again restores");
}

void testHelpMenuItemsExistAndFire(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    if (!ok) {
        expectations.expect(false, "help menu: fixture editors registered");
        return;
    }
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost,
                      fixture.frameExportController);

    auto* reportIssueAction = window.findChild<QAction*>(QStringLiteral("reportIssueAction"));
    auto* licensesAction = window.findChild<QAction*>(QStringLiteral("openSourceLicensesAction"));
    expectations.expect(reportIssueAction != nullptr && licensesAction != nullptr,
                        "help menu: Report an Issue and Open Source Licenses both exist");
    if (reportIssueAction == nullptr || licensesAction == nullptr) {
        return;
    }

    UrlCapture capture;
    QDesktopServices::setUrlHandler(QStringLiteral("https"), &capture, "capture");
    reportIssueAction->trigger();
    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    expectations.expect(capture.callCount == 1,
                        "help menu: Report an Issue really calls QDesktopServices::openUrl()");
    expectations.expect(capture.lastUrl.scheme() == QStringLiteral("https") &&
                            capture.lastUrl.host() == QStringLiteral("github.com"),
                        "help menu: it opens the repository's issue tracker");

    // Non-modal (LicensesWindow::show(), never exec()): triggering it synchronously must not hang
    // this test.
    licensesAction->trigger();
    auto* licensesWindow = window.findChild<LicensesWindow*>();
    expectations.expect(licensesWindow != nullptr,
                        "help menu: Open Source Licenses opens a real LicensesWindow");
}

// The Preferences window is opened from Edit | Preferences... and carries PreferencesRole, which is
// how Qt routes it into the macOS application menu with the standard shortcut. Triggering it is not
// exercised here: it calls QDialog::exec() and would block this test.
void testPreferencesActionIsInTheEditMenuWithPreferencesRole(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    if (!ok) {
        expectations.expect(false, "preferences action: fixture editors registered");
        return;
    }
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost,
                      fixture.frameExportController);

    auto* settings = window.findChild<QAction*>(QStringLiteral("preferencesAction"));
    expectations.expect(settings != nullptr, "edit menu: a Settings action exists");
    if (settings == nullptr) {
        return;
    }
    expectations.expect(settings->menuRole() == QAction::PreferencesRole,
                        "edit menu: Settings carries PreferencesRole so macOS routes it");
    expectations.expect(settings->shortcut() == QKeySequence(QKeySequence::Preferences),
                        "edit menu: Settings uses the standard Preferences shortcut");

    QMenu* editMenu = nullptr;
    for (QAction* action : window.menuBar()->actions()) {
        if (action->text() == QStringLiteral("&Edit") && action->menu() != nullptr) {
            editMenu = action->menu();
        }
    }
    expectations.expect(editMenu != nullptr && editMenu->actions().contains(settings),
                        "edit menu: Settings lives in Edit, not File");
}

// --- Task VIEW-1: the window status bar --------------------------------------------------------

// A kit strip under the workspace, never QMainWindow's own QStatusBar, and it carries every cell
// the task names. The preview cells are exercised separately (viewer_editor_tests/ram_preview_tests
// read the same free functions the strip's cells call); what is proved here is that the strip
// exists, is the right KIND of thing, reports the version, and clears a notice after five seconds.
void testWindowStatusBarIsAKitStripWithEveryCell(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "status bar: stand-in editors register");
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost,
                      fixture.frameExportController);

    auto* strip = window.findChild<WindowStatusBar*>(QStringLiteral("windowStatusBar"));
    expectations.expect(strip != nullptr && strip == window.statusStrip(),
                        "status bar: the window carries one, under the objectName windowStatusBar");
    if (strip == nullptr) {
        return;
    }
    expectations.expect(window.findChild<QStatusBar*>() == nullptr,
                        "status bar: and it is NOT QMainWindow's own QStatusBar chrome");
    expectations.expect(window.centralWidget() != nullptr &&
                            window.centralWidget()->isAncestorOf(strip),
                        "status bar: it is a row of the central column, so it stays visible "
                        "whichever central page is authoritative");
    for (const char* cell :
         {"windowStatusBarColorChip", "windowStatusBarPreviewState", "windowStatusBarDroppedFrames",
          "windowStatusBarCache", "windowStatusBarMediaDiskCache", "windowStatusBarMessage",
          "windowStatusBarVersion"}) {
        expectations.expect(strip->findChild<QWidget*>(QString::fromLatin1(cell)) != nullptr,
                            std::string{"status bar: it carries the "} + cell + " cell");
    }
    expectations.expect(strip->versionTextForTest() == QStringLiteral("0.1.0"),
                        "status bar: the application version is what the version cell shows");

    bloom::runtime::OperationCache operationCache(1U << 20U);
    operationCache.store(
        "status-entry", bloom::document::Revision::fromRaw(1),
        {.image = {}, .values = {bloom::runtime::CompiledValue{0.5}}, .bounds = {}});
    (void)operationCache.find("status-entry", bloom::document::Revision::fromRaw(1));
    (void)operationCache.find("missing-entry", bloom::document::Revision::fromRaw(1));
    WindowStatusBar cacheStrip(fixture.compositionSession, nullptr, nullptr, &operationCache);
    expectations.expect(
        cacheStrip.cacheTextForTest().contains(QStringLiteral("Ops 1 hits")) &&
            cacheStrip.cacheTextForTest().contains(QStringLiteral("1 misses")) &&
            cacheStrip.cacheTextForTest().contains(QStringLiteral("/ 1.0 MB")),
        "status bar: the cache cell reports operation hits, misses, retained bytes, "
        "and budget");
    auto* cacheLabel = cacheStrip.findChild<QLabel*>(QStringLiteral("windowStatusBarCache"));
    expectations.expect(cacheLabel != nullptr &&
                            cacheLabel->toolTip().contains(QStringLiteral("operation-cache")),
                        "status bar: the dense cache cell explains its two cache accounts in a "
                        "tooltip");
    // CACHE-2: the window is built with no disk cache (this fixture matches Timeline/window
    // fixtures elsewhere that pass no ninth argument), so the cell reports "off" rather than an
    // invented hit rate.
    expectations.expect(strip->mediaDiskCacheTextForTest() == QStringLiteral("Disk cache off"),
                        "status bar: a window built without a media disk cache reports it as off");
}

// CACHEFIX-2: deterministic machine samples and clock exercise admission, recovery and notices.
void testWindowStatusBarTrimsCachesUnderMemoryPressure(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "memory pressure: stand-in editors register");
    constexpr std::size_t kBudget = std::size_t{1} << 20U;
    constexpr auto gib = std::size_t{1024} * 1024 * 1024;
    bloom::runtime::MemoryBudgetLedger ledger(16 * gib, 16 * gib);
    bloom::runtime::OperationCache operationCache(kBudget, ledger);
    const auto fill = [&operationCache](const char* prefix) {
        for (int index = 0; index < 16; ++index)
            operationCache.store(
                prefix + std::to_string(index) + std::string(std::size_t{64} * 1024U, 'x'),
                bloom::document::Revision::fromRaw(1), {.image = {}, .values = {}, .bounds = {}});
    };
    fill("pressure-");
    WindowStatusBar strip(fixture.compositionSession, nullptr, nullptr, &operationCache, nullptr,
                          ledger);
    const auto poll = [&strip](std::size_t available, int seconds) {
        strip.pollMemoryPressureForTest({.availableBytes = available},
                                        bloom::runtime::MemoryBudgetLedger::Clock::time_point{} +
                                            std::chrono::seconds(seconds));
    };

    const auto reserve = strip.memoryReserveBytesForTest();
    expectations.expect(reserve >= bloom::runtime::kMinimumHostMemoryReserve,
                        "memory pressure: the reserve is never below the 8 GiB minimum");
    const auto filled = operationCache.retainedBytes();
    expectations.expect(filled > kBudget / 2,
                        "memory pressure: the fixture cache holds more than half its budget");

    poll(reserve * 2, 0);
    expectations.expect(strip.messageTextForTest().isEmpty() &&
                            operationCache.retainedBytes() == filled,
                        "memory pressure: a machine with memory to spare is left alone");
    strip.pollMemoryPressureForTest({}, bloom::runtime::MemoryBudgetLedger::Clock::time_point{});
    expectations.expect(strip.messageTextForTest().isEmpty() &&
                            operationCache.retainedBytes() == filled,
                        "memory pressure: a platform that reports nothing is not read as empty");

    poll(reserve / 2, 5);
    expectations.expect(strip.messageTextForTest() ==
                            QStringLiteral("Memory pressure: caches trimmed"),
                        "memory pressure: the bar reports the trim in the artist's terms");
    expectations.expect(operationCache.retainedBytes() <= kBudget / 4,
                        "memory pressure: the operation cache is trimmed below 25 percent");
    expectations.expect(operationCache.byteBudget() <= kBudget / 4 &&
                            ledger.state().configuredBytes == 8 * gib,
                        "memory pressure: admission shrinks while the configured total survives");
    expectations.expect(operationCache.statistics().pressureDrops > 0,
                        "memory pressure: trimmed entries are counted apart from budget eviction");

    strip.clearTransientMessage();
    poll(reserve / 2, 10);
    expectations.expect(strip.messageTextForTest().isEmpty(),
                        "memory pressure: a machine that stays busy is told once, not every poll");

    expectations.expect(operationCache.byteBudget() <= kBudget / 10,
                        "memory pressure: second poll limits further admission to 10 percent");
    poll(reserve * 2, 15);
    poll(reserve * 2, 25);
    poll(reserve * 2, 35);
    poll(reserve * 2, 45);
    fill("recovered-");
    expectations.expect(operationCache.retainedBytes() > kBudget / 2,
                        "memory pressure: the cache refills after the timed recovery ladder");
    poll(reserve / 2, 50);
    expectations.expect(strip.messageTextForTest() ==
                            QStringLiteral("Memory pressure: caches trimmed"),
                        "memory pressure: a second episode is reported again");
    expectations.expect(
        strip.cacheToolTipForTest().contains(QStringLiteral("Effective cap:")) &&
            strip.cacheToolTipForTest().contains(QStringLiteral("Configured total:")) &&
            strip.cacheToolTipForTest().contains(QStringLiteral("MemAvailable:")) &&
            strip.cacheToolTipForTest().contains(QStringLiteral("Memory pressure")),
        "the tooltip exposes the effective cap, configured total, sample and state");
    poll(reserve * 2, 55);
    strip.pollMemoryPressureForTest(
        {.availableBytes = reserve * 2, .swapTotalBytes = 4 * gib, .swapUsedBytes = 2 * gib},
        bloom::runtime::MemoryBudgetLedger::Clock::time_point{} + std::chrono::seconds(60));
    expectations.expect(strip.messageTextForTest() ==
                            QStringLiteral("Memory pressure: caches trimmed"),
                        "swap pressure preserves the existing first notice");
    strip.clearTransientMessage();
    expectations.expect(strip.messageTextForTest() ==
                            QStringLiteral("Swap pressure: caches trimmed"),
                        "swap pressure also produces its own distinct second notice");
}

// CACHE-2: "Clear Media Cache…" lives in the Composition menu beside RAM Preview, present (though
// reporting "not enabled" if triggered) even for a window built without a disk cache -- see
// confirmAndClearMediaDiskCache()'s own null handling. Not triggered here: it opens a modal
// QMessageBox, which this offscreen suite has no driver for.
void testClearMediaCacheActionExists(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "clear media cache: stand-in editors register");
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost,
                      fixture.frameExportController);
    auto* action =
        window.findChild<QAction*>(QStringLiteral("compositionClearMediaDiskCacheAction"));
    expectations.expect(action != nullptr && action->text() == QStringLiteral("Clear Media Cache…"),
                        "clear media cache: the Composition menu carries the command");
}

// A transient notice clears itself; a persistent one does not. The five-second life is asserted by
// driving the timer rather than by waiting on the wall clock.
void testWindowStatusBarMessagesClearThemselves(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "status bar messages: stand-in editors register");
    // A standalone strip, not the one inside a MainWindow: ProjectHost's own asynchronous startup
    // publishes activity messages through that window, and this test is about the strip's own
    // precedence and expiry rules rather than about who happens to be talking to it.
    WindowStatusBar standalone(fixture.compositionSession, nullptr);
    auto* strip = &standalone;

    strip->setPersistentMessage(QStringLiteral("Saving…"));
    expectations.expect(strip->messageTextForTest() == QStringLiteral("Saving…"),
                        "status bar messages: a persistent message shows");

    strip->showTransientMessage(QStringLiteral("Exported one frame"));
    expectations.expect(strip->messageTextForTest() == QStringLiteral("Exported one frame"),
                        "status bar messages: a notice takes precedence while it lasts");

    auto* timer = strip->findChild<QTimer*>();
    expectations.expect(timer != nullptr && timer->isSingleShot() && timer->isActive() &&
                            timer->interval() == 5000,
                        "status bar messages: a notice is armed to clear after exactly five "
                        "seconds");
    // The expiry is proved by the CONNECTION rather than by waiting five seconds: disconnect()
    // returns true only if the pair was actually connected, so this both asserts the wiring and
    // leaves the test deterministic. It is reconnected immediately afterwards.
    expectations.expect(timer != nullptr &&
                            QObject::disconnect(timer, &QTimer::timeout, strip,
                                                &WindowStatusBar::clearTransientMessage),
                        "status bar messages: the timeout is what clears the notice");
    if (timer != nullptr) {
        QObject::connect(timer, &QTimer::timeout, strip, &WindowStatusBar::clearTransientMessage);
    }
    strip->clearTransientMessage();
    expectations.expect(strip->messageTextForTest() == QStringLiteral("Saving…"),
                        "status bar messages: when the notice expires the persistent message is "
                        "what is left, not an empty strip");

    CompositionPreviewState warning;
    warning.activity = PreviewActivity::Ready;
    warning.diagnostics.push_back({"bloom.runtime.evaluation.color-space-missing",
                                   bloom::runtime::DiagnosticSeverity::Warning,
                                   "Colour transform bypassed: MissingInputColorSpace",
                                   {},
                                   {}});
    expectations.expect(
        previewActivityText(warning) ==
            QStringLiteral("Colour transform bypassed: MissingInputColorSpace"),
        "a ready pass-through frame displays its typed colour refusal in the status bar");
    strip->setPersistentMessage(QString{});
    expectations.expect(strip->messageTextForTest().isEmpty(),
                        "status bar messages: clearing the persistent message empties the cell");
}

// A refused command reaches the strip as a notice. CompositionSession::commandRejected() is the one
// signal every refusal already travels on, so this is the whole wiring.
void testRejectedCommandsBecomeStatusBarNotices(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "rejected commands: stand-in editors register");
    WindowStatusBar strip(fixture.compositionSession, nullptr);
    Q_EMIT fixture.compositionSession.commandRejected(QStringLiteral("Nothing to split"));
    expectations.expect(strip.messageTextForTest() == QStringLiteral("Nothing to split"),
                        "rejected commands: the refusal reason is what the strip says");
}

// D1 regression: the View menu's checkable Audio Playback action is a QAction, not a
// PreferencesAware widget, so applying a committed preference must explicitly mirror it. Without
// the fix the action keeps its stale check state and the artist's next click toggles from the
// wrong value (re-enabling audio instead of muting it).
void testPreferencesAudioActionMirrorsCommittedPreference(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "audio action: stand-in editors registered");
    if (!ok) {
        return;
    }
    // MainWindow builds the checkable View action only with a real PlaybackController.
    bloom::runtime::CpuCompositionEvaluator evaluator;
    bloom::runtime::CpuReferenceDisplayPreparer displayPreparer;
    bloom::runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    CompositionPreviewController previewController(
        fixture.compositionSession, fixture.scheduler, fixture.taskUiBridge,
        makeCompositionPreviewPipeline(fixture.snapshotCompiler, evaluator, displayPreparer,
                                       qualifiedProcessorProvider));
    PlaybackController playback(fixture.compositionSession, previewController);

    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost,
                      fixture.frameExportController, nullptr, &previewController, nullptr,
                      &playback);
    auto* audioAction = window.findChild<QAction*>(QStringLiteral("viewAudioEnabledAction"));
    expectations.expect(audioAction != nullptr && audioAction->isCheckable(),
                        "audio action: the View menu offers the checkable Audio Playback item");
    if (audioAction == nullptr) {
        previewController.beginShutdown();
        return;
    }

    audioAction->setChecked(true);
    expectations.expect(playback.isAudioEnabled(),
                        "audio action: checking it enables audio through the controller");
    // Force the desync a stale commit leaves behind: audio off in the controller while the action
    // still reads checked. The committed preference is the single source of truth.
    playback.setAudioEnabled(false);
    expectations.expect(
        audioAction->isChecked(),
        "audio action: the action can lag the controller before a preferences apply");

    ApplicationPreferences preferences;
    preferences.audioEnabled = false;
    window.applyCommittedPreferencesForTest(preferences);
    expectations.expect(!audioAction->isChecked(),
                        "audio action: applying audio-off mirrors the stale action to unchecked");

    preferences.audioEnabled = true;
    window.applyCommittedPreferencesForTest(preferences);
    expectations.expect(audioAction->isChecked(),
                        "audio action: applying audio-on mirrors the action to checked");

    previewController.beginShutdown();
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testChromeModeFromSettingsReadsTheInjectedFile(expectations);
    testLegacyLayoutMigration(expectations);
    testResetWorkspaceRestoresAndPersistsTheOwnerArrangement(expectations);
    testMainWindowAlwaysBuildsNativeChrome(expectations);
    testWindowTitleIsJustTheDocumentTitle(expectations);
    testViewMenuItemsExistAndFire(expectations);
    testHelpMenuItemsExistAndFire(expectations);
    testPreferencesActionIsInTheEditMenuWithPreferencesRole(expectations);
    testPreferencesAudioActionMirrorsCommittedPreference(expectations);
    testWindowStatusBarIsAKitStripWithEveryCell(expectations);
    testWindowStatusBarTrimsCachesUnderMemoryPressure(expectations);
    testWindowStatusBarMessagesClearThemselves(expectations);
    testRejectedCommandsBecomeStatusBarNotices(expectations);
    testClearMediaCacheActionExists(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}

#include "main_window_chrome_tests.moc"
