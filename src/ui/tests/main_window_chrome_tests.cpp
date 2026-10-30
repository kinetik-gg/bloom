#ifndef BLOOM_GRAMMAR_ARTIFACT_DIR
#define BLOOM_GRAMMAR_ARTIFACT_DIR "."
#endif
#include "window_fixture.hpp"
#undef BLOOM_GRAMMAR_ARTIFACT_DIR

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/cache_purge_controller.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/context_bar.hpp>
#include <bloom/ui/workspace_presets.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/split_handle.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/layer_alignment_actions.hpp>
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

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QJsonArray>
#include <QLineEdit>
#include <QKeyEvent>
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
#include <QTest>
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
// ProjectHost/CompositionSessionwiring inline -- mirrors tests/ui_smoke.cpp
// and main_window_readonly_placeholder_tests.cpp's own precedent for this construction.
struct Fixture final {
    EditorRegistry registry;
    bloom::runtime::TaskScheduler scheduler;
    ProjectHost projectHost{scheduler};
    CompositionSession compositionSession;
    bloom::runtime::NodeDefinitionRegistry nodeDefinitions;
    bloom::runtime::SnapshotCompiler snapshotCompiler{nodeDefinitions};
    TaskUiBridge taskUiBridge{scheduler};

    explicit Fixture(bool* ok)
        : compositionSession(*projectHost.liveDocumentAndStack().first,
                             *projectHost.liveDocumentAndStack().second,
                             projectHost.lowestCompositionId()) {
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
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost);
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
    expectations.expect(window.workspaceHost()->areaCount() == 4 &&
                            window.workspacePresets()->activeName() == QStringLiteral("Default"),
                        "legacy layout gains all panels of the Default preset");
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
        // The Assets | Viewer row is the top child of the Timeline area's own (vertical) splitter.
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
            "reset workspace: the default Assets/Viewer row and timeline are visible");
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
        expectations.expect(std::abs(share(sizes, 0) - 0.25) <= 0.02 &&
                                std::abs(share(sizes, 1) - 0.75) <= 0.02,
                            "reset workspace: the Assets/Viewer proportions are restored");
        expectations.expect(fixture.window->workspacePresets()->activeName() ==
                                    QStringLiteral("Default") &&
                                !fixture.window->workspacePresets()->isEdited(),
                            "reset workspace: the Workspace field reads Default");
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
        const auto savedLeftWeights = leftColumnNode.value(QStringLiteral("weights")).toArray();
        const auto savedTopWeights = leftColumnNode.value(QStringLiteral("children"))
                                         .toArray()
                                         .at(0)
                                         .toObject()
                                         .value(QStringLiteral("weights"))
                                         .toArray();
        expectations.expect(savedRootWeights.size() == 2 &&
                                std::abs(savedRootWeights.at(0).toDouble() - 0.80) <= 0.02 &&
                                std::abs(savedRootWeights.at(1).toDouble() - 0.20) <= 0.02 &&
                                savedLeftWeights.size() == 2 &&
                                std::abs(savedLeftWeights.at(0).toDouble() - 0.66) <= 0.02 &&
                                std::abs(savedLeftWeights.at(1).toDouble() - 0.34) <= 0.02 &&
                                savedTopWeights.size() == 2 &&
                                std::abs(savedTopWeights.at(0).toDouble() - 0.25) <= 0.02 &&
                                std::abs(savedTopWeights.at(1).toDouble() - 0.75) <= 0.02,
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

// Numpad 0 is the RAM Preview key (After Effects): a window shortcut on the Composition menu's
// action, which a keypad digit typed into a text field overrides (it is text input there).
void testNumpadZeroIsTheRamPreviewShortcut(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "numpad shortcut editors registered");
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost);
    auto* action = window.findChild<QAction*>(QStringLiteral("compositionRamPreviewAction"));
    expectations.expect(action != nullptr, "the Composition menu owns the RAM Preview action");
    if (action == nullptr)
        return;
    expectations.expect(action->shortcutContext() == Qt::WindowShortcut &&
                            action->shortcuts().contains(
                                QKeySequence(Qt::KeypadModifier | Qt::Key_0)) &&
                            action->shortcuts().contains(
                                QKeySequence(Qt::KeypadModifier | Qt::Key_Insert)) &&
                            action->shortcuts().contains(
                                QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Space)),
                        "Numpad 0 and Ctrl+Shift+Space are window shortcuts of one action");
    auto* cancel = window.findChild<QAction*>(QStringLiteral("cancelRamPreviewAction"));
    expectations.expect(cancel != nullptr && cancel->shortcut() == QKeySequence(Qt::Key_Escape) &&
                            !cancel->isEnabled(),
                        "Esc is the Cancel RAM Preview shortcut, armed only while a fill runs");
    expectations.expect(!action->shortcuts().contains(QKeySequence(Qt::Key_0)),
                        "the main-row 0 key is not a RAM Preview shortcut");
    // Inside a text field the digit is text input: the field accepts the ShortcutOverride, so the
    // window shortcut does not fire.
    QLineEdit field;
    field.show();
    field.setFocus();
    for (const auto modifiers : {Qt::KeypadModifier, Qt::NoModifier}) {
        QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_0, modifiers, QStringLiteral("0"));
        override.ignore();
        QCoreApplication::sendEvent(&field, &override);
        expectations.expect(override.isAccepted(),
                            "a zero typed into a text field is text input, not a shortcut");
    }
}

