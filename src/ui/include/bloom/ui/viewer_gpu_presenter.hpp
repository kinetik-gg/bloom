#pragma once

// UI-side viewer GPU presenter: the small Qt adapter that presents one GPU-resident viewer target
// through the existing runtime presentation coordinator.
//
// Ownership (measured, not assumed -- see the lifecycle review's probe):
//   * The UI thread owns the QVulkanInstance (which adopts the coordinator client's borrowed
//     VkInstance through setVkInstance), the QWindow with a real VulkanSurface, and the QWidget
//     returned by QWidget::createWindowContainer. No device, pipeline, queue, or worker thread is
//     created here; every native check and driver call stays on the existing owner thread.
//   * This adapter never reparents, never hides to "park", and never destroys the container while a
//     native target is live. On Qt 6 Wayland a busy reparent silently recreates the VkSurfaceKHR
//     without a SurfaceAboutToBeDestroyed event, and a container destruction frees the surface
//     immediately. The host must therefore call prepareForMutation() and wait for SafeToMutate
//     BEFORE it mutates the widget tree, and it must keep the containing widget tree and this
//     presenter alive until then.
//
// Lifetime preconditions:
//   * `~ViewerGpuPresenter()` requires that no live native target is owned (state() is
//     Uninitialized/Unsupported/Retired or a never-created rejection). Destroying the presenter
//     while a target is live is an ownership-contract violation and CANNOT be made safe: the
//     container's parent still owns the widget and may destroy the live QWindow/VkSurfaceKHR at any
//     time. The destructor therefore never reparents, never hides, and never destroys that
//     container; in debug it asserts the precondition, and in release it emits a truthful
//     diagnostic naming the violation (no `_Exit`, no process-global keeper, no claim that the
//     surface was retained). Normal application shutdown must await every retirement before Qt
//     teardown.
//   * Every proven-terminal record (Retired, or Rejected with surfaceSafeToDestroy) is acknowledged
//     through the owner's bounded `forget()` after its terminal diagnostic has been captured, so
//     repeated detach/attach does not accumulate retained records. A live/unproven/duplicate target
//     is never forgotten.
//   * The borrowed instance view/epoch is only used while the owner generation is valid; a missing,
//     stale, or owner-gone client is reported as Unsupported with a diagnostic and no attach is
//     attempted. There is no blank activation promise and no CPU-fallback claim beyond that
//     diagnostic.
//
// All raw Vulkan interop (VkInstance/VkSurfaceKHR reinterpretation) is confined to this src/ui
// adapter; the runtime types stay Qt-free and Vulkan-free.

#include <bloom/render/gpu_present_image.hpp>
#include <bloom/render/gpu_presentation_types.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/gpu_resident_frame_lease.hpp>

#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class QEvent;
class QTimer;
class QVulkanInstance;
class QWidget;
class QWindow;

namespace bloom::ui {

// Implementation seam (viewer_gpu_presenter_port.hpp): the production implementation wraps the
// existing runtime::GpuPresentationClient; the fake used by the CPU-only adapter test implements
// the same subset. It is never part of the product API.
class ViewerGpuPort;

// One forwarded window input event, translated from the exact Qt event without inventing names.
// `local` is relative to the presenting container; `global` is the screen position. The outer host
// adds the container's own origin to `local` once, after this callback, so this adapter performs no
// layout math.
enum class ViewerGpuInputKind : std::uint8_t {
    MousePress,
    MouseRelease,
    MouseMove,
    MouseDoubleClick,
    Wheel,
    KeyPress,
    KeyRelease,
    Enter,
    Leave,
    FocusIn,
    FocusOut,
    InputMethod,
    GrabMouse,
    UngrabMouse,
    Cancel,
};

struct ViewerGpuInputEvent final {
    ViewerGpuInputKind kind = ViewerGpuInputKind::MouseMove;
    QPointF local;
    QPointF global;
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    QPoint pixelDelta;
    QPoint angleDelta;
    Qt::ScrollPhase scrollPhase = Qt::NoScrollPhase;
    bool inverted = false;
    int key = 0;
    bool autoRepeat = false;
    // Key text (key press/release) or the commit string (InputMethod).
    QString text;
};

class ViewerGpuPresenter final : public QObject {
  public:
    enum class State : std::uint8_t {
        // No window/instance created yet.
        Uninitialized,
        // No borrowed instance, stale client, or Qt adoption failure: the caller keeps its CPU
        // path.
        // No attach was attempted.
        Unsupported,
        // The window exists and the adapter is waiting for a real exposed surface and the attach
        // ack.
        Attaching,
        // The owner admitted the target and it is ready for present updates.
        Active,
        // A resize is in flight; the owner retires the old native extent before recreating.
        Resizing,
        // Retire requested; no new update is admitted and no surface may be mutated yet.
        Retiring,
        // The owner proved retirement (surfaceSafeToDestroy). The surface may be destroyed.
        Retired,
        // Quarantined / unproven / duplicate-surface / owner-gone: the native surface is retained
        // and
        // must not be mutated or destroyed.
        Retained,
    };

