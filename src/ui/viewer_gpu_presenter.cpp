// ViewerGpuPresenter implementation. All Qt/Vulkan interop for the adapter lives here.

#include <bloom/ui/viewer_gpu_presenter.hpp>

#include "viewer_gpu_presenter_input.hpp"
#include "viewer_gpu_presenter_port.hpp"

#include <cstdint>
#include <utility>

// Portable UI builds compile this TU empty; the Unsupported fallback lives in the portable TU.
#ifdef BLOOM_UI_HAS_VULKAN

#include <QEvent>
#include <QFileInfo>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QTimer>
#include <QVulkanInstance>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace bloom::ui {
namespace {

constexpr int kPollIntervalMs = 8;

[[nodiscard]] VkInstance instanceOf(const std::uint64_t bits) noexcept {
    // The borrowed VkInstance crosses Qt's QVulkanInstance boundary as an integer handle; this is
    // the single documented reconstruction point.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(bits));
}

[[nodiscard]] std::uint64_t bitsOf(const VkSurfaceKHR surface) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(surface));
}

[[nodiscard]] std::uint32_t logicalExtent(const std::uint32_t devicePixels,
                                          const double ratio) noexcept {
    const double safeRatio = ratio > 0.0 ? ratio : 1.0;
    const double logical = std::ceil(static_cast<double>(devicePixels) / safeRatio);
    return logical >= 1.0 ? static_cast<std::uint32_t>(logical) : 1U;
}

// Embedded-window ownership rule. When the presenter is hosted in a QWindowContainer
// (containerParent set), the host editor/controller sizes the embedded QWindow from its logical
// contentRect and Qt applies the live widget device pixel ratio. Resizing that QWindow from here
// would make the presenter a second owner using the once-injected config ratio; under fractional
// scaling (live 1.5 vs injected 1.0, say) the two owners disagree and fight. A standalone presenter
// (no host container) is the sole owner and sizes its own window.
[[nodiscard]] bool presenterOwnsWindowGeometry(const QWidget* containerParent) noexcept {
    return containerParent == nullptr;
}

// The live window ratio is authoritative for a standalone presenter; the injected config ratio is
// only the fallback for a window that has no screen yet.
[[nodiscard]] double standaloneWindowRatio(const QWindow* window,
                                           const double configured) noexcept {
    const double live = window != nullptr ? window->devicePixelRatio() : 0.0;
    if (live > 0.0) {
        return live;
    }
    return configured > 0.0 ? configured : 1.0;
}

} // namespace

struct ViewerGpuPresenter::Impl final {
    Impl(ViewerGpuPresenter* presenterValue, std::shared_ptr<ViewerGpuPort> portValue,
         Config configValue)
        : self(presenterValue), port(std::move(portValue)), config(std::move(configValue)) {}

    ~Impl() {
        if (timer != nullptr) {
            timer->stop();
        }
        if (container != nullptr && self != nullptr) {
            container->removeEventFilter(self);
        }
        if (window != nullptr && self != nullptr) {
            window->removeEventFilter(self);
        }
        readyCallback = nullptr;
        inputCallback = nullptr;
        mutationCallback = nullptr;
        if (container != nullptr && attached && !safeToDestroy) {
            // PRECONDITION VIOLATION. This is NOT a safe lifetime handoff and does not retain the
            // surface: the container's parent still owns it (or the host is about to reparent it)
            // and may destroy the live QWindow/VkSurfaceKHR at any time. The contract is that the
            // host calls prepareForMutation() and waits for SafeToMutate before this destructor can
            // run. Nothing here can repair that, so we only (a) avoid a second delete of a widget
            // that is not ours, and (b) keep the adopted QVulkanInstance object alive so the still-
            // owned window has no dangling instance pointer for as long as the parent keeps it. No
            // _Exit, no process-global keeper, and no claim that the surface was retained.
            std::fprintf(stderr,
                         "ViewerGpuPresenter: FORBIDDEN destructor with a live target (state=%u): "
                         "the parent still owns the container and may destroy the live surface; "
                         "this is an ownership-contract violation, not a safe handoff\n",
                         static_cast<unsigned>(state));
            Q_ASSERT(false);
            // The quarantined instance must not be deleted while its surface is still busy;
            // release() deliberately hands ownership to the caller and the result is dropped.
            // NOLINTNEXTLINE(bugprone-unused-return-value)
            static_cast<void>(instance.release());
        } else {
            delete container; // owns the QWindow
            instance.reset();
        }
        container = nullptr;
        window = nullptr;
    }