void testMainWindowAlwaysBuildsNativeChrome(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "native chrome: fixture editors registered");
    if (!ok) {
        return;
    }

    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost);
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

    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost);
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
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost);

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
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost);

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
    licensesAction->trigger();
    expectations.expect(window.findChildren<LicensesWindow*>().size() == 1,
                        "help menu: asking again raises the one Licenses window");
    expectations.expect(licensesWindow != nullptr &&
                            licensesWindow->findChild<QAbstractButton*>(
                                QStringLiteral("licensesCloseButton")) != nullptr &&
                            licensesWindow->minimumWidth() == kit::px(kit::Size::DialogWideWidth) &&
                            licensesWindow->windowModality() == Qt::NonModal &&
                            licensesAction->text() == QStringLiteral("Open Source Licenses…"),
                        "help menu: Licenses has a Close button, token sizes, and is non-modal");
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
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost);

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
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost);

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
         {"windowStatusBarViewer", "windowStatusBarColorChip", "windowStatusBarExposure",
          "windowStatusBarChannel", "windowStatusBarDroppedFrames", "windowStatusBarActivity",
          "windowStatusBarMessage", "windowStatusBarMemory", "windowStatusBarCache",
          "windowStatusBarDisk", "windowStatusBarMediaDiskCache", "windowStatusBarVersion"}) {
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
    expectations.expect(cacheStrip.memoryTextForTest().startsWith(QStringLiteral("RAM ")) &&
                            cacheStrip.memoryTextForTest().contains(QStringLiteral("/1.0 MB")),
                        "status bar: the RAM segment reads used/budget in one line");
    expectations.expect(
        cacheStrip.cacheToolTipForTest().contains(QStringLiteral("1 hits · 1 misses")) &&
            !cacheStrip.memoryTextForTest().contains(QStringLiteral("hits")) &&
            !cacheStrip.memoryTextForTest().contains(QStringLiteral("Ops")),
        "status bar: hit and miss counters are in the tooltip, never inline");
    // Every value has a visible label, and none is a bare number; MCP and Version labels included.
    const auto labels = strip->segmentLabelsForTest();
    expectations.expect(labels.size() == 7 && labels[0].contains(QStringLiteral("Render")) &&
                            labels[0].contains(QStringLiteral("Exposure")) &&
                            labels[0].contains(QStringLiteral("Channel")) &&
                            labels[0].contains(QStringLiteral("Dropped")) &&
                            labels[1] == QStringLiteral("Activity") &&
                            labels[3].contains(QStringLiteral("RAM")) &&
                            labels[3].contains(QStringLiteral("Cache")) &&
                            labels[4] == QStringLiteral("Disk cache") &&
                            labels[5] == QStringLiteral("MCP") &&
                            labels[6] == QStringLiteral("Version"),
                        "status bar: every segment carries a short visible label");
    expectations.expect(strip->activityTextForTest() == QStringLiteral("Idle") &&
                            !strip->segmentVisibleForTest(QStringLiteral("windowStatusBarMcpSegment")),
                        "status bar: idle placeholder, and no MCP text while the server is stopped");
    expectations.expect(strip->findChild<QWidget*>(QStringLiteral("windowStatusBarColorChip")) &&
                            strip->findChild<QWidget*>(QStringLiteral("windowStatusBarExposure"))
                                    ->font()
                                    .family() == bloom::ui::kit::font(bloom::ui::kit::TypeRole::Value).family(),
                        "status bar: values use the monospaced face");
    // CACHE-2: the window is built with no disk cache (this fixture matches Timeline/window
    // fixtures elsewhere that pass no ninth argument), so the cell reports "off" rather than an
    // invented hit rate.
    expectations.expect(strip->mediaDiskCacheTextForTest() == QStringLiteral("Disk cache off"),
                        "status bar: a window built without a media disk cache reports it as off");
}

