#pragma once

#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_native_surface.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/preferences_aware.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/viewer_editor_probe.hpp>
#include <bloom/ui/viewer_gpu_resident.hpp>
#include <bloom/ui/viewer_overlays.hpp>

#include <QCursor>
#include <QImage>
#include <QLineF>
#include <QMetaObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace bloom::core {
class PixelAspectRatio;
}

namespace bloom::runtime {
class GpuPresentationClient;
}

namespace bloom::render {
class ImageExtent;
}

namespace bloom::ui::kit {
class KDropdown;
class KIconToggle;
class KValueField;
} // namespace bloom::ui::kit

class QAction;
class QLabel;
class QPixmap;
class QToolButton;
class QTimer;

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
// "viewer/background"; Checkerboard is the default so alpha behind the composition is always
// visible.
//
// Solid fills with the panel Canvas token, so the surround blends with the panel chrome rather
// than reading as a separate black rectangle; it does not read the composition's authored
// backgroundColor. Black and White are literal. It is viewer presentation only and never fills
// process or export alpha.
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

// True when a completed CPU-fallback frame answers the CURRENT resident request identity. A stale
// completion for a superseded identity is false and must be dropped, then the latest requeued.
[[nodiscard]] bool cpuFallbackCompletionIsCurrent(const runtime::PreviewRequestIdentity& completed,
                                                  const runtime::PreviewRequestIdentity& current);

// The display descriptor a viewer gesture maps against, from the CPU packed view or, for the
// GPU-resident arm, its lease geometry. Geometry only: no pixels are read, copied, or
// reconstructed. Nullopt when neither source yields a descriptor; at most one is expected.
[[nodiscard]] std::optional<render::ReferenceDisplayBufferDescriptor>
viewerDisplayDescriptorForFrame(std::optional<runtime::PreviewDisplayBufferView> cpuView,
                                std::optional<ResidentFrameGeometry> residentFrame) noexcept;

// The composition-frame border and the active composition's empty-state invitation, painted exactly
// as the CPU paint path draws them. The GPU-resident overlay recording is the only paint on the
// resident route, so it shares these: a resident composition still shows its bounds and an empty
// one still invites a layer. Kept as two functions (rather than one combined paint) because the CPU
// path draws the border beneath the guides and the invitation above everything; both are directly
// testable without a GPU device.
void paintViewerCompositionFrame(QPainter& painter, const QRectF& displayRect);
void paintViewerEmptyInvitation(QPainter& painter, const QRectF& canvasRect,
                                const QString& invitation);

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
struct ViewerTextEdit;