    [[nodiscard]] bool initialize() {
        if (initialized) {
            return true;
        }
        if (!ViewerGpuPresenter::pinVulkanLoader(config.vulkan_loader_path)) {
            return fail(State::Unsupported,
                        "no usable pinned Vulkan loader path was supplied by the application");
        }
        if (port == nullptr) {
            return fail(State::Unsupported, "no presentation client/port was supplied");
        }
        if (config.target_width == 0U || config.target_height == 0U) {
            return fail(State::Unsupported, "a positive target extent is required");
        }
        view = port->instanceView();
        if (!view.valid) {
            return fail(State::Unsupported,
                        "the borrowed instance view is unavailable (client missing, stale, or the "
                        "presentation owner is gone)");
        }
        instance = std::make_unique<QVulkanInstance>();
        instance->setVkInstance(instanceOf(view.instance_bits));
        if (!instance->create() || !instance->isValid()) {
            instance.reset();
            return fail(State::Unsupported,
                        "QVulkanInstance could not adopt the borrowed instance");
        }
        targetWidth = config.target_width;
        targetHeight = config.target_height;

        window = new QWindow();
        window->setSurfaceType(QSurface::VulkanSurface);
        window->setVulkanInstance(instance.get());
        // Only a standalone presenter owns its QWindow geometry. An embedded QWindow is sized by
        // its QWindowContainer from the host's live logical contentRect and DPR; sizing it here
        // with the once-injected config ratio seeds an immediate size disagreement that the
        // container undoes, driving redundant native geometry changes (and parent-expose/SHM-buffer
        // churn).
        if (presenterOwnsWindowGeometry(containerParent)) {
            const double ratio = standaloneWindowRatio(window, config.device_pixel_ratio);
            window->resize(static_cast<int>(logicalExtent(targetWidth, ratio)),
                           static_cast<int>(logicalExtent(targetHeight, ratio)));
        }
        window->installEventFilter(self);

        container = QWidget::createWindowContainer(window);
        if (containerParent != nullptr) {
            // Parent BEFORE the window is exposed/attached, so the host never reparents a live
            // surface.
            container->setParent(containerParent);
        }
        container->setObjectName(QStringLiteral("bloomViewerGpuContainer"));
        container->setFocusPolicy(Qt::StrongFocus);
        container->setMouseTracking(true);
        container->setAttribute(Qt::WA_AcceptTouchEvents, false);
        container->installEventFilter(self);

        surfaceBits = 0;
        surfaceDestroyed = false;
        safeToDestroy = false;
        attached = false;
        initialized = true;
        state = State::Attaching;
        startTimer();
        return true;
    }

    [[nodiscard]] bool fail(const State terminal, std::string message) {
        state = terminal;
        diagnostic = std::move(message);
        safeToDestroy = true; // no native target exists or will be created in this branch
        stopTimer();
        publishReady(false, diagnostic);
        return false;
    }

    void retain(std::string message) {
        state = State::Retained;
        diagnostic = std::move(message);
        safeToDestroy = false;
        stopTimer();
        publishReady(false, diagnostic);
        if (mutationPending) {
            mutationPending = false;
            MutationCallback callback = std::move(mutationCallback);
            mutationCallback = nullptr;
            if (callback) {
                callback(MutationResult{MutationOutcome::Retained, state, false, diagnostic});
            }
        }
    }

    void publishReady(const bool ready, const std::string& message) {
        if (readyNotified) {
            return;
        }
        readyNotified = true;
        if (readyCallback) {
            readyCallback(ready, message);
        }
    }

    void startTimer() {
        if (timer != nullptr) {
            if (!timer->isActive()) {
                timer->start();
            }
            return;
        }
        timer = new QTimer(self);
        timer->setInterval(kPollIntervalMs);
        timer->setTimerType(Qt::CoarseTimer);
        QPointer<ViewerGpuPresenter> guard(self);
        QObject::connect(timer, &QTimer::timeout, self, [this, guard] {
            if (!guard.isNull()) {
                tick();
            }
        });
        timer->start();
    }

    void stopTimer() {
        if (timer != nullptr) {
            timer->stop();
        }
    }

    void tick() {
        if (!initialized || !port) {
            return;
        }
        if (state == State::Retiring || state == State::Active || state == State::Resizing ||
            state == State::Attaching) {
            if (!port->ownerAlive()) {
                retain("the presentation owner is gone; the surface is retained (no safe ack)");
                return;
            }
        }
        if (state == State::Attaching && !attached) {
            tryAttach();
        }
        if (attached && !safeToDestroy) {
            detectSilentSurfaceReplacement();
        }
        if (attached && !safeToDestroy) {
            applySnapshot(port->status(target));
        }
    }

