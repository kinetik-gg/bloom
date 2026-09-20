#pragma once

// Viewer-side GPU-resident presentation controller.
//
// This is the single cohesive helper that lets the EXISTING ViewerEditor
// present the closed GPU-resident PreparedPreviewFrame arm through the EXISTING
// ViewerGpuPresenter. It creates no device, pipeline, queue, thread, or native
// renderer: the presenter already owns the only UI-created
// QVulkanInstance/QWindow/container and drives the runtime coordinator; this
// controller only turns a resident frame + the viewer's current view transform
// into one GpuPresentImageParams, records an immutable overlay QPicture on the
// UI thread and rasterizes it on the EXISTING TaskScheduler, and forwards the
// presenter's already-translated window input back into the ViewerEditor's real
// event handlers.
//
// It is deliberately a plain typed member of ViewerEditor (no QObject base, no
// global registry, no service locator, no event bus). The production dependency
// is injected through setDependencies(); tests inject a fake ViewerGpuPort
// through setPresentationPortForTest().
//
// Geometry rule (brief): a resident frame's geometry is read from its
// storage-independent lease metadata (width/height/displayWindow/pixelAspect)
// and the PreparedPreviewFrame identity. The controller NEVER reads a CPU
// display span, because a resident frame has none.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/gpu_present_image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/editor_native_surface.hpp>
#include <bloom/ui/viewer_gpu_presenter.hpp>

#include <QPicture>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QSizeF>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class QWidget;

namespace bloom::ui {

// Test-only implementation seam (viewer_gpu_presenter_port.hpp).
// Forward-declared so the production header stays free of the private client
// subset.
class ViewerGpuPort;

// Storage-independent geometry of one resident display frame, read only from
// the opaque lease and the frame identity -- never from a CPU pixel span.
struct ResidentFrameGeometry final {
    render::ImageExtent displayExtent;
    render::ImageWindow displayWindow;
    core::PixelAspectRatio pixelAspect;
    runtime::GpuResidentFrameLease lease;
};

// One immutable overlay recording made on the UI thread. `picture` is recorded
// in target-logical coordinates; `logicalSize` is the coordinate space it was
// recorded in; `token` identifies the overlay generation so the presenter
// uploads only on a real change.
struct ResidentOverlaySource final {
    QPicture picture;
    QSizeF logicalSize;
    std::uint64_t token = 0;
    bool valid = false;
};

// Everything the controller needs to build one present update. All rectangles
// are in target-logical (widget) coordinates; `containerRect` is the rect the
// native container covers.
struct ResidentPresentRequest final {
    QRectF destination;   // viewTransformedDisplayRect(), PAR already folded
    QRectF containerRect; // widget-logical rect the native container covers
    double devicePixelRatio = 1.0;
    render::GpuPresentChannel channel = render::GpuPresentChannel::Rgba;
    render::GpuPresentBackground background = render::GpuPresentBackground::Solid;
    render::GpuPresentColor backgroundColor{0.0F, 0.0F, 0.0F, 1.0F};
    render::GpuPresentColor checkerColorA{0.0F, 0.0F, 0.0F, 1.0F};
    render::GpuPresentColor checkerColorB{1.0F, 1.0F, 1.0F, 1.0F};
    float checkerTilePixels = 22.0F;
    // Stable signature of everything the overlay recording depends on (frame
    // identity, view transform, channel, background, overlay options, selection
    // revision). When it matches the last presented overlay, the retained overlay
    // is reused and no raster is scheduled.
    std::uint64_t overlaySignature = 0;
    ResidentOverlaySource overlay;
};

class ViewerGpuResidentController final {
  public:
    struct Dependencies final {
        std::shared_ptr<runtime::GpuPresentationClient> client;
        runtime::TaskScheduler* scheduler = nullptr;
        // The widget the native container is parented to BEFORE the window is
        // exposed/attached. Must outlive the controller; the presenter never
        // reparents a live surface.
        QWidget* containerParent = nullptr;
        std::string vulkanLoaderPath;
        double devicePixelRatio = 1.0;
    };

    using InputSink = std::function<void(const ViewerGpuInputEvent&)>;
    // Fired exactly once per present request, with the truthful result: true only
    // after the owner published a genuine presentCount/appliedSequence advance
    // for that request; false on a refusal/unsupported/stale-lease path.
    using PresentAck = std::function<void(bool presented, const std::string& diagnostic)>;
    // Invoked when a resident frame cannot be presented, so the host can run its
    // explicit same-request CPU fallback. Never a GPU readback.
    using CpuFallback = std::function<void()>;
    // Supplies the last valid CPU image as a pixmap for the native cover that
    // keeps it visible over the (not yet acknowledged) native child. Called on
    // the UI thread.
    using CpuCoverSnapshot = std::function<QPixmap()>;

    ViewerGpuResidentController();
    ~ViewerGpuResidentController();
    ViewerGpuResidentController(const ViewerGpuResidentController&) = delete;
    ViewerGpuResidentController& operator=(const ViewerGpuResidentController&) = delete;