class ViewerEditor final : public QWidget,
                           public EditorChromeProvider,
                           public EditorNativeSurface,
                           public PreferencesAware {
    Q_OBJECT

  public:
    [[nodiscard]] EditorChromeSpec& editorChrome() override { return chrome_; }
    // Applies the Preferences window's committed viewer preferences by driving this panel's own
    // controls, so their existing persistence and preview-controller updates stay the single code
    // path.
    void applyApplicationPreferences(const ApplicationPreferences& preferences) override;
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
    [[nodiscard]] PreparedPreviewFrameHandle displayedFrameForTest() const {
        return displayedFrame();
    }
    [[nodiscard]] bool textEditing() const noexcept;
    [[nodiscard]] QLineF textCaretForTest() const;
    [[nodiscard]] QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    [[nodiscard]] ViewTransform viewTransformForTest() const noexcept;
    [[nodiscard]] QRectF canvasRectForTest() const { return canvasRect(); }
    [[nodiscard]] QRectF contentRectForTest() const { return contentRect(); }
    // The invitation the empty canvas paints ("Create a composition to begin" with no active
    // composition, "Create a layer to begin" for an active composition that has no layers, empty
    // once there is content).
    [[nodiscard]] QString emptyStateInvitationTextForTest() const { return emptyStateInvitation(); }
    [[nodiscard]] QString statusBarReadoutTextForTest() const;
    [[nodiscard]] kit::KDropdown* zoomDropdownForTest() const noexcept;
    // Task VIEW-1's own seams, on the same terms as the four above.
    [[nodiscard]] ViewerChannel channelForTest() const noexcept;
    [[nodiscard]] ViewerBackground backgroundForTest() const noexcept;
    [[nodiscard]] QString timeReadoutTextForTest() const;

    // --- GPU-resident presentation (prepared integration) --------------------------------------
    // Explicit dependency injection, callable by the later application integration. There is no
    // global locator; a null client (or a non-resident displayed frame) leaves the CPU paint path
    // completely unchanged. `scheduler` is used only to rasterize overlays off the UI thread.
    void setGpuPresentationDependencies(std::shared_ptr<runtime::GpuPresentationClient> client,
                                        runtime::TaskScheduler* scheduler,
                                        std::string vulkanLoaderPath, double devicePixelRatio);

    // EditorNativeSurface: the host's retire-before-mutation gate, forwarded to the adapter.
    [[nodiscard]] bool hasLiveNativeTarget() const override;
    [[nodiscard]] PrepareOutcome prepareNativeSurfaceMutation(std::uint64_t generation,
                                                              PrepareCallback completion) override;
    void resumeNativeSurfaceAfterMutation() override;
    [[nodiscard]] std::string nativeSurfaceDiagnostic() const override;

    // Test/diagnostic seams only (same terms as the accessors above).
    [[nodiscard]] bool residentPresentationActiveForTest() const noexcept;
    [[nodiscard]] bool gpuResidentConfiguredForTest() const noexcept;
    [[nodiscard]] std::size_t gpuPresentAttemptCountForTest() const noexcept;
    [[nodiscard]] std::size_t gpuPresentAcceptedCountForTest() const noexcept;
    // Genuine owner-observed native present progress (read-only; mailbox admission does not advance
    // it). A caller proving a specific request was presented waits until
    // gpuNativeAppliedSequenceForTest() >= gpuNativeLastEnqueuedSequenceForTest().
    [[nodiscard]] std::uint64_t gpuNativeAppliedSequenceForTest() const noexcept;
    [[nodiscard]] std::uint64_t gpuNativePresentCountForTest() const noexcept;
    [[nodiscard]] std::uint64_t gpuNativeLastEnqueuedSequenceForTest() const noexcept;
    // Read-only native cover/container state: the native CPU cover must never stay visible once a
    // genuine present is acknowledged (it would occlude the resident image and intercept input),
    // and the container must be the viewer's live native child. No behavior change.
    [[nodiscard]] bool gpuCpuCoverVisibleForTest() const noexcept;
    [[nodiscard]] QWidget* gpuNativeContainerForTest() const noexcept { return gpuContainer_; }
    [[nodiscard]] std::string gpuPresentationDiagnosticForTest() const;
    void pollGpuResidentForTest();
    // Renders the actual cover handoff pixmap for the current CPU content (test seam).
    [[nodiscard]] QPixmap renderCpuCoverSnapshotForTest();
    // Test-only: inject the adapter's private port seam so a CPU-only fixture can drive the
    // controller without a device. The product never calls this.
    void setGpuPresentationPortForTest(std::shared_ptr<ViewerGpuPort> port);
    // Test-only: simulate an internal or external retirement already in flight (no presenter
    // needed), settle the internal one, and read the external completion generation, so the
    // fold-in/refusal contract is testable without a device.
    void simulateNativeRetireInFlightForTest();
    void simulateExternalRetireInFlightForTest();
    void finishSimulatedNativeRetireForTest(bool safeToMutate, const std::string& diagnostic = {});
    // Drives the standalone external retirement completion (the queued body) with the given host
    // generation, so the unsafe-clear / safe-preserve / stale contract is testable without a
    // presenter.
    void finishSimulatedExternalRetireForTest(std::uint64_t hostGeneration, bool safeToMutate,
                                              PrepareCallback completion,
                                              const std::string& diagnostic = {});
    [[nodiscard]] std::uint64_t hostMutationGenerationForTest() const noexcept;
    // Test-only: the resident present request the viewer would submit for the current transform,
    // channel, and background. Device-free, so a test can prove the GPU surround matches the CPU
    // drawCanvasBackground() paint for every background mode.
    [[nodiscard]] ResidentPresentRequest buildResidentPresentRequestForTest();
    // Test-only: drive one already-translated native input event through the real dispatch path so
    // a test can prove it reaches receiver event filters (workspace panel activation) instead of
    // only the handler.
    void forwardGpuInputForTest(const ViewerGpuInputEvent& event) { forwardGpuInput(event); }

  signals:
    void probeChanged(ProbeReadout readout);

  protected:
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;
    // Direct viewer manipulation of the selected layer's position (docs/architecture/
    // animation-and-time.md, "Direct Manipulation And Preview Overrides"; issue #82). press ->
    // beginTransformInteraction (+ beginInteractiveScrub() arming so drag previews ride Interactive
    // cadence); move -> updateTransformInteraction; release -> commit + disarm; Escape or a
    // detected resize/format/proxy/pixel-aspect/display-descriptor change -> cancel + disarm. A
    // middle-button press begins a PAN gesture instead
    // (decision 2) and never touches CompositionSession.
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

  private:
    bool beginTextEditing(std::optional<QPointF> point = std::nullopt, bool selectAll = false);
    void finishTextEditing(bool commit);
    void refreshTextLayout();
    void publishTextEdit();
    void replaceTextSelection(const QString& replacement);
    bool textEditKey(QKeyEvent* event);
    bool textEditPress(QMouseEvent* event);
    void moveTextCaret(QPointF screenPoint, bool extend);
    [[nodiscard]] QTransform textToScreen() const;
    [[nodiscard]] std::size_t textCaretByte() const;
    void paintTextEditing(QPainter& painter) const;
    std::unique_ptr<ViewerTextEdit> textEdit_;
    EditorChromeSpec chrome_;
    // The region paintEvent() draws the canvas into and currentMapping() maps gestures against:

    // already relocated that strip to an external footer slot (FORMAL AMENDMENT 1), in which case
    // the canvas is full-bleed with no inset at all. There is no other inset either way
    // (decision 1).
    // The whole content area: right of the tool column, above the footer. The surround fills it.
    [[nodiscard]] QRectF contentRect() const;
    [[nodiscard]] QRectF canvasRect() const;
    // The empty-canvas invitation for the current session: composition wording when no composition
    // is active, layer wording when an active composition has no layers, empty once it has content.
    [[nodiscard]] QString emptyStateInvitation() const;

    // describe no longer belongs to this widget's own geometry.
    [[nodiscard]] QRectF statusBarRect() const;
    void updatePreviewAccessibility();
    // Recomputes the mapping context (fitted display rectangle, composition format, proxy
    // resolution, pixel aspect, display descriptor) from the currently displayed preview frame and
    // this widget's current geometry. Returns std::nullopt when there is no current-composition
    // frame to map from -- a stale frame from another composition (or an older revision) is never
    // a mapping source.
    [[nodiscard]] std::optional<ViewerMapping> currentMapping() const;
    // True while a gesture is active AND the freshly recomputed mapping still matches the one
    // frozen at gesture begin.
    [[nodiscard]] bool mappingStillValid() const;
    [[nodiscard]] ViewerHit hitAt(const ViewerMapping& mapping, QPointF point) const;
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

    // GPU-resident presentation plumbing (viewer_gpu_resident.cpp): presents the resident arm
    // through the adapter, records the overlays as an immutable QPicture for off-thread raster, and
    // forwards the native window input back through the real event handlers with the container
    // origin added exactly once.
    //
    // updateGpuResidentPresentation() is a re-entrancy-guarded dispatcher; the body is separate so
    // a controller callback fired from a present cannot re-enter the surface/geometry transition.
    void updateGpuResidentPresentation();
    void updateGpuResidentPresentationBody();
    void pollGpuResident();
    void requestResidentCpuFallback();
    void pollResidentCpuFallback();
    void forwardGpuInput(const ViewerGpuInputEvent& event);
    // Retire-before-unmap for a live target on the blank/no-frame transition: raise the host's
    // CURRENT CPU paint over the still-mapped container, request the adapter's async retire, and
    // only clear the container and reset the adapter after a genuine SafeToMutate. A Retained or
    // refused answer leaves the surface mapped and never fabricates safety.
    void requestResidentNativeRetire();
    // Completion of requestResidentNativeRetire(): drops the container handle BEFORE the presenter
    // reset and resolves folded-in host mutations; Retained keeps everything mapped. Never runs
    // inline from the presenter's retire stack.
    void onResidentNativeRetireResult(std::uint64_t generation, bool safeToMutate,
                                      const std::string& diagnostic);
    // Delivers the standalone external mutation completion (the queued body). On an unsafe result
    // it clears gpuHostMutationPending_ BEFORE invoking the host completion, because the host gate
    // never resumes an unsafe entry and the completion may destroy this editor or start another
    // generation. A safe result keeps the gate pending until resumeNativeSurfaceAfterMutation().
    void onExternalNativeRetireResult(std::uint64_t generation, std::uint64_t hostGeneration,
                                      bool safeToMutate, const std::string& diagnostic,
                                      const PrepareCallback& completion);
    // Resolves the single external mutation folded into an in-flight internal retirement; it is
    // delivered during the queued internal resolution, never on the presenter's own stack.
    void resolveGpuHostMutationWaiters(bool safeToMutate, const std::string& diagnostic);
    void clearGpuContainer();
    [[nodiscard]] bool residentFrameIsDisplayed() const;
    // The frame the CPU paint path should draw: the live frame for a CPU arm, the same-request CPU
    // fallback (or the last CPU frame) while a resident present is pending/failed, and nothing
    // while a resident present is genuinely active.
    [[nodiscard]] PreparedPreviewFrameHandle paintableCpuFrame();
    [[nodiscard]] ResidentPresentRequest buildResidentPresentRequest();
    // Renders the last valid CPU content into a bounded pixmap for the native CPU cover, with the
    // correct device pixel ratio and content origin, excluding native children/cover.
    [[nodiscard]] QPixmap renderCpuCoverSnapshot();
    // Paints the CPU image + viewer overlays (shared by paintEvent and the cover snapshot).
    void paintViewerContent(QPainter& painter);

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    bool dragActive_ = false;
    std::optional<ViewerMapping> activeMapping_;

    // Zoom/pan (decision 2).
    ViewTransform transform_;
    enum class Tool { Select, Hand, Zoom, Rectangle, Ellipse, Polygon, Star, Line, Pen, Text };
    void selectTool(Tool tool);
    struct CreationGesture {
        ViewerMapping mapping;
        document::Revision revision;
        core::RationalTime time;
        document::CompositionId composition;
        QPointF origin;
        QPointF pointer;
        Qt::KeyboardModifiers modifiers;
    };
    std::optional<CreationGesture> creation_;
    [[nodiscard]] bool creationValid() const;
    [[nodiscard]] document::ShapeKind creationKind() const;
    [[nodiscard]] commands::ShapeLayerGeometry creationGeometry() const;
    bool creationPress(QMouseEvent* event);
    bool textPress(QMouseEvent* event);
    bool creationMove(QMouseEvent* event);
    bool creationRelease(QMouseEvent* event);
    void cancelCreation();
    void paintCreation(QPainter& painter) const;
    bool roiPress(QMouseEvent* event);
    bool roiMove(QMouseEvent* event);
    bool roiRelease(QMouseEvent* event);
    void paintRoi(QPainter& painter) const;
    void publishRoi();
    [[nodiscard]] QString analysisSettingsPrefix() const;
    [[nodiscard]] QString displayViewSettingsKey() const;
    [[nodiscard]] QString lookSettingsKey() const;
    void rebuildDisplayViewControl();
    void updateLookControl();
    void loadViewAdjust();
    void refreshViewAdjustment();
    void consumeViewAdjustment();
    void refreshProbe(QPointF position);
    void clearProbe();
    void consumeProbe();
    std::optional<QPointF> probePosition_;
    ProbeReadout probeReadout_{};
    QTimer* probeTimer_ = nullptr;
    std::optional<runtime::TaskHandle<runtime::PreviewPreparationResultHandle>> probeTask_;
    std::optional<runtime::PreviewRequestIdentity> probeIdentity_;
    PreparedPreviewFrameHandle probeTaskFrame_;
    QPoint probeTaskPixel_;
    PreparedPreviewFrameHandle probeCacheFrame_;
    QPoint probeCachePixel_;
    std::optional<core::Color4d> probeCacheReference_;
    std::optional<core::Color4d> probeCacheDisplayLinear_;
    // The packed display sample for a probe whose source frame is the resident arm: it has no CPU
    // pixels, so the sample arrives with the async 1x1 CPU analysis and is cached here.
    std::optional<render::Rgba8> probeCacheDisplay_;
    QString probeFailure_;
    [[nodiscard]] PreparedPreviewFrameHandle displayedFrame() const;
    runtime::ViewAdjust viewAdjust_{};
    kit::KValueField* exposureField_ = nullptr;
    kit::KValueField* gammaField_ = nullptr;
    QTimer* analysisTimer_ = nullptr;
    std::optional<runtime::TaskHandle<runtime::PreviewPreparationResultHandle>> adjustTask_;
    std::optional<runtime::PreviewRequestIdentity> adjustIdentity_;
    PreparedPreviewFrameHandle adjustedFrame_;
    std::optional<CreationGesture> roiGesture_;
    std::optional<QRectF> roiRect_;
    QToolButton* roiButton_ = nullptr;
    QToolButton* roiClearButton_ = nullptr;
    document::PathValue penPath_;
    bool penDown_ = false;
    struct PathSelection {
        document::ParameterId parameter;
        document::PathValue path;
        QTransform toWorld;
    };
    struct PathDrag {
        CreationGesture gesture;
        PathSelection selection;
        QPointF origin;
        std::size_t index;
        int part; // 0 anchor, 1 incoming handle, 2 outgoing handle
    };
    std::optional<PathDrag> pathDrag_;
    std::optional<std::pair<document::ParameterId, std::size_t>> selectedAnchor_;
    [[nodiscard]] std::optional<PathSelection> selectedPath() const;
    bool pathPress(QMouseEvent* event);
    bool pathMove(QMouseEvent* event);
    bool pathRelease(QMouseEvent* event);
    bool pathKey(QKeyEvent* event);
    void cancelPathDrag();
    void finishPen(bool closed);
    void paintPathTools(QPainter& painter) const;
    Tool tool_ = Tool::Select;
    QWidget* toolColumn_ = nullptr;
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
    kit::KDropdown* viewerDisplayView_ = nullptr;
    kit::KIconToggle* viewerLookToggle_ = nullptr;
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
    ViewerBackground background_ = ViewerBackground::Checkerboard;
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
    // GPU-resident presentation state. `gpuResident_` is a plain typed member (no QObject base),
    // created once in the constructor; the native container it owns is parented to this widget and
    // is never reparented or destroyed while a target is live (the host gate retires first).
    std::unique_ptr<ViewerGpuResidentController> gpuResident_;
    QWidget* gpuContainer_ = nullptr;
    // True only while the blank/no-frame transition is retiring a live target before it may unmap
    // the container; the update path must not hide it or start a second retire while pending.
    bool gpuNativeRetirePending_ = false;
    // Bumped on every resolution, resume, or teardown so a stale queued retire is discarded.
    std::uint64_t gpuNativeRetireGeneration_ = 0;
    // True while an external EditorNativeSurface mutation owns the presenter's single retire slot.
    bool gpuHostMutationPending_ = false;
    // The single host request folded into an in-flight internal retirement; a second is refused.
    struct GpuHostMutationWaiter final {
        std::uint64_t generation = 0;
        PrepareCallback completion;
    };
    std::optional<GpuHostMutationWaiter> gpuHostMutationWaiter_;
    // Monotonic token for an external mutation's queued completion; bumping it discards a
    // superseded delivery (the presenter invokes its callback inline, so it is queued).
    std::uint64_t gpuHostMutationGeneration_ = 0;
    // Defensive re-entrancy guard; the repaint/OOM report depth was never established.
    bool gpuPresentationUpdating_ = false;
    QTimer* gpuResidentTimer_ = nullptr;
    bool residentActive_ = false;
    PreparedPreviewFrameHandle lastCpuFrame_;
    // The explicit same-request CPU fallback for a resident frame that could not be presented. It
    // is produced by the existing async viewer-analysis CPU path (never a GPU readback).
    PreparedPreviewFrameHandle cpuFallbackFrame_;
    std::optional<runtime::TaskHandle<runtime::PreviewPreparationResultHandle>> cpuFallbackTask_;
    // The identity the active fallback task was submitted for, and the identity that genuinely
    // failed (so a stale completion is dropped and an actual failure is not retried forever).
    std::optional<runtime::PreviewRequestIdentity> cpuFallbackIdentity_;
    std::optional<runtime::PreviewRequestIdentity> cpuFallbackFailedIdentity_;
    PreparedPreviewFrameHandle gpuPresentedFrame_;
    ViewTransform gpuPresentedTransform_{};
    std::uint64_t gpuOverlayToken_ = 0;
    // True only while renderCpuCoverSnapshot() is painting the CPU content into the cover pixmap.
    bool coverSnapshotInProgress_ = false;
    // Bumped whenever something the native overlay depends on but the view transform does not
    // (selection handles/paths/ROI, overlay toggles) changes, so the overlay signature changes.
    std::uint64_t gpuOverlayRevision_ = 0;
    ViewerOverlayOptions overlayOptions_{};
};

} // namespace bloom::ui
