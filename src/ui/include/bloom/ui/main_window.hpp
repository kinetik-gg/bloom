#pragma once

#include <QMainWindow>

namespace bloom::ui {

class MainWindow final : public QMainWindow {
  public:
    explicit MainWindow(QWidget* parent = nullptr);

  private:
    void createMenus();
    void createWorkspaceSwitcher();
    void createEditorLayout();
    void applyFoundationTheme();
};

} // namespace bloom::ui