void testWindowStatusBarReportsSelectedMemoryBudgets(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "memory budgets: stand-in editors register");
    constexpr auto gib = std::size_t{1024} * 1024 * 1024;
    bloom::runtime::MemoryBudgetLedger ledger(16 * gib, 16 * gib);
    ledger.setConfiguredTotal(12 * gib);
    bloom::runtime::OperationCache operationCache(10 * gib, ledger);
    WindowStatusBar strip(fixture.compositionSession, nullptr, nullptr, &operationCache, nullptr,
                          ledger);
    strip.pollMemoryPressureForTest(
        {.availableBytes = gib, .swapTotalBytes = 4 * gib, .swapUsedBytes = 2 * gib},
        bloom::runtime::MemoryBudgetLedger::Clock::time_point{});
    strip.pollMemoryPressureForTest(
        {.availableBytes = gib, .swapTotalBytes = 4 * gib, .swapUsedBytes = 3 * gib},
        bloom::runtime::MemoryBudgetLedger::Clock::time_point{} + std::chrono::seconds(5));
    expectations.expect(operationCache.byteBudget() == 10 * gib &&
                            ledger.state().configuredBytes == 12 * gib &&
                            !strip.messageTextForTest().contains(QStringLiteral("trimmed")),
                        "memory budgets: host free memory and swap do not reduce selected budgets");
    expectations.expect(
        strip.cacheToolTipForTest().contains(QStringLiteral("Current cache budget:")) &&
            strip.cacheToolTipForTest().contains(QStringLiteral("Requested cache total:")) &&
            strip.cacheToolTipForTest().contains(QStringLiteral("Available memory:")),
        "memory budgets: the tooltip distinguishes current and requested budgets");

    ledger.beginProcessSampling();
    const auto now = bloom::runtime::MemoryBudgetLedger::Clock::now();
    static_cast<void>(ledger.applySample(
        {.machine = {.availableBytes = 16 * gib},
         .physicalBytes = 16 * gib,
         .process = {.source = bloom::runtime::ProcessMemorySource::LinuxCgroupV2,
                     .status = bloom::runtime::ProcessMemoryStatus::Finite,
                     .sampledAt = now,
                     .membershipPath = "/fixture",
                     .domains = {{"/fixture", bloom::runtime::ProcessMemoryLimitKind::Finite,
                                  8 * gib, 6 * gib}},
                     .reclaimableBytes = {}}},
        now));
    strip.pollMemoryPressureForTest({.availableBytes = 16 * gib}, now);
    expectations.expect(
        strip.cacheToolTipForTest().contains(QStringLiteral("system memory limit of 8.0 GB")) &&
            strip.cacheToolTipForTest().contains(QStringLiteral("Operation cache: 8.0 GB")) &&
            strip.cacheToolTipForTest().contains(QStringLiteral("Requested cache total: 12.0 GB")),
        "memory budgets: the actual reduced budget, selected total and real limit stay distinct");
}

void testWindowStatusBarKeepsMemoryReasonDuringRamPreview(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "RAM tooltip: stand-in editors register");
    constexpr auto gib = std::size_t{1024} * 1024 * 1024;
    bloom::runtime::MemoryBudgetLedger ledger(16 * gib, 16 * gib);
    CompositionPreviewController preview(fixture.compositionSession, fixture.scheduler,
                                         fixture.taskUiBridge, {});
    WindowStatusBar strip(fixture.compositionSession, &preview, nullptr, nullptr, nullptr, ledger);
    preview.beginRamPreviewProgress(24);
    preview.setRamPreviewProgress(4, 3, 2);
    const auto includesBoth = [&strip] {
        return strip.cacheToolTipForTest().contains(QStringLiteral("Current cache budget:")) &&
               strip.cacheToolTipForTest().contains(QStringLiteral("4 retained of 24 requested")) &&
               strip.cacheToolTipForTest().contains(QStringLiteral("3 prepared this run")) &&
               strip.cacheToolTipForTest().contains(
                   QStringLiteral("Longest cached span: 2 frames"));
    };
    expectations.expect(includesBoth(), "RAM tooltip: progress preserves the memory explanation");
    strip.pollMemoryPressureForTest(16 * gib);
    expectations.expect(includesBoth(), "RAM tooltip: a memory refresh preserves preview progress");
    preview.endRamPreviewProgress();
    expectations.expect(
        strip.cacheToolTipForTest().contains(QStringLiteral("Current cache budget:")) &&
            !strip.cacheToolTipForTest().contains(QStringLiteral("retained of")),
        "RAM tooltip: ending the run clears progress while keeping the memory explanation");
}