    void setDependencies(Dependencies dependencies);
    // Test-only seam; the product never calls this.
    void setPresentationPortForTest(std::shared_ptr<ViewerGpuPort> port);
    void setInputSink(InputSink sink);
    void setPresentAck(PresentAck ack);
    void setCpuFallback(CpuFallback callback);
    // A native cover widget is created (as a sibling above the native container)
    // that paints the supplied CPU snapshot until a genuine owner present
    // acknowledgement, so the native child can never blank the viewer during
    // first attach or a resize. Input-transparent; retired with the host tree.
    void setCpuCoverSnapshot(CpuCoverSnapshot snapshot);
    [[nodiscard]] QWidget* cpuCoverForTest() const noexcept;
    // Hides the native CPU cover so the host's own CPU paint (or no frame) is
    // exposed. Safe when no cover exists; never destroys or reparents a surface.
    void concealCpuCover();
    [[nodiscard]] bool cpuCoverVisibleForTest() const noexcept;
    // Fired whenever the presenter's state changed since the previous poll
    // (attach ack, retire, refusal), so the host can re-evaluate whether to
    // present or fall back.
    void setStateChanged(std::function<void()> callback);

    // Drives one non-blocking tick: drains a completed overlay raster, fires a
    // genuine present acknowledgement when the owner publishes one, and reports
    // any presenter state change. Safe to call from a UI timer; never blocks.
    void poll();

    // The swapchain resize gate: retires the old native extent and re-creates it.
    // Present is refused until the owner reports the new extent Active again.
    // Returns false when there is no live target to resize.
    [[nodiscard]] bool requestTargetResize(std::uint32_t deviceWidth, std::uint32_t deviceHeight);

    [[nodiscard]] bool configured() const noexcept;
    [[nodiscard]] bool unsupported() const noexcept;
    [[nodiscard]] bool hasLiveTarget() const noexcept;
    [[nodiscard]] ViewerGpuPresenter::State presenterState() const noexcept;
    [[nodiscard]] const std::string& diagnostic() const noexcept;

    // The container the presenter created, or nullptr before initialize()/on
    // Unsupported. The host (ViewerEditor) parents it and keeps it until a
    // genuinely safe mutation.
    [[nodiscard]] QWidget* container() const noexcept;

    // Presents `frame` if it is the resident arm and the controller is usable.
    // Returns false when the caller must keep its CPU paint (not configured,
    // unsupported, or not a resident frame). The actual admission result (if any)
    // arrives through the present ack.
    [[nodiscard]] bool present(const runtime::PreparedPreviewFrame& frame,
                               const ResidentPresentRequest& request);

    // EditorNativeSurface passthroughs.
    [[nodiscard]] EditorNativeSurface::PrepareOutcome
    prepareForMutation(std::uint64_t generation,
                       const EditorNativeSurface::PrepareCallback& completion);
    void resumeAfterMutation();
    [[nodiscard]] std::string mutationDiagnostic() const;

    // Once a live target has been proven Retired, the old presenter can never
    // reattach; the controller destroys it and (if a resident frame is still
    // held) lazily builds a fresh one.
    void resetRetiredPresenter();

    // Diagnostics / tests.
    [[nodiscard]] std::size_t presentAttemptCount() const noexcept;
    [[nodiscard]] std::size_t acceptedPresentCount() const noexcept;
    [[nodiscard]] bool lastPresentWasResident() const noexcept;
    // True once the owner genuinely acknowledged the most recent present request.
    [[nodiscard]] bool presentationAcknowledged() const noexcept;
    [[nodiscard]] std::uint64_t presentedSequence() const noexcept;
    [[nodiscard]] std::size_t overlayRasterCount() const noexcept;
    // Genuine owner-observed native present progress for the live presenter (read-only): the
    // applied sequence and applied present count the presentation owner published, and the last
    // sequence this controller enqueued. A mailbox admission never advances these; a caller that
    // needs proof a specific request was genuinely presented waits until
    // nativeAppliedSequence() >= nativeLastEnqueuedSequence(). Zero when no presenter exists.
    [[nodiscard]] std::uint64_t nativeAppliedSequence() const noexcept;
    [[nodiscard]] std::uint64_t nativePresentCount() const noexcept;
    [[nodiscard]] std::uint64_t nativeLastEnqueuedSequence() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Pure geometry helper (used by the controller and directly testable): reads
// the resident arm's storage-independent geometry, or nullopt when `frame` is
// not a live resident frame.
[[nodiscard]] std::optional<ResidentFrameGeometry>
residentFrameGeometry(const runtime::PreparedPreviewFrame& frame) noexcept;

// True only when the owner has published a present whose applied sequence reaches the request's
// enqueued sequence. A nonzero present count or a mailbox admission is never sufficient: an older
// in-flight present can advance the count while its sequence is below a newer pending request.
[[nodiscard]] bool presentSequenceAcknowledged(std::uint64_t appliedSequence,
                                               std::uint64_t enqueuedSequence) noexcept;

} // namespace bloom::ui
