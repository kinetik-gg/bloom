#pragma once

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/viewer_overlays.hpp>

#include <QCursor>
#include <QImage>
#include <QMetaObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <cstdint>
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

class QAction;
class QLabel;
class QToolButton;

class QContextMenuEvent;
class QKeyEvent;
class QMenu;
class QMouseEvent;
class QResizeEvent;
class QWheelEvent;

namespace bloom::ui {

class CompositionPreviewController;
class PlaybackController;
class RamPreviewController;
class ViewerTimecodeReadout;

// Which channels of the display buffer the Viewer shows (task VIEW-1). This is a VIEWER-only
// remap applied while packing the frame for presentation: it never reaches an export, a cached
// frame, or any other surface, because the only thing it changes is the QImage paintEvent() blits.
//
//   Rgba  -- the delivered buffer, untouched (and uncopied: the zero-copy borrow paintEvent() has
//            always used).
//   Rgb   -- the same colour with alpha forced opaque, so a transparent region shows its colour
//            rather than the background behind it.
//   Red / Green / Blue -- that one channel as grey.
//   Alpha -- the alpha channel as luminance.
enum class ViewerChannel : std::uint8_t {
    Rgba,
    Rgb,
    Red,
    Green,
    Blue,
    Alpha,
};

// What the Viewer paints behind (and around) the composition (task VIEW-1). Persisted under
// "viewer/background"; Solid is the default.
//
// Solid is the application's own canvas Background token, NOT a per-composition colour: the
// document model carries no background colour for a composition today (document::CompositionFormat
// holds extent, pixel aspect, and frame rate and nothing else), so claiming one here would be an
// invented value. When the document gains one, Solid is the single place that has to start reading
// it.
enum class ViewerBackground : std::uint8_t {
    Solid,
    Checkerboard,
    Black,
    White,
};

// Widgets that consume Left/Right/Home/End for their own navigation set this dynamic property to
// true (task VIEW-1). The Viewer's four frame-stepping actions are Qt::WindowShortcut, so they
// would otherwise silently swallow that navigation window-wide; ViewerEditor disables them while
// focus rests on (or inside) a widget carrying this marker, which is exactly the rule
// TimelineEditor used to implement against its own layer stack before the transport moved here. A
// disabled QAction never claims ShortcutOverride, so the key reaches the focused widget unchanged.
//
// A dynamic property rather than a direct type check because the Viewer must not know which panels
// exist; anything that needs the arrow keys opts in by name.
inline constexpr char kDefersTransportKeysProperty[] = "bloomDefersTransportKeys";

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
// One wheel notch, or one Zoom In/Out menu action: the current effective zoom multiplied by this
// factor and clamped into [ViewTransform::kMinZoom, ViewTransform::kMaxZoom]. Published here rather
// than kept file-local to viewer_editor.cpp (task U4, issue #123, decision 1) because the Nodes
// canvas must step by exactly the same amount for an artist to feel one application; sharing the
// constant makes that structurally true instead of a number two files happen to agree on.
inline constexpr double kZoomStepFactor = 1.1;

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

// can host its bottom status bar (zoom control, exact frame/timecode readout, and the

// instead of a strip painted inside ViewerEditor's own canvas. Until something actually calls

// rebuildEditor() -- the status bar stays exactly where every existing test already expects it:
// painted inside ViewerEditor's own bottom Size::Control strip, with canvasRect() reserving that
// same space it always has. Every geometry-sensitive test that constructs a ViewerEditor directly
// (viewer_editor_tests.cpp, direct_manipulation_tests.cpp, composition_session_position_
// interaction_tests.cpp) never calls it, so their pinned canvasRect()-derived math is completely
// unaffected by this amendment.
class ViewerEditor final : public QWidget, public EditorChromeProvider {
    Q_OBJECT

  public:
    [[nodiscard]] EditorChromeSpec& editorChrome() override { return chrome_; }
    // `ramPreview` is the RAM Preview command (task PERF1, item 3), shared with the Composition
    // menu so both entry points call one method. Null leaves the footer's RAM Preview button
    // present and disabled -- an affordance that is visibly unavailable rather than one that
    // silently does nothing. It moved here with the rest of the transport (task VIEW-1).
    ViewerEditor(CompositionSession& session, CompositionPreviewController& previewController,
                 RamPreviewController* ramPreview = nullptr, QWidget* parent = nullptr);
    // Drops the application-wide focusChanged subscription BEFORE Qt starts deleting this panel's
    // children, for exactly the reason TimelineEditor's own destructor documents: a child losing
    // focus re-enters that subscription, which reads sibling widgets deleteChildren() may already
    // have destroyed.
    ~ViewerEditor() override;

    // them immediately after the panel switcher in the shared editor header.

