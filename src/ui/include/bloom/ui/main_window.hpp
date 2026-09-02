#pragma once

#include <QMainWindow>

class QAction;
class QCloseEvent;
class QLabel;
class QMenu;
class QSettings;
class QStackedWidget;

namespace bloom::ui {

class CompositionSession;
class EditorRegistry;
class FrameExportController;
class ProjectHost;
class WorkspaceHost;
enum class WorkspaceLayoutRestoreResult;

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    MainWindow(const EditorRegistry& editorRegistry, CompositionSession& compositionSession,
               ProjectHost& projectHost, FrameExportController& frameExportController,
               QWidget* parent = nullptr);

    [[nodiscard]] WorkspaceHost* workspaceHost() const noexcept;
    [[nodiscard]] WorkspaceLayoutRestoreResult restoreApplicationState(QSettings& settings);
    void saveApplicationState(QSettings& settings) const;
    // Presentation-level read-only surface (task R1, issue #74): true exactly when the central
    // QStackedWidget's current page is the read-only placeholder instead of the editor workspace
    // -- i.e. the live ProjectHost content kind is PreservedReadOnly. Exposed so an offscreen test
    // can assert the switch without depending on QWidget::isVisible(), which only reports
    // correctly once the top-level window itself has been shown.
    [[nodiscard]] bool isShowingReadOnlyPlaceholder() const noexcept;

  signals:
    void shutdownRequested();

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void createMenus();
    void createFileMenu(QMenu& fileMenu);
    void createWorkspaceSwitcher();
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

    CompositionSession& compositionSession_;
    ProjectHost& projectHost_;
    FrameExportController& frameExportController_;
    QMenu* windowMenu_ = nullptr;
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
    bool workspaceLayoutWritable_ = true;
    bool shutdownRequested_ = false;
};

} // namespace bloom::ui
