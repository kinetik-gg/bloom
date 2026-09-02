#pragma once

#include <QMainWindow>

#include <cstdint>

class QAction;
class QCloseEvent;
class QLabel;
class QMenu;
class QMenuBar;
class QResizeEvent;
class QSettings;
class QStackedWidget;

namespace bloom::ui::kit {
class TitleBar;
} // namespace bloom::ui::kit

namespace bloom::ui {

class CompositionSession;
class EditorRegistry;
class FrameExportController;
class FramelessEdgeResizer;
class ProjectHost;
class WorkspaceHost;
enum class WorkspaceLayoutRestoreResult;

// Custom window chrome, default on, with a persisted native fallback (task U2, issue #118,
// decision 1). Read via chromeModeFromSettings() at startup, in apps/bloom/main.cpp, BEFORE
// MainWindow is constructed -- QSettings can only be read correctly once QCoreApplication's
// organization/application name is set, and MainWindow's window flags must be right from its very
// first construction, not patched in afterward, so the mode is a constructor argument rather than
// something MainWindow discovers for itself.
enum class ChromeMode : std::uint8_t {
    Custom,
    Native,
};

// "appearance/chrome" = "custom" (default) | "native". Any other or missing value reads as
// Custom. Shared by apps/bloom/main.cpp's startup read and by MainWindow's own "Use Native Window
// Frame" View-menu toggle, so both agree on exactly what the setting means.
[[nodiscard]] ChromeMode chromeModeFromSettings(const QSettings& settings);
void setChromeModeInSettings(QSettings& settings, ChromeMode mode);

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    MainWindow(const EditorRegistry& editorRegistry, CompositionSession& compositionSession,
               ProjectHost& projectHost, FrameExportController& frameExportController,
               ChromeMode chromeMode = ChromeMode::Custom, QWidget* parent = nullptr);

    [[nodiscard]] WorkspaceHost* workspaceHost() const noexcept;
    [[nodiscard]] WorkspaceLayoutRestoreResult restoreApplicationState(QSettings& settings);
    void saveApplicationState(QSettings& settings) const;
    // Presentation-level read-only surface (task R1, issue #74): true exactly when the central
    // QStackedWidget's current page is the read-only placeholder instead of the editor workspace
    // -- i.e. the live ProjectHost content kind is PreservedReadOnly. Exposed so an offscreen test
    // can assert the switch without depending on QWidget::isVisible(), which only reports
    // correctly once the top-level window itself has been shown.
    [[nodiscard]] bool isShowingReadOnlyPlaceholder() const noexcept;
    [[nodiscard]] ChromeMode chromeMode() const noexcept;

  signals:
    void shutdownRequested();

  protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void createChrome();
    void createMenus(QMenuBar& menuBar);
    void createFileMenu(QMenu& fileMenu);
    void createViewMenu(QMenu& viewMenu);
    void createHelpMenu(QMenu& helpMenu);
    void createWorkspaceSwitcher(QMenuBar& menuBar);
    void createEditorLayout(const EditorRegistry& editorRegistry);
    void createCentralStack();
    QWidget* createReadOnlyPlaceholderPage();
    void createWorkspaceActions();
    void resetCompositingLayout();
    void updateEditActions();
    void updateWorkspaceActions();
    void updateFileActions();
    void updateExportAction();
    void updateWindowTitle();
    void updateContentSurface();
    void updateWindowMask();
    void toggleFullScreen();
    void toggleMaximizeRestore();

    CompositionSession& compositionSession_;
    ProjectHost& projectHost_;
    FrameExportController& frameExportController_;
    ChromeMode chromeMode_;
    kit::TitleBar* titleBar_ = nullptr;
    QMenuBar* menuBar_ = nullptr;
    FramelessEdgeResizer* edgeResizer_ = nullptr;
    QMenu* windowMenu_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    QStackedWidget* centralStack_ = nullptr;
    WorkspaceHost* workspaceHost_ = nullptr;
    QWidget* readOnlyPlaceholderPage_ = nullptr;
    QLabel* readOnlyPlaceholderFileNameLabel_ = nullptr;
    QLabel* readOnlyPlaceholderBodyLabel_ = nullptr;
    QAction* newProjectAction_ = nullptr;
    QAction* openProjectAction_ = nullptr;
    QAction* saveProjectAction_ = nullptr;
    QAction* saveProjectAsAction_ = nullptr;
    QAction* saveProjectCopyAction_ = nullptr;
    QAction* exportFrameAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* splitLeftRightAction_ = nullptr;
    QAction* splitTopBottomAction_ = nullptr;
    QAction* closeAreaAction_ = nullptr;
    QAction* maximizeAreaAction_ = nullptr;
    QAction* viewFullScreenAction_ = nullptr;
    QAction* viewMaximizePanelAction_ = nullptr;
    QAction* useNativeFrameAction_ = nullptr;
    QAction* reportIssueAction_ = nullptr;
    QAction* openSourceLicensesAction_ = nullptr;
    bool workspaceLayoutWritable_ = true;
    bool shutdownRequested_ = false;
};

} // namespace bloom::ui