    // widget away from this ViewerEditor and returns it -- the caller (EditorArea) takes ownership
    // from there. canvasRect() becomes full-bleed from that point on, since the bottom strip it
    // used to reserve now belongs to the caller's own footer slot instead. The color-state chip
    // keeps rendering inside the returned widget exactly as before (contract, FORMAL AMENDMENT 1).
    // A second call (this ViewerEditor already gave its footer away) returns nullptr.

    // Test/diagnostic surface only (never read by production code, mirroring kit::KDropdown's own
    // displayedText()/popupView() precedent): exposes state a test needs to assert on without
    // reaching into private members.
    [[nodiscard]] ViewTransform viewTransformForTest() const noexcept;
    [[nodiscard]] QString statusBarReadoutTextForTest() const;
    [[nodiscard]] kit::KDropdown* zoomDropdownForTest() const noexcept;
    // Task VIEW-1's own seams, on the same terms as the four above.
    [[nodiscard]] ViewerChannel channelForTest() const noexcept;
    [[nodiscard]] ViewerBackground backgroundForTest() const noexcept;
    [[nodiscard]] QString timeReadoutTextForTest() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;
    // Direct viewer manipulation of the selected layer's position (docs/architecture/
    // animation-and-time.md, "Direct Manipulation And Preview Overrides"; issue #82). press ->
    // beginPositionInteraction (+ beginInteractiveScrub() arming so drag previews ride Interactive
    // cadence); move -> updatePositionInteraction; release -> commit + disarm; Escape or a detected
    // resize/format/proxy/pixel-aspect/display-descriptor change -> cancel + disarm. A
    // middle-button press begins a PAN gesture instead
    // (decision 2) and never touches CompositionSession.
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

  private:
    EditorChromeSpec chrome_;
    // The region paintEvent() draws the canvas into and currentMapping() maps gestures against:

    // already relocated that strip to an external footer slot (FORMAL AMENDMENT 1), in which case
    // the canvas is full-bleed with no inset at all. There is no other inset either way
    // (decision 1).
    [[nodiscard]] QRectF canvasRect() const;

    // describe no longer belongs to this widget's own geometry.
    [[nodiscard]] QRectF statusBarRect() const;
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
    // The composition's full extent/pixel aspect, independent of the delivered proxy size.
    // Absent only when there is no composition. Shared by paintEvent(), currentMapping(),
    // and every zoom/pan gesture so they never disagree about what "the content" is.
    [[nodiscard]] std::optional<DisplayGeometry> currentDisplayGeometry() const;

    void setZoomFit();
    void setZoomActualSize();
    void setZoomPercent(int percent);
    void buildFooter(RamPreviewController* ramPreview);
    void wireTransport();
    // Pauses playback first, then writes the exact mapped time for `frameIndex`. The one
    // path every transport landing goes through -- the step actions, and the readout's own
    // click-to-edit commit.
    void seekToFrame(std::uint64_t frameIndex);
    void setChannel(ViewerChannel channel);
    void setBackground(ViewerBackground background);
    void buildHeader();
    void rebuildCompositionSelector();
    void rebuildObjectSelector();
    void updateCompositionActions();
    void selectAllObjects();
    void selectNoObjects();
    void invertObjectSelection();
    void zoomInAtCenter();
    void zoomOutAtCenter();
    void applySafeAreaPreset(ViewerSafeAreaPreset preset);
    void showCustomSafeAreaDialog();
    void updateOverlayActions();
    [[nodiscard]] double effectiveZoom(const QRectF& displayRect,
                                       const DisplayGeometry& geometry) const;
    // Reflects PlaybackController::stateChanged() onto the toggle button's text/tooltip/checked
    // state (issue #105, design decision 4: "button/icon state reflects transport state via a
    // signal"), unchanged except for which panel hosts the button.
    void updatePlaybackButton(PlaybackState state);
    void updateRamPreviewButton();
    void updateLoopButton();
    // Frame stepping (issue #108, decisions 1/2): Left/Right step one frame back/forward from
    // nearestFrameIndex(currentTime()), clamped to [0, maxFrameIndex]; delta is -1 or +1. Home/End
    // (stepToStart()/stepToEnd()) jump to frame 0 / the last frame. Every landing goes through the
    // exact mapped frame time via CompositionSession::setCurrentTime(), and pauses playback FIRST
    // through PlaybackController's own public pause() -- never by racing its tick().
    void stepFrame(int delta);
    void stepToStart();
    void stepToEnd();
    void beginPan(Qt::MouseButton button, QPointF screenPoint, const DisplayGeometry& geometry);
    void updatePanCursor();
    void layoutStatusBar();
    void refreshZoomDropdown();
    void updatePreviewResolution();

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    bool dragActive_ = false;
    QPointF dragOrigin_;
    std::optional<PositionInteractionMapping> activeMapping_;

    // Zoom/pan (decision 2).
    ViewTransform transform_;
    bool panActive_ = false;
    Qt::MouseButton panButton_ = Qt::NoButton;
    QPointF panOrigin_;
    ViewTransform panBaseTransform_;

