#pragma once

#include <QMainWindow>

class QAction;
class QMenu;
class QSettings;

namespace bloom::ui {

class EditorRegistry;
class WorkspaceHost;
enum class WorkspaceLayoutRestoreResult;

class MainWindow final : public QMainWindow {
  public:
    explicit MainWindow(const EditorRegistry& editorRegistry, QWidget* parent = nullptr);

    [[nodiscard]] WorkspaceHost* workspaceHost() const noexcept;
    [[nodiscard]] WorkspaceLayoutRestoreResult restoreApplicationState(QSettings& settings);
    void saveApplicationState(QSettings& settings) const;

  private:
    void createMenus();
    void createWorkspaceSwitcher();
    void createEditorLayout(const EditorRegistry& editorRegistry);
    void createWorkspaceActions();
    void resetCompositingLayout();
    void updateWorkspaceActions();
    void applyFoundationTheme();

    QMenu* windowMenu_ = nullptr;
    WorkspaceHost* workspaceHost_ = nullptr;
    QAction* splitLeftRightAction_ = nullptr;
    QAction* splitTopBottomAction_ = nullptr;
    QAction* closeAreaAction_ = nullptr;
    QAction* maximizeAreaAction_ = nullptr;
    bool workspaceLayoutWritable_ = true;
};

} // namespace bloom::ui