// CACHE-PURGE: Edit | Purge… carries exactly "Purge preview cache" and "Purge media cache", and the
// old Composition | Clear Media Cache… command is gone. Both purge actions stay present and enabled
// for a window built without any cache, matching the "report unavailability when it runs" rule.
void testPurgeMenuActionsExistAndAreInTheEditMenu(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "purge menu: stand-in editors register");
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost);

    auto* purgeMenu = window.findChild<QMenu*>(QStringLiteral("editPurgeMenu"));
    expectations.expect(purgeMenu != nullptr, "purge menu: the Edit menu carries a Purge submenu");

    QMenu* editMenu = nullptr;
    for (QAction* action : window.menuBar()->actions()) {
        if (action->text() == QStringLiteral("&Edit") && action->menu() != nullptr) {
            editMenu = action->menu();
        }
    }
    expectations.expect(editMenu != nullptr &&
                            editMenu->actions().contains(purgeMenu->menuAction()),
                        "purge menu: it lives in Edit, not Composition");

    auto* previewAction = window.findChild<QAction*>(QStringLiteral("purgePreviewCacheAction"));
    auto* mediaAction = window.findChild<QAction*>(QStringLiteral("purgeMediaCacheAction"));
    expectations.expect(previewAction != nullptr &&
                            previewAction->text() == QStringLiteral("Purge preview cache") &&
                            previewAction->isEnabled(),
                        "purge menu: the preview action has the exact label and is discoverable");
    expectations.expect(mediaAction != nullptr &&
                            mediaAction->text() == QStringLiteral("Purge media cache") &&
                            mediaAction->isEnabled(),
                        "purge menu: the media action has the exact label and is discoverable");
    expectations.expect(window.findChild<QAction*>(
                            QStringLiteral("compositionClearMediaDiskCacheAction")) == nullptr,
                        "purge menu: the superseded Composition clear command is gone");
}

// CACHE-PURGE: triggering "Purge preview cache" has a real effect through the window wiring -- the
// evaluator's derived operation results are dropped -- while the project revision, dirty state, and
// undo history are unaffected (a purge is derived-cache maintenance, never a project edit). This
// window is built without a purge controller, so it exercises the memory-only fallback; the
// asynchronous controller path (disk store, video memory, GPU-scene stores) is asserted in
// media_disk_cache_settings_tests.cpp.
void testPurgePreviewCacheActionHasRealEffect(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "purge preview: stand-in editors register");
    bloom::runtime::MemoryBudgetLedger ledger(std::size_t{1} << 30U, std::size_t{1} << 30U);
    bloom::runtime::OperationCache operationCache(std::size_t{1} << 20U, ledger);
    operationCache.store("derived", bloom::document::Revision::fromRaw(1),
                         {.image = {}, .values = {}, .bounds = {}});
    operationCache.store("decoded", bloom::document::Revision::fromRaw(1),
                         {.image = {}, .values = {}, .bounds = {}},
                         bloom::runtime::OperationCacheEntryKind::DecodedMedia);
    expectations.expect(operationCache.retainedBytes() > 0,
                        "purge preview: the operation cache holds entries before the purge");

    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost, nullptr, nullptr, nullptr, nullptr,
                      &operationCache);

    const auto revisionBefore = fixture.compositionSession.snapshot().revision();
    const auto undoBefore = fixture.compositionSession.undoLabel();
    const bool canUndoBefore = fixture.compositionSession.canUndo();

    auto* action = window.findChild<QAction*>(QStringLiteral("purgePreviewCacheAction"));
    expectations.expect(action != nullptr, "purge preview: the action exists to trigger");
    action->trigger();

    expectations.expect(
        operationCache.retainedBytes(bloom::runtime::OperationCacheEntryKind::Operation) == 0,
        "purge preview: derived operation results are dropped");
    expectations.expect(
        operationCache.retainedBytes(bloom::runtime::OperationCacheEntryKind::DecodedMedia) > 0,
        "purge preview: decoded media is left for Purge media cache");
    expectations.expect(fixture.compositionSession.snapshot().revision() == revisionBefore &&
                            fixture.compositionSession.undoLabel() == undoBefore &&
                            fixture.compositionSession.canUndo() == canUndoBefore,
                        "purge preview: project revision and undo history are untouched");
    expectations.expect(window.statusStrip()->messageTextForTest() ==
                            QStringLiteral("Preview cache purged"),
                        "purge preview: the status bar reports the completed purge");
}