    void tryAttach() {
        // A surface only exists once the platform has actually created and exposed it.
        if (!window->isExposed()) {
            return;
        }
        const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(window);
        if (surface == VK_NULL_HANDLE) {
            if (surfaceDestroyed) {
                retain("the platform surface was destroyed before attach");
            }
            return;
        }
        render::GpuBorrowedSurface borrowed;
        borrowed.surface_bits = bitsOf(surface);
        borrowed.epoch = view.epoch;
        const runtime::GpuPresentationPortResult result =
            port->attach(borrowed, targetWidth, targetHeight);
        switch (result.code) {
        case runtime::GpuPresentationPortCode::Accepted:
            surfaceBits = borrowed.surface_bits;
            target = result.target;
            attached = true;
            diagnostic = result.message;
            break;
        case runtime::GpuPresentationPortCode::DuplicateSurface:
            // Another live target owns this surface; the existing owner is untouched and this
            // surface is NOT safe to destroy.
            retain("the borrowed surface is already owned by a live target; retained");
            break;
        case runtime::GpuPresentationPortCode::Rejected:
        case runtime::GpuPresentationPortCode::TooManyTargets:
        case runtime::GpuPresentationPortCode::OverBudget:
            // Refused before any native target for this surface was created.
            static_cast<void>(fail(State::Unsupported, result.message.empty()
                                                           ? "the target attach was refused"
                                                           : result.message));
            break;
        case runtime::GpuPresentationPortCode::ShuttingDown:
        case runtime::GpuPresentationPortCode::OwnerGone:
        case runtime::GpuPresentationPortCode::Coalesced:
        case runtime::GpuPresentationPortCode::UnknownTarget:
        case runtime::GpuPresentationPortCode::StaleSequence:
        case runtime::GpuPresentationPortCode::Closed:
        default:
            retain(result.message.empty() ? "the target attach was not admitted" : result.message);
            break;
        }
    }

    void detectSilentSurfaceReplacement() {
        if (!window->isExposed()) {
            return;
        }
        const VkSurfaceKHR current = QVulkanInstance::surfaceForWindow(window);
        if (current == VK_NULL_HANDLE) {
            return;
        }
        // Measured hazard: Qt can replace the VkSurfaceKHR without SurfaceAboutToBeDestroyed.
        if (bitsOf(current) != surfaceBits) {
            retain("the platform VkSurfaceKHR changed without a destruction event; retained");
        }
    }

    void applySnapshot(const runtime::GpuPresentationTargetSnapshot& snapshot) {
        if (snapshot.id == target) {
            appliedSequenceValue = snapshot.appliedSequence;
            presentCountValue = snapshot.presentCount;
        }
        switch (snapshot.state) {
        case runtime::GpuPresentationTargetState::Attaching:
            state = mutationPending ? State::Retiring : State::Attaching;
            break;
        case runtime::GpuPresentationTargetState::Active:
            // A retire already admitted by the port must never be downgraded to Active while the
            // owner has not yet published the terminal snapshot.
            if (mutationPending) {
                state = State::Retiring;
                break;
            }
            if (state == State::Resizing) {
                pendingResize = false;
            }
            state = State::Active;
            publishReady(true, std::string{});
            break;
        case runtime::GpuPresentationTargetState::Resizing:
            state = mutationPending ? State::Retiring : State::Resizing;
            break;
        case runtime::GpuPresentationTargetState::Retiring:
            state = State::Retiring;
            break;
        case runtime::GpuPresentationTargetState::Retired:
            state = State::Retired;
            safeToDestroy = true;
            if (snapshot.surfaceSafeToDestroy) {
                stopTimer();
                // Preserve the terminal diagnostic before the owner record is reclaimed.
                diagnostic = snapshot.message;
                if (mutationPending) {
                    mutationPending = false;
                    MutationCallback callback = std::move(mutationCallback);
                    mutationCallback = nullptr;
                    if (callback) {
                        callback(MutationResult{MutationOutcome::SafeToMutate, State::Retired, true,
                                                snapshot.message});
                    }
                }
                reclaimTerminalRecord();
            } else {
                // Published Retired without the safety proof: never invent a safe ack.
                retain("the target reported Retired without surfaceSafeToDestroy; retained");
            }
            break;
        case runtime::GpuPresentationTargetState::Rejected:
            // Rejected is terminal-safe only when the owner proves no native target was created.
            // A Rejected without that proof is retained and never forgotten.
            if (!snapshot.surfaceSafeToDestroy) {
                retain(snapshot.message.empty()
                           ? "a Rejected target without a safety proof is retained"
                           : snapshot.message);
                break;
            }
            state = State::Unsupported;
            safeToDestroy = true;
            stopTimer();
            // Preserve the terminal diagnostic before the owner record is reclaimed.
            diagnostic = snapshot.message;
            publishReady(false, snapshot.message);
            if (mutationPending) {
                mutationPending = false;
                MutationCallback callback = std::move(mutationCallback);
                mutationCallback = nullptr;
                if (callback) {
                    callback(MutationResult{MutationOutcome::SafeToMutate, state, true,
                                            snapshot.message});
                }
            }
            reclaimTerminalRecord();
            break;
        case runtime::GpuPresentationTargetState::Quarantined:
        case runtime::GpuPresentationTargetState::Unproven:
        case runtime::GpuPresentationTargetState::Gone:
        default:
            retain(snapshot.message.empty() ? "the target did not reach a provable Retired state"
                                            : snapshot.message);
            break;
        }
    }

