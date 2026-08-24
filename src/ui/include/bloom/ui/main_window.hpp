#pragma once

#include <QMainWindow>

class QAction;
class QCloseEvent;
class QMenu;
class QSettings;

namespace bloom::ui {

class CompositionSession;
class EditorRegistry;
class WorkspaceHost;
enum class WorkspaceLayoutRestoreResult;

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    MainWindow(const EditorRegistry& editorRegistry, CompositionSession& compositionSession,
               QWidget* parent = nullptr);

    [[nodiscard]] WorkspaceHost* workspaceHost() const noexcept;
    [[nodiscard]] WorkspaceLayoutRestoreResult restoreApplicationState(QSettings& settings);
    void saveApplicationState(QSettings& settings) const;

  signals:
    void shutdownRequested();

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void createMenus();
    void createWorkspaceSwitcher();
    void createEditorLayout(const EditorRegistry& editorRegistry);
    void createWorkspaceActions();
    void resetCompositingLayout();
    void updateEditActions();
    void updateWorkspaceActions();
    void applyFoundationTheme();

    CompositionSession& compositionSession_;
    QMenu* windowMenu_ = nullptr;
    WorkspaceHost* workspaceHost_ = nullptr;
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
