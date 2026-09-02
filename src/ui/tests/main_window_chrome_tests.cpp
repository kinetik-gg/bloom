#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/kit/title_bar.hpp>
#include <bloom/ui/licenses_window.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QLabel>
#include <QMenuBar>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
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
           addTestEditor("bloom.timeline", "Timeline") && addTestEditor("bloom.media", "Media") &&
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

void testCustomChromeBuildsAFramelessTitleBarWindow(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "custom chrome: fixture editors registered");
    if (!ok) {
        return;
    }

    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost,
                      fixture.frameExportController, ChromeMode::Custom);
    expectations.expect(window.chromeMode() == ChromeMode::Custom,
                        "custom chrome: chromeMode() reports what it was constructed with");
    expectations.expect(window.windowFlags().testFlag(Qt::FramelessWindowHint),
                        "custom chrome: the window is really frameless");

    auto* titleBar = window.findChild<kit::TitleBar*>();
    expectations.expect(titleBar != nullptr, "custom chrome: a TitleBar exists");
    if (titleBar == nullptr) {
        return;
    }
    expectations.expect(window.menuWidget() == titleBar,
                        "custom chrome: the TitleBar is installed as the menu widget");
    expectations.expect(titleBar->findChild<QMenuBar*>() != nullptr,
                        "custom chrome: the menu bar is embedded inside the title bar's row");

    // Title follows the session (decision 1): a fresh project shows "Bloom — Untitled".
    auto* label = titleBar->findChild<QLabel*>(QStringLiteral("titleBarTitleLabel"));
    expectations.expect(label != nullptr && label->text() == QStringLiteral("Bloom — Untitled"),
                        "custom chrome: the title bar's label follows the project name");

    // Maximize state propagation: MainWindow::changeEvent syncs the TitleBar's appearance.
    expectations.expect(!titleBar->maximizedAppearance(), "custom chrome: starts unmaximized");
    window.setWindowState(window.windowState() | Qt::WindowMaximized);
    QApplication::sendPostedEvents();
    expectations.expect(titleBar->maximizedAppearance(),
                        "custom chrome: a WindowStateChange event syncs the title bar's glyph");
}

void testNativeChromeKeepsTheClassicMenuBar(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    expectations.expect(ok, "native chrome: fixture editors registered");
    if (!ok) {
        return;
    }

    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost,
                      fixture.frameExportController, ChromeMode::Native);
    expectations.expect(window.chromeMode() == ChromeMode::Native,
                        "native chrome: chromeMode() reports Native");
    expectations.expect(!window.windowFlags().testFlag(Qt::FramelessWindowHint),
                        "native chrome: stock decorations, not frameless");
    expectations.expect(window.findChild<kit::TitleBar*>() == nullptr,
                        "native chrome: no TitleBar is constructed at all");
    expectations.expect(window.menuBar() != nullptr && window.menuWidget() == window.menuBar(),
                        "native chrome: QMainWindow's own classic menu bar is in charge");
}

void testViewMenuItemsExistAndFire(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    if (!ok) {
        expectations.expect(false, "view menu: fixture editors registered");
        return;
    }
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost,
                      fixture.frameExportController, ChromeMode::Custom);

    auto* fullScreenAction = window.findChild<QAction*>(QStringLiteral("viewFullScreenAction"));
    auto* maximizePanelAction =
        window.findChild<QAction*>(QStringLiteral("viewMaximizePanelAction"));
    auto* nativeFrameAction = window.findChild<QAction*>(QStringLiteral("useNativeFrameAction"));
    expectations.expect(fullScreenAction != nullptr && maximizePanelAction != nullptr &&
                            nativeFrameAction != nullptr,
                        "view menu: Full Screen, Maximize Panel, and Use Native Window Frame all "
                        "exist");
    if (fullScreenAction == nullptr || maximizePanelAction == nullptr ||
        nativeFrameAction == nullptr) {
        return;
    }

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

    expectations.expect(nativeFrameAction->isCheckable() && !nativeFrameAction->isChecked(),
                        "view menu: Use Native Window Frame reflects the Custom chrome it was "
                        "constructed with");
}

void testHelpMenuItemsExistAndFire(Expectations& expectations) {
    bool ok = false;
    Fixture fixture(&ok);
    if (!ok) {
        expectations.expect(false, "help menu: fixture editors registered");
        return;
    }
    MainWindow window(fixture.registry, fixture.compositionSession, fixture.projectHost,
                      fixture.frameExportController, ChromeMode::Custom);

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

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testChromeModeFromSettingsReadsTheInjectedFile(expectations);
    testCustomChromeBuildsAFramelessTitleBarWindow(expectations);
    testNativeChromeKeepsTheClassicMenuBar(expectations);
    testViewMenuItemsExistAndFire(expectations);
    testHelpMenuItemsExistAndFire(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}

#include "main_window_chrome_tests.moc"