    [[nodiscard]] bool safeNow() const noexcept { return safeToDestroy; }

    // Releases the owner-side record for a terminal-safe target, after the terminal message.
    void reclaimTerminalRecord() {
        if (terminalRecordReclaimed || target == runtime::kInvalidPresentationTarget || !port) {
            return;
        }
        const runtime::GpuPresentationPortResult result = port->forget(target);
        if (result.accepted() || result.code == runtime::GpuPresentationPortCode::UnknownTarget) {
            terminalRecordReclaimed = true;
            return;
        }
        // The record stays retained (truthful, non-fatal); no blind retry is attempted.
        if (!result.message.empty()) {
            diagnostic = result.message;
        }
    }

    [[nodiscard]] bool present(const runtime::GpuResidentFrameLease& lease,
                               const render::GpuPresentImageParams& params,
                               std::shared_ptr<const runtime::GpuPresentationOverlay> overlay) {
        if (state != State::Active || mutationPending || !attached) {
            diagnostic = "present refused: the target is not Active or a retire is pending";
            return false;
        }
        const std::uint64_t sequenceValue = ++sequence;
        runtime::GpuPresentationUpdate updateValue;
        updateValue.lease = lease;
        updateValue.params = params;
        updateValue.overlay = std::move(overlay);
        const runtime::GpuPresentationPortResult result =
            port->update(target, sequenceValue, std::move(updateValue));
        if (result.accepted()) {
            lastEnqueued = sequenceValue;
            return true;
        }
        diagnostic = result.message.empty() ? "the present update was refused" : result.message;
        return false;
    }

    [[nodiscard]] bool requestResize(const std::uint32_t deviceWidth,
                                     const std::uint32_t deviceHeight) {
        if (state != State::Active || mutationPending || !attached) {
            diagnostic = "resize refused: the target is not Active or a retire is pending";
            return false;
        }
        if (deviceWidth == 0U || deviceHeight == 0U) {
            diagnostic = "resize refused: a positive extent is required";
            return false;
        }
        targetWidth = deviceWidth;
        targetHeight = deviceHeight;
        // Only a standalone presenter owns its QWindow geometry. An embedded QWindow (hosted by the
        // editor/controller through a QWindowContainer) is sized by that container from its logical
        // contentRect with the live widget DPR; resizing it here with the stale injected config
        // ratio is the ownership conflict this method used to create. The exact requested physical
        // extent is still handed to the GPU port below either way, so the coordinator's swapchain
        // matches atomically-authored content.
        if (presenterOwnsWindowGeometry(containerParent)) {
            const double ratio = standaloneWindowRatio(window, config.device_pixel_ratio);
            window->resize(static_cast<int>(logicalExtent(deviceWidth, ratio)),
                           static_cast<int>(logicalExtent(deviceHeight, ratio)));
        }
        pendingResize = true;
        state = State::Resizing;
        const std::uint64_t sequenceValue = ++sequence;
        const runtime::GpuPresentationPortResult result =
            port->resize(target, sequenceValue, deviceWidth, deviceHeight);
        if (result.accepted()) {
            lastEnqueued = sequenceValue;
            return true;
        }
        pendingResize = false;
        state = State::Active;
        diagnostic = result.message.empty() ? "the resize was refused" : result.message;
        return false;
    }