void testMediaPurgeDropsCompletedSourceFrames(Expectations& expectations) {
    bloom::ui::test::WindowFixture fixture;
    const auto revision = fixture.session.snapshot().revision();
    const auto undo = fixture.session.undoLabel();
    const auto displayed = fixture.preview->state().frame;
    expectations.expect(fixture.preview->frameCache().size() > 0 && displayed,
                        "media refresh starts with a completed cached frame");
    auto* action = fixture.window->findChild<QAction*>(QStringLiteral("purgeMediaCacheAction"));
    expectations.expect(action != nullptr, "media refresh action exists");
    if (!action)
        return;
    action->trigger();
    expectations.expect(fixture.preview->frameCache().size() == 0 &&
                            fixture.preview->state().frame == displayed,
                        "memory-only media refresh drops cached frames while preserving display");

    fixture.preview->requestRefresh();
    QElapsedTimer timer;
    timer.start();
    while (fixture.preview->frameCache().size() == 0 && timer.elapsed() < 15000)
        QTest::qWait(5);
    expectations.expect(fixture.preview->frameCache().size() > 0,
                        "a fresh request repopulates the completed-frame cache");
    fixture.window.reset();
    CachePurgeController purge(
        fixture.scheduler, fixture.bridge, nullptr, fixture.evaluator.operationCache().get(),
        nullptr, nullptr, {},
        [&fixture](const bool gated) { fixture.preview->setCachePurgeGate(gated); });
    fixture.window = std::make_unique<MainWindow>(
        fixture.registry, fixture.session, fixture.projectHost, nullptr,
        fixture.preview.get(), nullptr, nullptr, fixture.evaluator.operationCache().get(), nullptr,
        nullptr, &purge);
    action = fixture.window->findChild<QAction*>(QStringLiteral("purgeMediaCacheAction"));
    expectations.expect(action != nullptr, "asynchronous media refresh action exists");
    if (!action) {
        fixture.window.reset();
        return;
    }
    const auto insertions = fixture.preview->frameCache().statistics().insertions;
    action->trigger();
    expectations.expect(fixture.preview->cachePurgeGateActive() &&
                            fixture.preview->frameCache().size() == 0,
                        "asynchronous media refresh clears completed frames under producer gate");
    timer.restart();
    while ((purge.isPurging() ||
            fixture.preview->frameCache().statistics().insertions == insertions) &&
           timer.elapsed() < 15000)
        QTest::qWait(5);
    expectations.expect(!purge.isPurging() && !fixture.preview->cachePurgeGateActive() &&
                            fixture.preview->frameCache().statistics().insertions > insertions,
                        "gate release computes a new frame instead of reusing a pre-refresh hit");
    expectations.expect(fixture.session.snapshot().revision() == revision &&
                            fixture.session.undoLabel() == undo,
                        "media refresh leaves project revision and undo unchanged");
    fixture.window.reset();
}

