#pragma once

#include <QMainWindow>

namespace bloom::ui {

class EditorRegistry;

class MainWindow final : public QMainWindow {
  public:
    explicit MainWindow(const EditorRegistry& editorRegistry, QWidget* parent = nullptr);

  private:
    void createMenus();
    void createWorkspaceSwitcher();
    void createEditorLayout(const EditorRegistry& editorRegistry);
    void applyFoundationTheme();
};

} // namespace bloom::ui
