#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QWidget>

class QAbstractItemModel;
class QFrame;
class QListView;

namespace bloom::ui::kit {

// The raised surface a KDropdown opens. A top-level Qt::Popup with a translucent background and a
// margin the elevation's blur fits into, holding an inner frame that carries the drop shadow, the
// SurfaceRaised fill, the Radius::Medium corners, and the item list.
//
// The translucent-outer / opaque-inner split is what lets a real drop shadow exist at all: a
// graphics effect cannot paint outside its widget, so the shadow needs margin to live in.
class KDropdownPopup final : public QWidget {
    Q_OBJECT

  public:
    explicit KDropdownPopup(QWidget* parent = nullptr);

    void setModel(QAbstractItemModel* model);
    [[nodiscard]] QListView* view() const noexcept;

    // Opens the popup directly below `anchor`, matching its width, with the Pop motion's 4-pixel
    // rise. Under reduced motion it appears at its final position with no animation at all.
    void openBelow(const QWidget& anchor, int currentIndex);

  Q_SIGNALS:
    // Emitted only for an item the artist may actually choose; a disabled row swallows the click.
    void itemChosen(int index);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    QFrame* surface_ = nullptr;
    QListView* view_ = nullptr;
};

} // namespace bloom::ui::kit