// ADR 0031: the context bar sits directly under the menu bar, above the workspace; Window > Context
// Bar hides it and the choice persists with the workspace UI state; the bar acts on the Viewer the
// status strip follows; Composition > Align offers the bar's own alignment actions.
void testContextBarIsAFixedSurfaceUnderTheMenuBar(Expectations& expectations) {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("bar.ini"));
    {
        bloom::ui::test::WindowFixture fixture;
        auto* bar = fixture.window->contextBar();
        auto* toggle = fixture.window->contextBarAction();
        expectations.expect(bar != nullptr && bar->isVisible() && toggle != nullptr &&
                                toggle->isCheckable() && toggle->isChecked() &&
                                toggle->text() == QStringLiteral("Toolbar"),
                            "the context bar is shown, with a checked Window > Toolbar");
        if (bar == nullptr || toggle == nullptr)
            return;
        auto* workspace = fixture.window->workspaceHost();
        expectations.expect(bar->mapTo(fixture.window.get(), QPoint()).y() <
                                    workspace->mapTo(fixture.window.get(), QPoint()).y() &&
                                bar->width() == workspace->width(),
                            "the bar spans the window above the workspace");
        QTest::qWait(20);
        expectations.expect(bar->viewer() != nullptr &&
                                bar->viewer() ==
                                    fixture.window->statusStrip()->displayedViewerForTest(),
                            "the bar acts on the Viewer the window follows");
        auto* composition = fixture.window->findChild<QMenu*>(QStringLiteral("alignMenu"));
        expectations.expect(composition != nullptr &&
                                composition->actions().contains(bar->alignment().action(
                                    bloom::commands::LayerArrangement::AlignLeft)),
                            "Composition > Align offers the bar's own Align actions");
        toggle->trigger();
        expectations.expect(!bar->isVisible() && !toggle->isChecked(),
                            "Window > Toolbar hides the bar");
        QSettings settings(path, QSettings::IniFormat);
        fixture.window->saveApplicationState(settings);
        settings.sync();
    }
    {
        bloom::ui::test::WindowFixture fixture;
        QSettings settings(path, QSettings::IniFormat);
        (void)fixture.window->restoreApplicationState(settings);
        expectations.expect(!fixture.window->contextBar()->isVisible() &&
                                !fixture.window->contextBarAction()->isChecked(),
                            "a hidden context bar stays hidden in the next session");
    }
}

// The window follows its viewers through coalesced deferred updates, so a state is awaited, never
// assumed after a fixed delay.
template <typename Condition> bool eventually(Condition condition) {
    return QTest::qWaitFor(std::move(condition), 5000);
}

void testDisplayedViewerStatusPolicy(Expectations& expectations) {
    bloom::ui::test::WindowFixture fixture;
    auto* workspace = fixture.window->workspaceHost();
    auto* status = fixture.window->statusStrip();
    workspace->resetToSingleArea("bloom.viewer");
    auto* firstArea = workspace->activeArea();
    ViewerEditor* first = nullptr;
    expectations.expect(eventually([&] {
                            first = firstArea->findChild<ViewerEditor*>();
                            return first && status->displayedViewerForTest() == first;
                        }),
                        "active visible viewer owns global display status");
    auto* secondArea = workspace->splitArea(*firstArea, Qt::Horizontal, "bloom.viewer");
    ViewerEditor* second = nullptr;
    expectations.expect(eventually([&] {
                            second = secondArea ? secondArea->findChild<ViewerEditor*>() : nullptr;
                            return second != nullptr;
                        }),
                        "a second real viewer is created");
    if (!second)
        return;
    auto* channel = secondArea->findChild<kit::KDropdown*>("viewerChannelDropdown");
    expectations.expect(channel != nullptr,
                        "the second area's hosted footer has its channel control");
    if (!channel)
        return;
    channel->setCurrentIndex(static_cast<int>(ViewerChannel::Red));
    workspace->setActiveArea(secondArea);
    expectations.expect(eventually([&] {
                            return status->displayedViewerForTest() == second &&
                                   status->previewStateTextForTest().contains("Red");
                        }),
                        "active second viewer reports its own painted channel intent");
    auto* nodesArea = workspace->splitArea(*secondArea, Qt::Vertical, "bloom.nodes");
    workspace->setActiveArea(nodesArea);
    expectations.expect(eventually([&] { return status->displayedViewerForTest() == second; }),
                        "a non-viewer active panel retains the last-active visible viewer");
    ProbeReadout readout;
    readout.valid = true;
    readout.display = {11, 22, 33, 255};
    Q_EMIT second->probeChanged(readout);
    auto* probe = status->findChild<QLabel*>("windowStatusBarProbe");
    expectations.expect(probe && !probe->text().isEmpty(),
                        "selected viewer supplies the probe cell");
    workspace->toggleMaximizeActiveArea();
    expectations.expect(eventually([&] {
                            return !status->displayedViewerForTest() &&
                                   status->colorChipTextForTest() == "No visible viewer" &&
                                   status->previewStateTextForTest().isEmpty() &&
                                   probe->text().isEmpty();
                        }),
                        "no visible viewer clears displayed qualification, activity and probe");
    workspace->toggleMaximizeActiveArea();
    expectations.expect(eventually([&] { return status->displayedViewerForTest() == second; }),
                        "restoring visibility restores the last-active viewer");
    Q_EMIT second->probeChanged(readout);
    expectations.expect(secondArea->setEditorId("bloom.nodes"),
                        "selected viewer replacement succeeds");
    expectations.expect(
        eventually(
            [&] { return status->displayedViewerForTest() == first && probe->text().isEmpty(); }),
        "destroyed selected viewer clears its probe and falls back to first visible viewer");
}

