#pragma once

#include <QObject>

class QMainWindow;

namespace bloom::ui {

// Edge-resize for a frameless, custom-chrome top-level window (task U2, issue #118, decision 1).
// A frameless QMainWindow has no OS-owned border to grab, so this installs an application-wide
// event filter (the only way to see a mouse press near the window's own edge before whichever
// child widget sits there -- WorkspaceHost's Background gutter, the TitleBar itself -- claims it)
// and, for a left-button press that lands within `marginPx` of `window`'s own rectangle, calls
// QWindow::startSystemResize() for the nearest edge/corner. Native chrome never constructs one:
// the platform's own border already does this.
//
// Wayland-first correctness (issue #118's target session), X11 also supported: both compositors
// implement QWindow::startSystemResize() through the same Qt platform-integration contract this
// class calls through, so no platform-specific code path is needed here.
class FramelessEdgeResizer final : public QObject {
    Q_OBJECT

  public:
    FramelessEdgeResizer(QMainWindow& window, int marginPx, QObject* parent = nullptr);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    QMainWindow& window_;
    int marginPx_;
};

} // namespace bloom::ui
