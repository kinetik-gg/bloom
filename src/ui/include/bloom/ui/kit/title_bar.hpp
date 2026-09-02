#pragma once

#include <QWidget>

class QHBoxLayout;
class QLabel;
class QMenuBar;
class QMouseEvent;

namespace bloom::ui::kit {

class KButton;

// The Kinetik application title bar (task U2, issue #118): a Size::TitleBar-tall row carrying the
// app title (Type::Title) on the left, an optional embedded QMenuBar (custom chrome shares the
// SAME QMenuBar instance the native-chrome path would otherwise dock classically -- see
// MainWindow), and minimize/maximize-or-restore/close ghost buttons on the right.
//
// Drag-to-move and double-click-to-maximize are handled here, on the title bar's own empty area:
// Qt delivers mouse events to this widget only where no child (the label, the menu bar, a button)
// already claimed them, so no manual hit-testing against those children is needed.
class TitleBar final : public QWidget {
    Q_OBJECT

  public:
    explicit TitleBar(QWidget* parent = nullptr);

    void setTitle(const QString& title);

    // Reparents `menuBar` into the title bar row, right after the title label. Ownership follows
    // Qt's normal parent-child rule -- the caller must not also parent it elsewhere. Native chrome
    // never calls this; it keeps the QMenuBar docked classically via QMainWindow::menuBar().
    void setMenuBar(QMenuBar* menuBar);

    // Swaps the maximize button between its Maximize and Restore glyph/tooltip -- the window owner
    // (MainWindow) calls this from QWidget::changeEvent(QEvent::WindowStateChange) so every path to
    // a state change (button, double-click, OS action, keyboard) stays in sync, mirroring
    // EditorArea::setMaximizedAppearance()'s precedent for the same problem at panel scope.
    void setMaximized(bool maximized);
    // Named to avoid shadowing QWidget::isMaximized() (a real window-manager state query this
    // widget has no opinion on) -- this is only ever the last value setMaximized() was given.
    [[nodiscard]] bool maximizedAppearance() const noexcept;

  Q_SIGNALS:
    void minimizeRequested();
    void maximizeOrRestoreRequested();
    void closeRequested();

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

  private:
    QLabel* titleLabel_ = nullptr;
    QHBoxLayout* layout_ = nullptr;
    int menuBarSlot_ = 0;
    QMenuBar* menuBar_ = nullptr;
    KButton* minimizeButton_ = nullptr;
    KButton* maximizeButton_ = nullptr;
    KButton* closeButton_ = nullptr;
    bool maximized_ = false;
};

} // namespace bloom::ui::kit