    [[nodiscard]] bool prepareForMutation(MutationCallback callback) {
        if (mutationPending) {
            diagnostic = "a retire is already pending";
            return false;
        }
        if (!initialized || state == State::Uninitialized || state == State::Unsupported ||
            state == State::Retired) {
            const bool safe = safeToDestroy;
            if (callback) {
                callback(MutationResult{
                    safe ? MutationOutcome::SafeToMutate : MutationOutcome::Retained, state, safe,
                    safe ? "no live presentation target"
                         : "no proven release for the retained surface"});
            }
            return true;
        }
        if (state == State::Retained) {
            if (callback) {
                callback(MutationResult{MutationOutcome::Retained, state, false, diagnostic});
            }
            return true;
        }
        if (!attached) {
            // The attach has not been admitted yet, so no native target exists for this surface.
            // Stop any later attach attempt and report the surface safe rather than retaining it.
            state = State::Unsupported;
            safeToDestroy = true;
            stopTimer();
            if (callback) {
                callback(MutationResult{MutationOutcome::SafeToMutate, state, true,
                                        "no native presentation target was created"});
            }
            return true;
        }
        if (state == State::Retiring) {
            mutationPending = true;
            mutationCallback = std::move(callback);
            startTimer();
            return true;
        }
        // Attaching / Active / Resizing: close admission and ask the owner to retire.
        mutationPending = true;
        mutationCallback = std::move(callback);
        state = State::Retiring;
        const std::uint64_t sequenceValue = ++sequence;
        const runtime::GpuPresentationPortResult result = port->retire(target, sequenceValue);
        if (result.accepted() || result.code == runtime::GpuPresentationPortCode::Closed) {
            startTimer();
            return true;
        }
        // UnknownTarget / OwnerGone / ShuttingDown: no genuine proof. Retain, never fake-ack.
        mutationPending = false;
        MutationCallback pending = std::move(mutationCallback);
        mutationCallback = nullptr;
        state = State::Retained;
        diagnostic = result.message.empty() ? "the retire was not admitted; surface retained"
                                            : result.message;
        safeToDestroy = false;
        stopTimer();
        if (pending) {
            pending(MutationResult{MutationOutcome::Retained, state, false, diagnostic});
        }
        return false;
    }

    [[nodiscard]] bool handleEvent(QObject* watched, QEvent* event) {
        if (watched != window && watched != container) {
            return false;
        }
        if (event->type() == QEvent::PlatformSurface) {
            auto* surfaceEvent = static_cast<QPlatformSurfaceEvent*>(event);
            if (surfaceEvent->surfaceEventType() ==
                QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
                surfaceDestroyed = true;
                if (attached && !safeToDestroy) {
                    retain("the platform surface is being destroyed while a target is live");
                }
            }
            return false;
        }
        static_cast<void>(
            detail::forwardViewerGpuInput(event, lastLocal, lastGlobal, inputCallback));
        return false;
    }

    ViewerGpuPresenter* self = nullptr;
    std::shared_ptr<ViewerGpuPort> port;
    Config config;
    bool initialized = false;

    std::unique_ptr<QVulkanInstance> instance;
    QWindow* window = nullptr;
    QWidget* container = nullptr;
    QTimer* timer = nullptr;

    render::GpuBorrowedInstanceView view;
    runtime::GpuPresentationTargetId target = runtime::kInvalidPresentationTarget;
    std::uint64_t sequence = 0;
    std::uint64_t surfaceBits = 0;
    std::uint32_t targetWidth = 0;
    std::uint32_t targetHeight = 0;
    bool attached = false;
    bool surfaceDestroyed = false;
    bool safeToDestroy = false;
    bool pendingResize = false;
    bool mutationPending = false;
    bool readyNotified = false;
    bool terminalRecordReclaimed = false;

    // Owner-observed present progress (from the published snapshot). A mailbox admission is not a
    // native present; presentCountValue only advances when the owner actually presents.
    std::uint64_t appliedSequenceValue = 0;
    std::uint64_t presentCountValue = 0;
    std::uint64_t lastEnqueued = 0;
    QWidget* containerParent = nullptr;

    QPointF lastLocal{0.0, 0.0};
    QPointF lastGlobal{0.0, 0.0};

    State state = State::Uninitialized;
    std::string diagnostic;

    ReadyCallback readyCallback;
    InputCallback inputCallback;
    MutationCallback mutationCallback;
};