// Priority rows never let a hosted control leave its row: at each window width, every visible
// chrome control in every panel's header and footer, and every visible control in the context bar,
// lies inside the row that hosts it (a control that does not fit collapses into the overflow menu
// instead). Panels are shown one at a time so each gets the narrow widths.
void testPriorityRowsKeepControlsInside(Expectations& expectations) {
    bloom::ui::test::WindowFixture fixture;
    auto* workspace = fixture.window->workspaceHost();
    const auto escapes = [](QWidget& row, const bool onlyChromeControls) {
        std::string offenders;
        const QRect bounds = row.rect();
        for (auto* control : row.findChildren<QWidget*>()) {
            if (!control->isVisibleTo(&row) || control->isWindow())
                continue;
            if (onlyChromeControls ? !control->property("chromeControl").toBool()
                                   : !(qobject_cast<QAbstractButton*>(control) != nullptr ||
                                       qobject_cast<kit::KDropdown*>(control) != nullptr ||
                                       qobject_cast<kit::KValueField*>(control) != nullptr))
                continue;
            const QRect placed(control->mapTo(&row, QPoint()), control->size());
            if (!bounds.contains(placed))
                offenders += ' ' + control->objectName().toStdString();
        }
        return offenders;
    };
    for (const char* editor :
         {"bloom.viewer", "bloom.timeline", "bloom.nodes", "bloom.assets", "bloom.properties"}) {
        workspace->resetToSingleArea(editor);
        for (const int width : {640, 800, 1000, 1280, 1920}) {
            fixture.window->resize(width, 900);
            QTest::qWait(30);
            QApplication::processEvents();
            const std::string at = std::string{editor} + " at " + std::to_string(width) + " px";
            for (auto* area : fixture.window->findChildren<EditorArea*>()) {
                for (const char* rowName : {"editorHeader", "editorFooter"}) {
                    auto* row = area->findChild<QWidget*>(QString::fromLatin1(rowName));
                    if (row == nullptr || !row->isVisible())
                        continue;
                    const auto offenders = escapes(*row, true);
                    expectations.expect(offenders.empty(), std::string{rowName} +
                                                               " keeps its "
                                                               "controls inside: " +
                                                               at + offenders);
                }
            }
            if (auto* bar = fixture.window->contextBar(); bar != nullptr && bar->isVisible()) {
                const auto offenders = escapes(*bar, false);
                expectations.expect(offenders.empty(),
                                    "the context bar keeps its controls inside: " + at + offenders);
            }
        }
    }
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
                                   "Color transform bypassed: MissingInputColorSpace",
                                   {},
                                   {}});
    expectations.expect(
        previewActivityText(warning) ==
            QStringLiteral("Color transform bypassed: MissingInputColorSpace"),
        "a ready pass-through frame displays its typed color refusal in the status bar");
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

    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost, nullptr, &previewController, nullptr,
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

