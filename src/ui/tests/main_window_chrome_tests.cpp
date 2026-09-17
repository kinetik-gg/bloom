#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/licenses_window.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/window_status_bar.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenuBar>
#include <QObject>
#include <QSettings>
#include <QStatusBar>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>

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
          "windowStatusBarCache", "windowStatusBarMessage", "windowStatusBarVersion"}) {
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

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testChromeModeFromSettingsReadsTheInjectedFile(expectations);
    testLegacyLayoutMigration(expectations);
    testMainWindowAlwaysBuildsNativeChrome(expectations);
    testWindowTitleIsJustTheDocumentTitle(expectations);
    testViewMenuItemsExistAndFire(expectations);
    testHelpMenuItemsExistAndFire(expectations);
    testWindowStatusBarIsAKitStripWithEveryCell(expectations);
    testWindowStatusBarMessagesClearThemselves(expectations);
    testRejectedCommandsBecomeStatusBarNotices(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}

#include "main_window_chrome_tests.moc"