ViewerGpuPresenter::ViewerGpuPresenter(std::shared_ptr<runtime::GpuPresentationClient> client,
                                       Config config, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(
                           this, makeRuntimeViewerGpuPort(std::move(client)), std::move(config))) {}

ViewerGpuPresenter::ViewerGpuPresenter(std::shared_ptr<ViewerGpuPort> port, Config config,
                                       QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this, std::move(port), std::move(config))) {}

ViewerGpuPresenter::~ViewerGpuPresenter() {
    // Precondition: no live native target (retired via prepareForMutation/SafeToMutate).
    Q_ASSERT_X(impl_ == nullptr || !impl_->attached || impl_->safeToDestroy, "~ViewerGpuPresenter",
               "destroyed with a live GPU presentation target; host must settle "
               "prepareForMutation() first");
    impl_.reset();
}

bool ViewerGpuPresenter::pinVulkanLoader(const std::string& loaderPath) {
    if (loaderPath.empty()) {
        return false;
    }
    const QString path = QString::fromStdString(loaderPath);
    if (!QFileInfo::exists(path)) {
        return false;
    }
    if (!qputenv("QT_VULKAN_LIB", path.toLocal8Bit())) {
        return false;
    }
    return true;
}

bool ViewerGpuPresenter::initialize() { return impl_->initialize(); }

bool ViewerGpuPresenter::initialized() const noexcept { return impl_->initialized; }

QWidget* ViewerGpuPresenter::container() const noexcept { return impl_->container; }

QWindow* ViewerGpuPresenter::window() const noexcept { return impl_->window; }

ViewerGpuPresenter::State ViewerGpuPresenter::state() const noexcept { return impl_->state; }

bool ViewerGpuPresenter::attached() const noexcept { return impl_->attached; }

bool ViewerGpuPresenter::acceptingPresent() const noexcept {
    return impl_->state == State::Active && impl_->attached && !impl_->mutationPending;
}

runtime::GpuPresentationTargetId ViewerGpuPresenter::targetId() const noexcept {
    return impl_->target;
}

std::uint64_t ViewerGpuPresenter::lastSequence() const noexcept { return impl_->sequence; }

bool ViewerGpuPresenter::surfaceSafeToDestroy() const noexcept { return impl_->safeNow(); }

const std::string& ViewerGpuPresenter::diagnostic() const noexcept { return impl_->diagnostic; }

render::GpuBorrowedInstanceView ViewerGpuPresenter::borrowedInstanceView() const {
    return impl_->view;
}

std::uint64_t ViewerGpuPresenter::surfaceBits() const noexcept { return impl_->surfaceBits; }

std::uint64_t ViewerGpuPresenter::lastEnqueuedSequence() const noexcept {
    return impl_->lastEnqueued;
}

std::uint64_t ViewerGpuPresenter::appliedSequence() const noexcept {
    return impl_->appliedSequenceValue;
}

std::uint64_t ViewerGpuPresenter::presentCount() const noexcept { return impl_->presentCountValue; }

void ViewerGpuPresenter::setContainerParent(QWidget* parent) noexcept {
    impl_->containerParent = parent;
}

void ViewerGpuPresenter::setReadyCallback(ReadyCallback callback) {
    impl_->readyCallback = std::move(callback);
}

void ViewerGpuPresenter::setInputCallback(InputCallback callback) {
    impl_->inputCallback = std::move(callback);
}

bool ViewerGpuPresenter::present(const runtime::GpuResidentFrameLease& lease,
                                 const render::GpuPresentImageParams& params,
                                 std::shared_ptr<const runtime::GpuPresentationOverlay> overlay) {
    return impl_->present(lease, params, std::move(overlay));
}

bool ViewerGpuPresenter::requestResize(const std::uint32_t deviceWidth,
                                       const std::uint32_t deviceHeight) {
    return impl_->requestResize(deviceWidth, deviceHeight);
}

bool ViewerGpuPresenter::prepareForMutation(MutationCallback completion) {
    return impl_->prepareForMutation(std::move(completion));
}

bool ViewerGpuPresenter::pollNow() {
    impl_->tick();
    return impl_->state == State::Attaching || impl_->state == State::Active ||
           impl_->state == State::Resizing || impl_->state == State::Retiring;
}

bool ViewerGpuPresenter::eventFilter(QObject* watched, QEvent* event) {
    return impl_->handleEvent(watched, event);
}

} // namespace bloom::ui

#endif // BLOOM_UI_HAS_VULKAN
