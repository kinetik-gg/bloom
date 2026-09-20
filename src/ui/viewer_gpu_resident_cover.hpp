#pragma once

// Private native CPU cover for ViewerGpuResidentController. A plain QWidget
// sibling lives in the parent's backing store and is always occluded by an
// exposed native child, so this cover is forced native and raised above the
// container. It shows the last valid CPU image until a genuine owner present
// acknowledgement. It never takes focus or mouse/key/IME events, so input still
// reaches the container and is forwarded through the existing handlers.
//
// The pixels are displayed through the existing kit pixel-display control:
// KLabel subclasses QLabel and shows a QPixmap with no private painting, so the
// cover composes a kit control instead of hand-painting a canvas. It owns no
// geometry or color of its own -- zero margins, centred alignment and
// setScaledContents(true) -- so QLabel's own device-aware pixmap display scales
// the snapshot to the cover rect while honouring the pixmap's devicePixelRatio,
// which keeps the bounded CPU snapshot semantics identical to a direct drawPixmap.

#include <QPixmap>
#include <QWidget>

#include <bloom/ui/kit/controls.hpp>

namespace bloom::ui {

class ViewerGpuCpuCover final : public kit::KLabel {
  public:
    explicit ViewerGpuCpuCover(QWidget* parent) : kit::KLabel(QString{}, parent) {
        setAttribute(Qt::WA_NativeWindow, true);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setFocusPolicy(Qt::NoFocus);
        setScaledContents(true);
        setAlignment(Qt::AlignCenter);
        setContentsMargins(0, 0, 0, 0);
        // KLabel fixes its height to the kit Control size; a full-frame cover must instead take the
        // whole presented container rect. Clear the inherited fixed height and let the controller's
        // setGeometry drive both axes; the pixmap/DPR display path is unchanged.
        setMinimumHeight(0);
        setMaximumHeight(QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    }

    void setSnapshot(const QPixmap& snapshot) {
        // QLabel's device-aware display scales to its own rect while honouring the
        // pixmap's devicePixelRatio, exactly as the previous drawPixmap(rect(), snapshot) did.
        setPixmap(snapshot);
    }
};

} // namespace bloom::ui
