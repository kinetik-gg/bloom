#pragma once

#include <QWidget>

namespace bloom::ui::kit {

// task TL-FIX2. A draggable divider between two panes that share one row grid -- the timeline's
// layer table and its lanes today, the same precedent WorkspaceHost's QSplitter serves between
// separate EditorAreas. A real QSplitter does not fit here: the two panes are two widgets painting
// ONE logical row grid (see timeline_editor.cpp's own header comment), so their split has to be a
// single pixel value the owner keeps and re-applies to both halves, not two independently-sized
// splitter children.
//
// KSplitHandle owns only the GESTURE and its own painting: a hairline (TimelineSeparator-width)
// centered in a wider (Size::SplitHandle) hit zone, so it is easy to grab without widening the
// visible divider. It never touches layout or persistence itself -- the owner reads dragged() and
// resetRequested(), clamps and applies the result, and persists it.
class KSplitHandle final : public QWidget {
    Q_OBJECT

  public:
    explicit KSplitHandle(Qt::Orientation orientation, QWidget* parent = nullptr);

  Q_SIGNALS:
    // A press on the handle, before the first dragged() of a gesture.
    void dragStarted();
    // The cumulative offset (in the split's own axis) since dragStarted(), positive growing the
    // leading pane. The owner applies it to the width it captured at dragStarted(), rather than
    // this class tracking a running total itself.
    void dragged(int totalDelta);
    // The release that ends a drag gesture -- the owner's cue to persist the split it has already
    // been applying live through dragged().
    void dragFinished();
    // A double-click: the owner's cue to restore its own default split.
    void resetRequested();

  protected:
    [[nodiscard]] QSize sizeHint() const override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    Qt::Orientation orientation_;
    bool hovered_ = false;
    bool dragging_ = false;
    QPoint pressPosition_;
};

} // namespace bloom::ui::kit
