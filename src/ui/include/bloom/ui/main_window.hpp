#pragma once

#include <QMainWindow>

#include <cstdint>

class QAction;
class QCloseEvent;
class QLabel;
class QMenu;
class QMenuBar;
class QSettings;
class QStackedWidget;

namespace bloom::runtime {
class OperationCache;
}

namespace bloom::media::cache {
class MediaDiskCache;
} // namespace bloom::media::cache

namespace bloom::ui {

class CompositionPreviewController;
class CompositionSession;
class EditorRegistry;
class FrameExportController;
class ProjectHost;
class RamPreviewController;
class PlaybackController;
class WindowStatusBar;
class WorkspaceHost;
enum class WorkspaceLayoutRestoreResult;

// Historical custom-chrome setting (task U2, issue #118). Superseded by task C1 ("let OS handle
// the native window chrome for now"): MainWindow now always builds native (server-side) window
// decorations and never reads or writes this setting. ChromeMode and the two free functions below
// stay compiled and independently tested (main_window_chrome_tests.cpp's own
// chromeModeFromSettings coverage) alongside kit::TitleBar (kit_title_bar_tests.cpp) and
// FramelessEdgeResizer (frameless_window_support_tests.cpp) purely so a future custom-chrome/CSD
// return has working, tested pieces to build on -- nothing in the live application constructs or
// consults any of them today.
enum class ChromeMode : std::uint8_t {
    Custom,
    Native,
};

// "appearance/chrome" = "custom" (default) | "native". Any other or missing value reads as
// Custom. No in-app settings UI writes this key anymore (task C1 removed the View menu's "Use
// Native Window Frame" toggle); kept only for the round trip these two functions have always had.
[[nodiscard]] ChromeMode chromeModeFromSettings(const QSettings& settings);
void setChromeModeInSettings(QSettings& settings, ChromeMode mode);

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    // `ramPreview` is the Composition menu's RAM Preview command (task PERF1, item 3), the same
    // controller the Timeline's transport button reaches. Null leaves the menu item present and
    // disabled, which is what a window built without one should show.
    // `previewController` feeds the window status bar's preview cells (task VIEW-1) -- the colour
    // state, readiness, dropped frames and cache progress. Null builds the strip with those cells
    // empty rather than absent, which is what a window without a preview pipeline should show.
    // `operationCache` feeds the window status bar's combined cache cell with hit/miss/retained-
    // byte statistics for the evaluator's shared operation cache, appended beside the RAM preview
    // text. Null leaves that cell showing only the RAM preview account.
    // `mediaDiskCache` feeds the Composition menu's "Clear Media Cache…" command and the window
    // status bar's disk-cache cell (docs/architecture/media-io.md "Disk cache"). Null leaves the
    // menu item present but reporting "not enabled" rather than absent, matching `ramPreview`'s
    // own null convention above.
    MainWindow(const EditorRegistry& editorRegistry, CompositionSession& compositionSession,
               ProjectHost& projectHost, FrameExportController& frameExportController,
               RamPreviewController* ramPreview = nullptr,
               CompositionPreviewController* previewController = nullptr, QWidget* parent = nullptr,
               PlaybackController* playbackController = nullptr,
               runtime::OperationCache* operationCache = nullptr,
               media::cache::MediaDiskCache* mediaDiskCache = nullptr);

    [[nodiscard]] WorkspaceHost* workspaceHost() const noexcept;
    [[nodiscard]] WorkspaceLayoutRestoreResult restoreApplicationState(QSettings& settings);
    void saveApplicationState(QSettings& settings) const;
    // Presentation-level read-only surface (task R1, issue #74): true exactly when the central
    // QStackedWidget's current page is the read-only placeholder instead of the editor workspace
    // -- i.e. the live ProjectHost content kind is PreservedReadOnly. Exposed so an offscreen test
    // can assert the switch without depending on QWidget::isVisible(), which only reports
    // correctly once the top-level window itself has been shown.
    [[nodiscard]] bool isShowingReadOnlyPlaceholder() const noexcept;
    // The one persistent reporting surface (task VIEW-1). Exposed so a test can read what the
    // window is currently saying without grabbing pixels.
    [[nodiscard]] WindowStatusBar* statusStrip() const noexcept { return statusStrip_; }