    enum class MutationOutcome : std::uint8_t { SafeToMutate, Retained };

    struct MutationResult final {
        MutationOutcome outcome = MutationOutcome::Retained;
        State state = State::Retained;
        bool surfaceSafeToDestroy = false;
        std::string diagnostic;
    };

    struct Config final {
        // Explicit loader path supplied by the app; never a hardcoded workspace path. It is pinned
        // through QT_VULKAN_LIB before the first QVulkanInstance is constructed.
        std::string vulkan_loader_path;
        // Requested target extent in device pixels (the actual extent the coordinator clamps to).
        std::uint32_t target_width = 0;
        std::uint32_t target_height = 0;
        // Device pixel ratio used to size the logical QWindow once.
        double device_pixel_ratio = 1.0;
    };

    using InputCallback = std::function<void(const ViewerGpuInputEvent&)>;
    using ReadyCallback = std::function<void(bool ready, const std::string& diagnostic)>;
    using MutationCallback = std::function<void(const MutationResult&)>;

    // Production: adopts the coordinator's borrowed instance and drives the existing client.
    ViewerGpuPresenter(std::shared_ptr<runtime::GpuPresentationClient> client, Config config,
                       QObject* parent = nullptr);
    // Test-only seam for the CPU-only adapter test. The product never calls this.
    ViewerGpuPresenter(std::shared_ptr<ViewerGpuPort> port, Config config,
                       QObject* parent = nullptr);

    // Direct destructor is only valid with no live target; see the header contract. It neither
    // reparents nor hides the container, and it retains the surface (with a recorded diagnostic) if
    // the precondition is violated.
    ~ViewerGpuPresenter() override;

    ViewerGpuPresenter(const ViewerGpuPresenter&) = delete;
    ViewerGpuPresenter& operator=(const ViewerGpuPresenter&) = delete;
    ViewerGpuPresenter(ViewerGpuPresenter&&) = delete;
    ViewerGpuPresenter& operator=(ViewerGpuPresenter&&) = delete;

    // Idempotent. Must run before the first QVulkanInstance in the process; returns false for an
    // empty or missing path.
    [[nodiscard]] static bool pinVulkanLoader(const std::string& loaderPath);

    // Creates the instance/window/container and starts the bounded poll. Non-blocking. Returns
    // false (state Unsupported) without creating any Qt Vulkan object when the client is unusable.
    [[nodiscard]] bool initialize();

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] QWidget* container() const noexcept;
    [[nodiscard]] QWindow* window() const noexcept;
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] bool attached() const noexcept;
    [[nodiscard]] bool acceptingPresent() const noexcept;
    [[nodiscard]] runtime::GpuPresentationTargetId targetId() const noexcept;
    [[nodiscard]] std::uint64_t lastSequence() const noexcept;
    [[nodiscard]] bool surfaceSafeToDestroy() const noexcept;
    [[nodiscard]] const std::string& diagnostic() const noexcept;
    [[nodiscard]] render::GpuBorrowedInstanceView borrowedInstanceView() const;
    [[nodiscard]] std::uint64_t surfaceBits() const noexcept;

    void setReadyCallback(ReadyCallback callback);
    void setInputCallback(InputCallback callback);

    // Latest-only present. Refused unless the target is Active and no retire is pending.
    [[nodiscard]] bool present(const runtime::GpuResidentFrameLease& lease,
                               const render::GpuPresentImageParams& params,
                               std::shared_ptr<const runtime::GpuPresentationOverlay> overlay);

    // Retires the old native extent on the owner before a new extent is presented. The window is
    // resized once; present() is refused until the owner reports Active again.
    [[nodiscard]] bool requestResize(std::uint32_t deviceWidth, std::uint32_t deviceHeight);

    // Explicit non-blocking retire-before-UI-mutation gate. On SafeToMutate the caller may destroy
    // or reparent the containing widget tree; on Retained it must not. A second pending call is
    // refused.
    [[nodiscard]] bool prepareForMutation(MutationCallback completion);

    // Drives one poll tick synchronously (test/diagnostic); the bounded QTimer calls the same tick.
    [[nodiscard]] bool pollNow();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bloom::ui
