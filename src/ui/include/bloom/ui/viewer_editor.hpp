#pragma once

#include <bloom/ui/composition_session.hpp>

#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <optional>

namespace bloom::core {
class PixelAspectRatio;
}

namespace bloom::render {
class ImageExtent;
}

class QKeyEvent;
class QMouseEvent;
class QResizeEvent;

namespace bloom::ui {

class CompositionPreviewController;

[[nodiscard]] QRectF fitDisplayRect(const QRectF& available, render::ImageExtent extent,
                                    core::PixelAspectRatio pixelAspect) noexcept;

class ViewerEditor final : public QWidget {
    Q_OBJECT

  public:
    ViewerEditor(CompositionSession& session, CompositionPreviewController& previewController,
                 QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;
    // Direct viewer manipulation of the selected layer's position (docs/architecture/
    // animation-and-time.md, "Direct Manipulation And Preview Overrides"; issue #82). press ->
    // beginPositionInteraction (+ beginInteractiveScrub() arming so drag previews ride Interactive
    // cadence); move -> updatePositionInteraction; release -> commit + disarm; Escape or a detected
    // resize/format/proxy/pixel-aspect/display-descriptor change -> cancel + disarm.
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void updatePreviewAccessibility();
    // Recomputes the mapping context (fitted display rectangle, composition format, proxy
    // resolution, pixel aspect, display descriptor) from the currently displayed preview frame and
    // this widget's current geometry. Returns std::nullopt when there is no current-composition
    // frame to map from -- a stale frame from another composition (or an older revision) is never
    // a mapping source.
    [[nodiscard]] std::optional<PositionInteractionMapping> currentMapping() const;
    // True while a gesture is active AND the freshly recomputed mapping still matches the one
    // frozen at gesture begin.
    [[nodiscard]] bool mappingStillValid() const;
    void endDrag(bool commit);

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    bool dragActive_ = false;
    QPointF dragOrigin_;
    std::optional<PositionInteractionMapping> activeMapping_;
};

} // namespace bloom::ui
