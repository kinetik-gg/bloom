#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QMargins>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QWidget>

class QVariantAnimation;

namespace bloom::ui::kit {

// Whether a top-level popup's transparent pixels are really transparent on this platform: true on
// Wayland, Windows and macOS, whose compositors always blend per pixel, and offscreen; false on X11
// (a compositing manager is optional there) and on any platform not known to composite. Where it is
// false a popup has no shadow gutter and square corners -- a flat, hairline-bordered surface --
// because a gutter that is not transparent reads as a dark band under the last row.
[[nodiscard]] bool popupTranslucencySupported();

// The transparent margin a popup of `elevation` reserves around its frame for the shadow: the
// token's blur on every side, shifted by its offset. Empty where popupTranslucencySupported() is
// false, or for Elevation::Flat.
[[nodiscard]] QMargins popupGutter(Elevation elevation);

// The visual rectangle of `anchor` in global coordinates -- through a QGraphicsProxyWidget and the
// zoom of the view showing it, so a dropdown on a zoomed-out node card anchors to what is drawn,
// not to its unscaled widget size.
[[nodiscard]] QRect visualGlobalRect(const QWidget& anchor);

// Where a popup's frame lands, computed without a window so a test can place it against fake
// screens. The frame opens below the anchor, `gap` away; it flips above when it does not fit below
// and there is more room above; its height shrinks to whichever side it takes; and x is clamped so
// the frame stays on the anchor's screen. `leadingShift` moves the frame that far left of the
// anchor's left edge (a list popup hangs its check column out so row text lines up with the
// closed field's text).
struct PopupPlacementRequest final {
    QRect anchor;
    QSize frame;
    QRect screen;
    int gap = 0;
    int leadingShift = 0;
};

struct PopupPlacement final {
    QRect frame;
    bool above = false;
};

[[nodiscard]] PopupPlacement placePopup(const PopupPlacementRequest& request);

// The one popup window. A top-level Qt::Popup (bypassing any graphics proxy, so a popup opened from
// a node card is a real window above the scene) that paints its own SurfaceRaised frame -- one
// Border hairline, Radius::Small -- and the elevation's shadow from the cached nine-patch in a
// transparent gutter around it. The content widget fills the frame inside its hairline.
//
// Detached: it stays open until the content closes it, a press lands outside the frame, or Esc is
// pressed; the pointer leaving it does nothing. A press on the anchor that opened it closes it
// without reaching the anchor (WA_NoMouseReplay), so clicking a dropdown's field closes its popup
// instead of reopening it. A press in the shadow gutter closes it and is passed on to whatever lies
// under the pointer, exactly as a press just outside the gutter would be on that platform.
//
// The Pop entrance moves the frame inside the gutter -- never the window, whose position a
// Wayland compositor owns -- and is skipped under reduced motion.
class KPopupWindow : public QWidget {
    Q_OBJECT

  public:
    explicit KPopupWindow(QWidget* parent = nullptr, Elevation elevation = Elevation::Popup);

    // Reparents `content` into the frame. The widget the popup focuses on open is `focusTarget`, or
    // the content's first focusable descendant when none is given.
    void setContent(QWidget* content, QWidget* focusTarget = nullptr);
    [[nodiscard]] QWidget* content() const noexcept { return content_; }
    void setFocusTarget(QWidget* target) { focusTarget_ = target; }

    // The frame: the rounded surface the artist sees, in this window's coordinates.
    [[nodiscard]] QRect frameRect() const;
    [[nodiscard]] QMargins gutter() const noexcept { return gutter_; }
    [[nodiscard]] bool openedAbove() const noexcept { return above_; }
    // Whether this popup draws a shadow and rounded corners: the platform guarantees per-pixel
    // transparency and the window's own format really has an alpha channel.
    [[nodiscard]] bool translucent() const noexcept { return translucent_; }
    [[nodiscard]] QRect anchorRect() const noexcept { return anchorRect_; }

    // Opens with a frame of `frameSize` against `anchor` by placePopup(), on the anchor's screen.
    void openFor(const QWidget& anchor, QSize frameSize, int leadingShift = 0);
    // Opens with the frame's top-left at `globalPosition`, flipped and clamped onto its screen.
    void openAt(QPoint globalPosition, QSize frameSize);
    // Re-fits an open popup to a new frame size, keeping the edge that touches its anchor.
    void refit(QSize frameSize);

  Q_SIGNALS:
    void closed();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void open(const PopupPlacementRequest& request);
    void layoutContent();
    void focusContent();
    [[nodiscard]] qreal frameCorner() const;

    Elevation elevation_;
    QMargins gutter_;
    QWidget* content_ = nullptr;
    QPointer<QWidget> focusTarget_;
    QVariantAnimation* rise_ = nullptr;
    PopupPlacementRequest request_;
    QRect anchorRect_;
    int riseOffset_ = 0;
    bool above_ = false;
    // Set once the native window exists and its format is known: false when the surface Qt got
    // has no alpha channel, which makes this a flat popup whatever the platform promised.
    bool translucent_ = false;
    bool formatChecked_ = false;
};

} // namespace bloom::ui::kit
