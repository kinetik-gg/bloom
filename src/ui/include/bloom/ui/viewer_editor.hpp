#pragma once

#include <bloom/ui/composition_session.hpp>

#include <QCursor>
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

namespace bloom::ui::kit {
class KDropdown;
}

class QContextMenuEvent;
class QKeyEvent;
class QMouseEvent;
class QPainter;
class QResizeEvent;
class QWheelEvent;

namespace bloom::ui {

class CompositionPreviewController;

[[nodiscard]] QRectF fitDisplayRect(const QRectF& available, render::ImageExtent extent,
                                    core::PixelAspectRatio pixelAspect) noexcept;

// The un-zoomed, un-panned rectangle "100%" means: exactly one screen pixel per composition pixel
// (after pixel-aspect correction), centered in `available`. This is fitDisplayRect()'s sibling --
// fitDisplayRect scales content to FILL `available`; actualPixelRect never scales content at all.
// Both center on `available.center()`, so a transform with fitToWindow == false and zoom == 1.0
// reproduces exactly fitDisplayRect() only when the fitted content also happens to be shown at
// 100% -- the two are independent by design (task U3, issue #119, decision 2).
[[nodiscard]] QRectF actualPixelRect(const QRectF& available, render::ImageExtent extent,
                                     core::PixelAspectRatio pixelAspect) noexcept;

// The Viewer's zoom/pan state (task U3, issue #119, decision 2). `fitToWindow` selects
// fitDisplayRect()'s behavior (content scaled to fill `available`, recomputed every call -- there
// is no stored "fit zoom" value); when false, `zoom` (clamped to [kMinZoom, kMaxZoom]) scales
// actualPixelRect() about its own center and `pan` translates the result in screen pixels. `pan` is
// meaningless while `fitToWindow` is true and is always {0, 0} there by construction (see
// setFit()/materializeZoom() in viewer_editor.cpp).
struct ViewTransform final {
    static constexpr double kMinZoom = 1.0 / 16.0;
    static constexpr double kMaxZoom = 16.0;

    bool fitToWindow = true;
    double zoom = 1.0;
    QPointF pan{0.0, 0.0};

    friend bool operator==(const ViewTransform&, const ViewTransform&) = default;
};

// Composes `transform` onto `available`/`extent`/`pixelAspect`: fitDisplayRect() when
// transform.fitToWindow, otherwise actualPixelRect() scaled by transform.zoom (clamped) and
// translated by transform.pan. THE SEAM (decision 2): ViewerEditor::currentMapping() freezes
// exactly this rectangle -- the same freeze semantics the direct-manipulation contract has always
// had (docs/architecture/animation-and-time.md, "Direct Manipulation And Preview Overrides"), but
// the geometry it freezes now reflects the ACTIVE view transform at gesture begin instead of always
// being the fit-to-window rectangle. See viewer_editor.cpp's currentMapping() for the one call site
// that changed.
[[nodiscard]] QRectF viewTransformedDisplayRect(const QRectF& available, render::ImageExtent extent,
                                                core::PixelAspectRatio pixelAspect,
                                                const ViewTransform& transform) noexcept;

// Returns a new (fitToWindow == false) transform stepped by `factor` (>1 zooms in, <1 zooms out)
// such that the composition point under `screenPoint` -- expressed as `screenPoint`'s fractional
// position across the CURRENT viewTransformedDisplayRect() -- lands under `screenPoint` again after
// the step (the zoom-about-cursor invariant, pinned by tests). Degenerate geometry (empty
// `available`, zero-extent content) is a no-op that returns `transform` unchanged.
[[nodiscard]] ViewTransform zoomAboutPoint(const ViewTransform& transform, const QRectF& available,
                                          render::ImageExtent extent,
                                          core::PixelAspectRatio pixelAspect, QPointF screenPoint,
                                          double factor) noexcept;

class ViewerEditor final : public QWidget {
    Q_OBJECT

  public:
    ViewerEditor(CompositionSession& session, CompositionPreviewController& previewController,
                 QWidget* parent = nullptr);

    // Test/diagnostic surface only (never read by production code, mirroring kit::KDropdown's own
    // displayedText()/popupView() precedent): exposes state a test needs to assert on without
    // reaching into private members.
    [[nodiscard]] ViewTransform viewTransformForTest() const noexcept;
    [[nodiscard]] QString statusBarReadoutTextForTest() const;
    [[nodiscard]] QString statusBarColorChipTextForTest() const;
    [[nodiscard]] kit::KDropdown* zoomDropdownForTest() const noexcept;

  protected:
    void paintEvent(QPaintEvent* event) override;
    // Direct viewer manipulation of the selected layer's position (docs/architecture/
    // animation-and-time.md, "Direct Manipulation And Preview Overrides"; issue #82). press ->
    // beginPositionInteraction (+ beginInteractiveScrub() arming so drag previews ride Interactive
    // cadence); move -> updatePositionInteraction; release -> commit + disarm; Escape or a detected
    // resize/format/proxy/pixel-aspect/display-descriptor change -> cancel + disarm. A middle-button
    // press, or a left-button press while Space is held, begins a PAN gesture instead (decision 2)
    // and never touches CompositionSession.
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

  private:
    // The region paintEvent() draws the canvas into and currentMapping() maps gestures against:
    // the full widget rect minus the bottom status bar strip. There is no other inset -- the canvas
    // is full-bleed (decision 1).
    [[nodiscard]] QRectF canvasRect() const;
    [[nodiscard]] QRectF statusBarRect() const;
    void paintStatusBarSurface(QPainter& painter);
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

    struct DisplayGeometry final {
        render::ImageExtent extent;
        core::PixelAspectRatio pixelAspect;
    };
    // The current preview frame's extent/pixel aspect, or std::nullopt when there is nothing to
    // zoom/pan against (no composition, no frame yet). Shared by paintEvent(), currentMapping(),
    // and every zoom/pan gesture so they never disagree about what "the content" is.
    [[nodiscard]] std::optional<DisplayGeometry> currentDisplayGeometry() const;

    void setZoomFit();
    void setZoomActualSize();
    void setZoomPercent(int percent);
    void beginPan(Qt::MouseButton button, QPointF screenPoint, const DisplayGeometry& geometry);
    void updatePanCursor();
    void layoutStatusBar();
    void refreshZoomDropdown();

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    bool dragActive_ = false;
    QPointF dragOrigin_;
    std::optional<PositionInteractionMapping> activeMapping_;

    // Zoom/pan (decision 2).
    ViewTransform transform_;
    bool spaceHeld_ = false;
    bool panActive_ = false;
    Qt::MouseButton panButton_ = Qt::NoButton;
    QPointF panOrigin_;
    ViewTransform panBaseTransform_;

    // Bottom status bar (decision 3). Only the zoom control is a real child widget; the exact
    // frame/timecode readout and the color-state chip are painted directly (they update every
    // repaint from live session/preview state, so there is no separate text-cache to keep in sync).
    kit::KDropdown* zoomDropdown_ = nullptr;
};

} // namespace bloom::ui