    // The footer's controls (decision 3, reshaped by task VIEW-1). Every one of them is a child of
    // footer_ from construction, never of this ViewerEditor: the pre-VIEW-1 arrangement -- controls
    // parented here and the strip painted into this widget's own bottom inset until

    // reparenting exactly one widget.
    kit::KDropdown* zoomDropdown_ = nullptr;
    kit::KDropdown* resolutionDropdown_ = nullptr;
    // Task VIEW-1's footer, left to right: channel, zoom, resolution, background, the transport,
    // and the frame/timecode readout. Every one of them is a child of footer_ from construction --

    // hands the whole row over by reparenting exactly one widget.
    kit::KDropdown* channelDropdown_ = nullptr;
    kit::KDropdown* backgroundDropdown_ = nullptr;
    // Part of the Resolution control rather than a footer item of its own: what the chosen
    // policy actually resolved to ("Auto · ¼"), which for Auto is visible nowhere else.
    QLabel* resolutionReadout_ = nullptr;
    QToolButton* stepToStartButton_ = nullptr;
    QToolButton* stepBackButton_ = nullptr;
    QToolButton* playPauseButton_ = nullptr;
    QToolButton* stepForwardButton_ = nullptr;
    QToolButton* stepToEndButton_ = nullptr;
    // A real toggle now, not the status glyph the timeline used to show: PlaybackController has a
    // setLooping() command behind it (task VIEW-1), so a clickable control is honest here.
    QToolButton* loopButton_ = nullptr;
    // RAM Preview (task PERF1, item 3): folded into the play button's cached state -- the play
    // button reports whether the range it is about to play is cached -- while the explicit action
    // stays its own button, because caching a range and starting playback are different commands.
    QToolButton* ramPreviewButton_ = nullptr;
    ViewerTimecodeReadout* timeReadout_ = nullptr;
    // Borrowed from the preview controller; every panel drives the same transport.
    PlaybackController* playback_ = nullptr;
    // Borrowed: the RAM Preview command is application-wide (the Composition menu reaches the same
    // one), so this panel never owns it. Null when none was attached.
    RamPreviewController* ramPreview_ = nullptr;
    QAction* stepBackwardAction_ = nullptr;
    QAction* stepForwardAction_ = nullptr;
    QAction* stepToStartAction_ = nullptr;
    QAction* stepToEndAction_ = nullptr;
    QMetaObject::Connection focusConnection_;
    ViewerChannel channel_ = ViewerChannel::Rgba;
    ViewerBackground background_ = ViewerBackground::Solid;
    // The channel remap's one cached result. Keyed on the FRAME HANDLE (held by value, so the
    // bytes it was built from cannot be freed and a later frame cannot reuse the address) plus the
    // channel, so a repaint at an unchanged channel and frame costs nothing and playback does not
    // re-walk the buffer every tick. Empty whenever channel_ is Rgba, which is also the only case
    // paintEvent() keeps its original zero-copy borrow for.
    PreparedPreviewFrameHandle channelViewFrame_;
    ViewerChannel channelViewChannel_ = ViewerChannel::Rgba;
    QImage channelView_;
    // FORMAL AMENDMENT 1, as of task VIEW-1: non-null for this ViewerEditor's whole life. Until

    // widget's own bottom strip; after it, the caller owns it and this stays the surviving
    // reference the signal handlers repaint through.
    QWidget* statusBarFooter_ = nullptr;
    bool statusBarFooterTaken_ = false;

    QWidget* headerMenuWidget_ = nullptr;
    kit::KDropdown* compositionSelector_ = nullptr;
    kit::KDropdown* objectSelector_ = nullptr;
    QToolButton* compositionMenuButton_ = nullptr;
    QToolButton* fullscreenButton_ = nullptr;
    QMenu* viewerViewMenu_ = nullptr;
    QMenu* viewerSelectMenu_ = nullptr;
    QAction* viewerFitAction_ = nullptr;
    QAction* viewerActualSizeAction_ = nullptr;
    QAction* viewerZoomInAction_ = nullptr;
    QAction* viewerZoomOutAction_ = nullptr;
    QAction* viewerCompositionNewAction_ = nullptr;
    QAction* viewerCompositionDuplicateAction_ = nullptr;
    QAction* viewerCompositionDeleteAction_ = nullptr;
    QAction* viewerCompositionRenameAction_ = nullptr;
    QAction* safeAreasAction_ = nullptr;
    QAction* centreCrossAction_ = nullptr;
    QAction* thirdsAction_ = nullptr;
    QAction* rulersAction_ = nullptr;
    QAction* pixelGridAction_ = nullptr;
    QMenu* safeAreaPresetMenu_ = nullptr;
    std::array<QAction*, 5> safeAreaPresetActions_{};
    ViewerOverlayOptions overlayOptions_{};
};

} // namespace bloom::ui
