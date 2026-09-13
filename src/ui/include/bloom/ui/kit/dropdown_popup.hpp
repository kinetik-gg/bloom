#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QWidget>

class QAbstractItemModel;
class QFrame;
class QListView;

namespace bloom::ui::kit {

// The raised surface a KDropdown opens. A top-level Qt::Popup with a translucent background and a
// margin the elevation's blur fits into, holding an inner frame that carries the drop shadow, the
// SurfaceRaised fill, the Radius::Small corners (task U8, issue #131, fix 3), and the item list.
//
// The translucent-outer / opaque-inner split is what lets a real drop shadow exist at all: a
// graphics effect cannot paint outside its widget, so the shadow needs margin to live in.
//
// Detached (task F1, item F3): the popup stays open until an item is chosen, a click lands outside
// the frame, or Esc is pressed. Moving the pointer off it does nothing at all -- there is no
// close-on-leave anywhere in this class, and leaveEvent() exists precisely to say so.
//
// The FRAME is the rounded container, never the rows. Rows are rectangular and full width, and the
// accent hover bar the first and last of them paint is clipped by the frame's own rounded corners
// (the list is masked to the frame's inner radius) rather than each row rounding itself.
class KDropdownPopup final : public QWidget {
    Q_OBJECT

  public:
    explicit KDropdownPopup(QWidget* parent = nullptr);

    void setModel(QAbstractItemModel* model);
    [[nodiscard]] QListView* view() const noexcept;

    // The rounded, bordered SurfaceRaised container. Everything the artist sees is inside it; the
    // widget around it is only the transparent gutter the drop shadow lives in.
    [[nodiscard]] QFrame* surface() const noexcept;

    // Opens the popup directly below `anchor`, matching its width, with the Pop motion's 4-pixel
    // rise. Under reduced motion it appears at its final position with no animation at all.
    void openBelow(const QWidget& anchor, int currentIndex);

  Q_SIGNALS:
    // Emitted only for an item the artist may actually choose; a disabled row swallows the click.
    void itemChosen(int index);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    // Clips the item list to the frame's inner rounded rectangle, so a full-width accent hover bar
    // on the first or last row cannot square off the frame's corners.
    void applyRoundedListMask();

    QFrame* surface_ = nullptr;
    QListView* view_ = nullptr;
};

} // namespace bloom::ui::kit