  signals:
    void shutdownRequested();

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void createChrome();
    void createMenus(QMenuBar& menuBar);
    void createFileMenu(QMenu& fileMenu);
    void createCompositionMenu(QMenu& compositionMenu);
    void createViewMenu(QMenu& viewMenu);
    void createHelpMenu(QMenu& helpMenu);
    void createEditorLayout(const EditorRegistry& editorRegistry);
    void createCentralStack();
    QWidget* createReadOnlyPlaceholderPage();
    void createWorkspaceActions();
    void resetCompositingLayout(bool persist = false);
    void updateEditActions();
    void updateWorkspaceActions();
    void updateFileActions();
    void updateExportAction();
    void updateWindowTitle();
    void updateContentSurface();
    void updateCompositionActions();
    void toggleFullScreen();
    void showProjectColorSettings();

    CompositionSession& compositionSession_;
    ProjectHost& projectHost_;
    FrameExportController& frameExportController_;
    // Borrowed, may be null; the command itself is owned by the application composition root.
    RamPreviewController* ramPreview_ = nullptr;
    // Borrowed, may be null; owned by the application composition root, and read only by the
    // window status bar's preview cells.
    CompositionPreviewController* previewController_ = nullptr;
    PlaybackController* playbackController_ = nullptr;
    // Borrowed, may be null; owned by the application composition root.
    runtime::OperationCache* operationCache_ = nullptr;
    // Borrowed, may be null; owned by the application composition root.
    media::cache::MediaDiskCache* mediaDiskCache_ = nullptr;
    QMenuBar* menuBar_ = nullptr;
    QMenu* windowMenu_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    QMenu* compositionMenu_ = nullptr;
    QStackedWidget* centralStack_ = nullptr;
    // A kit strip under the workspace, NOT QMainWindow::statusBar(): that brings its own chrome,
    // size grip and item model, none of which the Kinetik language wants.
    WindowStatusBar* statusStrip_ = nullptr;
    WorkspaceHost* workspaceHost_ = nullptr;
    QWidget* readOnlyPlaceholderPage_ = nullptr;
    QLabel* readOnlyPlaceholderFileNameLabel_ = nullptr;
    QLabel* readOnlyPlaceholderBodyLabel_ = nullptr;
    QAction* newProjectAction_ = nullptr;
    QAction* openProjectAction_ = nullptr;
    QAction* saveProjectAction_ = nullptr;
    QAction* saveProjectAsAction_ = nullptr;
    QAction* saveProjectCopyAction_ = nullptr;
    QAction* projectColorSettingsAction_ = nullptr;
    QAction* exportFrameAction_ = nullptr;
    // Task S5, item 3a: the frame-range export, and the cancel a long sequence needs -- the single
    // frame export never had one because it is one attempt plus one publish.
    QAction* exportFrameRangeAction_ = nullptr;
    QAction* exportCompositionAction_ = nullptr;
    QAction* cancelFrameExportAction_ = nullptr;
    QAction* quitAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* splitLeftRightAction_ = nullptr;
    QAction* splitTopBottomAction_ = nullptr;
    QAction* closeAreaAction_ = nullptr;
    QAction* maximizeAreaAction_ = nullptr;
    QAction* ramPreviewAction_ = nullptr;
    QAction* cancelRamPreviewAction_ = nullptr;
    QAction* newCompositionAction_ = nullptr;
    QAction* duplicateCompositionAction_ = nullptr;
    QAction* renameCompositionAction_ = nullptr;
    QAction* deleteCompositionAction_ = nullptr;
    QAction* viewFullScreenAction_ = nullptr;
    QAction* viewMaximizePanelAction_ = nullptr;
    QAction* viewAudioEnabledAction_ = nullptr;
    QAction* reportIssueAction_ = nullptr;
    QAction* openSourceLicensesAction_ = nullptr;
    QAction* clearMediaDiskCacheAction_ = nullptr;
    bool workspaceLayoutWritable_ = true;
    bool shutdownRequested_ = false;
};

} // namespace bloom::ui