void testProjectColorDialogPreservesCurrentIntent(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "color settings: stand-in editors registered");
    if (!ok)
        return;
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost);
    auto* action = window.findChild<QAction*>(QStringLiteral("projectColorSettingsAction"));
    expectations.expect(action != nullptr, "color settings: project action exists");
    if (!action)
        return;

    const auto choose = [&](const QString& expectedConfig, const QString& expectedWorking,
                            const QString& changeConfig = {}) {
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            auto* config =
                dialog ? dialog->findChild<kit::KDropdown*>("projectColorConfigPicker") : nullptr;
            auto* working =
                dialog ? dialog->findChild<kit::KDropdown*>("projectWorkingColorSpacePicker")
                       : nullptr;
            expectations.expect(config && working, "color settings: dialog has both selectors");
            if (config && working) {
                expectations.expect(config->currentData().toString() == expectedConfig &&
                                        working->currentData().toString() == expectedWorking,
                                    "color settings: dialog opens on the saved intent");
                if (!changeConfig.isEmpty())
                    config->setCurrentIndex(config->findData(changeConfig));
            }
            if (dialog)
                dialog->accept();
        });
        action->trigger();
        // The dialog is asynchronous: let it open, be answered and close.
        QTest::qWait(20);
        (void)QTest::qWaitFor(
            [&window] {
                auto* open =
                    window.findChild<QDialog*>(QStringLiteral("projectColorSettingsDialog"));
                return open == nullptr || !open->isVisible();
            },
            2000);
        QApplication::processEvents();
    };

    auto aces = *fixture.projectHost.colorSettings();
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    expectations.expect(revision.has_value(), "color settings: ACES revision resolves");
    if (!revision)
        return;
    aces.ocioConfig.locator =
        bloom::document::BuiltInOcioConfigLocator{std::string(bloom::color::kAcesCgV1ConfigUri)};
    aces.ocioConfig.expectedRevision.digest = *revision;
    aces.processColorSpaceId = "Linear P3-D65";
    expectations.expect(fixture.projectHost.setColorSettings(aces) ==
                            bloom::host::ProjectSessionColorSettingsStatus::Updated,
                        "color settings: ACES setup is accepted");
    choose("aces", "Linear P3-D65");
    expectations.expect(*fixture.projectHost.colorSettings() == aces,
                        "color settings: unchanged ACES dialog retains nondefault working space");

    auto external = aces;
    external.ocioConfig.locator =
        bloom::document::ExternalOcioConfigLocator{"file:///show/config.ocio"};
    external.ocioConfig.portability = bloom::document::OcioConfigPortability::External;
    external.ocioConfig.contextVariables = {{"SHOW", "sample"}};
    expectations.expect(fixture.projectHost.setColorSettings(external) ==
                            bloom::host::ProjectSessionColorSettingsStatus::Updated,
                        "color settings: external setup is accepted");
    choose("current", "Linear P3-D65");
    expectations.expect(*fixture.projectHost.colorSettings() == external,
                        "color settings: unchanged external dialog retains locator and revision");

    choose("current", "Linear P3-D65", "neutral");
    const auto changed = fixture.projectHost.colorSettings();
    const auto* builtIn =
        changed == nullptr
            ? nullptr
            : std::get_if<bloom::document::BuiltInOcioConfigLocator>(&changed->ocioConfig.locator);
    expectations.expect(builtIn && builtIn->uri == bloom::color::kBloomNeutralV1ConfigUri &&
                            changed->processColorSpaceId == "lin_rec709_scene",
                        "color settings: an explicit config choice updates the project");
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
    testNumpadZeroIsTheRamPreviewShortcut(expectations);
    testWindowTitleIsJustTheDocumentTitle(expectations);
    testViewMenuItemsExistAndFire(expectations);
    testHelpMenuItemsExistAndFire(expectations);
    testPreferencesActionIsInTheEditMenuWithPreferencesRole(expectations);
    testPreferencesAudioActionMirrorsCommittedPreference(expectations);
    testProjectColorDialogPreservesCurrentIntent(expectations);
    testWindowStatusBarIsAKitStripWithEveryCell(expectations);
    testWindowStatusBarReportsSelectedMemoryBudgets(expectations);
    testWindowStatusBarKeepsMemoryReasonDuringRamPreview(expectations);
    testWindowStatusBarMessagesClearThemselves(expectations);
    try {
        testDisplayedViewerStatusPolicy(expectations);
        testPriorityRowsKeepControlsInside(expectations);
        testContextBarIsAFixedSurfaceUnderTheMenuBar(expectations);
        testMediaPurgeDropsCompletedSourceFrames(expectations);
    } catch (const std::exception& error) {
        expectations.expect(false, std::string("displayed viewer fixture: ") + error.what());
    } catch (...) {
        expectations.expect(false, "displayed viewer fixture: unexpected exception");
    }
    testRejectedCommandsBecomeStatusBarNotices(expectations);
    testPurgeMenuActionsExistAndAreInTheEditMenu(expectations);
    testPurgePreviewCacheActionHasRealEffect(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}

#include "main_window_chrome_tests.moc"
