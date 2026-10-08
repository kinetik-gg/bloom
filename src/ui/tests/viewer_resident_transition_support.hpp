#pragma once

#include "ocio_gpu_program_fault.hpp"
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/ui/kit/dropdown.hpp>

// A test-only observation barrier around the real owner-thread executor. Admission, submission,
// cancellation and fence retirement remain production code. Once a native submission really exists,
// temporarily stop polling it so the UI can deterministically supersede that exact work.
struct NativeTransitionGate final {
    std::atomic<bool> armed{false};
    std::atomic<bool> submitted{false};
    std::atomic<bool> failNextPoll{false};
    std::atomic<bool> displayArmed{false};
    std::atomic<bool> displayRetiring{false};
    std::atomic<bool> completionRetained{false};
};

[[nodiscard]] std::shared_ptr<NativeTransitionGate> installTransitionGate(
    const std::shared_ptr<bloom::runtime::detail::PreviewDisplayServiceCore>& core) {
    auto gate = std::make_shared<NativeTransitionGate>();
    const std::weak_ptr<bloom::runtime::detail::PreviewDisplayServiceCore> weakCore(core);
    core->residentExecutorPollOverride = [gate,
                                          weakCore](bloom::runtime::GpuSceneExecutor& executor) {
        if (gate->submitted.load(std::memory_order_acquire) &&
            gate->armed.load(std::memory_order_acquire)) {
            const auto owner = weakCore.lock();
            if (owner && owner->residentNativeActive &&
                owner->residentNativeActive->cancellationRequested &&
                gate->failNextPoll.exchange(false, std::memory_order_acq_rel)) {
                return bloom::runtime::GpuSceneExecutorPollResult::WrongThread;
            }
            return bloom::runtime::GpuSceneExecutorPollResult::Pending;
        }
        if (gate->failNextPoll.exchange(false, std::memory_order_acq_rel)) {
            // Explicit typed poll-failure seam; this does not claim actual driver device loss.
            return bloom::runtime::GpuSceneExecutorPollResult::WrongThread;
        }
        const auto result = executor.poll();
        if (gate->armed.load(std::memory_order_acquire) && executor.hasUnretiredSubmission()) {
            gate->submitted.store(true, std::memory_order_release);
            return bloom::runtime::GpuSceneExecutorPollResult::Pending;
        }
        return result;
    };
    const auto observeRetirement = [gate, weakCore] {
        const auto owner = weakCore.lock();
        if (owner && gate->displayArmed.load(std::memory_order_acquire) &&
            owner->residentNativeActive &&
            owner->residentNativeActive->residentPhase ==
                bloom::runtime::detail::ResidentNativePhase::Retiring) {
            gate->completionRetained.store(owner->residentNativeActive->completion.isValid() &&
                                               owner->residentNativeInFlight,
                                           std::memory_order_release);
            gate->displayRetiring.store(true, std::memory_order_release);
        }
    };
    core->residentDisplayPollOverride =
        [gate, observeRetirement](bloom::render::GpuResidentDisplay& display) {
            observeRetirement();
            if (gate->displayArmed.load(std::memory_order_acquire) &&
                display.hasUnretiredSubmission()) {
                // Neutral arm observation seam: a Failure cannot prove its real fence retired.
                return bloom::render::GpuResidentDisplayPollResult::Failure;
            }
            return display.poll();
        };
    core->residentGeneralDisplayPollOverride =
        [observeRetirement](bloom::runtime::GpuOcioDisplayArm& arm) {
            observeRetirement();
            return arm.poll(); // General arm uses the native unknown-fence seam after real submit.
        };
    return gate;
}

[[nodiscard]] bool
exerciseResolutionTransitions(bloom::ui::verticalproof::Expectations& checks,
                              bloom::ui::EditorArea& area, bloom::ui::ViewerEditor& viewer,
                              bloom::ui::CompositionSession& session,
                              bloom::ui::CompositionPreviewController& controller,
                              bloom::runtime::GpuPreviewDisplayService& service,
                              const std::shared_ptr<NativeTransitionGate>& gate,
                              const std::shared_ptr<bloom::ui::PreviewFrameTrace>& trace) {
    using namespace bloom;
    using namespace std::chrono_literals;
    auto* dropdown = area.findChild<ui::kit::KDropdown*>("viewerResolutionDropdown");
    checks.expect(dropdown != nullptr,
                  "resolution control is found under its real EditorArea owner");
    if (dropdown == nullptr)
        return false;
    const auto before = service.status().counters;
    // Starting Auto, visit Full/Half/Quarter/Auto. Each switch supersedes an actual submitted job.
    for (const int index : {1, 2, 3, 0}) {
        gate->submitted.store(false, std::memory_order_release);
        gate->armed.store(true, std::memory_order_release);
        const bool authored =
            session.setSelectedSolidColor(core::Color4d{0.10 + index * 0.10, 0.2, 0.3, 1.0});
        checks.expect(authored, "transition source edit uses the production command surface");
        controller.requestRefresh();
        const bool nativeStarted = ui::verticalproof::waitUntil(
            [&] { return gate->submitted.load(std::memory_order_acquire); }, 20s);
        checks.expect(nativeStarted, "resolution supersession follows a genuine native submission");
        if (!nativeStarted) {
            gate->armed.store(false, std::memory_order_release);
            return false;
        }
        dropdown->setCurrentIndex(index);
        const auto desired = controller.state().desiredIdentity;
        gate->armed.store(false, std::memory_order_release);
        checks.expect(desired && desired->resolutionPolicy ==
                                     static_cast<runtime::PreviewResolutionPolicy>(index),
                      "actual footer control changes the requested resolution policy");
        const bool recovered = ui::verticalproof::waitUntil(
            [&] {
                const auto& state = controller.state();
                const auto& ack = trace->lastNativeAcknowledgement();
                return state.desiredIdentity && state.frame && isResidentFrame(state.frame) &&
                       state.frame->desiredIdentity() == *state.desiredIdentity && ack &&
                       ack->identity == ui::PreviewFrameTrace::identity(*state.desiredIdentity) &&
                       state.activity == ui::PreviewActivity::Ready;
            },
            30s);
        checks.expect(recovered,
                      "cancelled resolution request recovers exact desired native identity");
        if (!recovered)
            return false;
        const auto appliedIdentity = controller.state().desiredIdentity;
        if (!appliedIdentity)
            return false;
        const auto geometry = ui::residentFrameGeometry(*controller.state().frame);
        const auto divisor = controller.resolutionDivisor();
        checks.expect(geometry &&
                          geometry->displayExtent.width() == (1920U + divisor - 1U) / divisor &&
                          geometry->displayExtent.height() == (1080U + divisor - 1U) / divisor,
                      "native output dimensions match the applied resolution divisor");
        std::cout << "TRANSITION policy=" << index << " divisor=" << divisor
                  << " generation=" << appliedIdentity->requestGeneration
                  << " submitted=1 recovered=" << recovered << '\n';
    }
    const auto after = service.status().counters;
    checks.expect(after.cpuFallbacks == before.cpuFallbacks,
                  "resolution cancellation never silently falls back to CPU");
    checks.expect(after.residentFailures == before.residentFailures,
                  "expected resolution cancellations are not backend failures");
    checks.expect(after.retirementUnprovenTeardowns == before.retirementUnprovenTeardowns,
                  "resolution recovery does not terminally abandon the resident route");
    checks.expect(viewer.residentPresentationActiveForTest(), "viewer remains natively resident");
    return true;
}

[[nodiscard]] bool
waitForTransitionIdentity(bloom::ui::CompositionPreviewController& controller,
                          const std::shared_ptr<bloom::ui::PreviewFrameTrace>& trace) {
    using namespace std::chrono_literals;
    return bloom::ui::verticalproof::waitUntil(
        [&] {
            const auto& state = controller.state();
            const auto& ack = trace->lastNativeAcknowledgement();
            return state.desiredIdentity && isResidentFrame(state.frame) &&
                   state.frame->desiredIdentity() == *state.desiredIdentity && ack &&
                   ack->identity ==
                       bloom::ui::PreviewFrameTrace::identity(*state.desiredIdentity) &&
                   state.activity == bloom::ui::PreviewActivity::Ready;
        },
        30s);
}

[[nodiscard]] bool exerciseConcurrentCancellationFailure(
    bloom::ui::verticalproof::Expectations& checks, bloom::ui::EditorArea& area,
    bloom::ui::CompositionSession& session, bloom::ui::CompositionPreviewController& controller,
    bloom::runtime::GpuPreviewDisplayService& service,
    const std::shared_ptr<NativeTransitionGate>& gate,
    const std::shared_ptr<bloom::ui::PreviewFrameTrace>& trace) {
    using namespace bloom;
    using namespace std::chrono_literals;
    const auto before = service.status().counters;
    gate->submitted.store(false, std::memory_order_release);
    gate->armed.store(true, std::memory_order_release);
    checks.expect(session.setSelectedSolidColor(core::Color4d{0.8, 0.2, 0.4, 1.0}),
                  "fault fixture authors an uncached source through commands");
    controller.requestRefresh();
    const bool submitted = ui::verticalproof::waitUntil(
        [&] { return gate->submitted.load(std::memory_order_acquire); }, 20s);
    auto* dropdown = area.findChild<ui::kit::KDropdown*>("viewerResolutionDropdown");
    if (!submitted || dropdown == nullptr) {
        gate->armed.store(false, std::memory_order_release);
        return false;
    }
    gate->failNextPoll.store(true, std::memory_order_release);
    dropdown->setCurrentIndex(1);
    checks.expect(ui::verticalproof::waitUntil(
                      [&] { return !gate->failNextPoll.load(std::memory_order_acquire); }, 2s),
                  "typed poll failure is observed on the cancelling owner stage");
    gate->armed.store(false, std::memory_order_release);
    const bool recovered = waitForTransitionIdentity(controller, trace);
    const auto after = service.status();
    checks.expect(after.counters.residentFailures == before.residentFailures + 1U,
                  "typed native poll failure still counts when cancellation races it");
    checks.expect(after.residentDetail.find(
                      "resident scene executor failed [poll=" +
                      std::to_string(static_cast<unsigned>(
                          runtime::GpuSceneExecutorPollResult::WrongThread))) != std::string::npos,
                  "concurrent cancellation preserves concrete unexpected failure diagnostics");
    checks.expect(after.counters.retirementUnprovenTeardowns ==
                          before.retirementUnprovenTeardowns &&
                      after.counters.cpuFallbacks == before.cpuFallbacks,
                  "concurrent fault retires exact native work then recovers without CPU fallback");
    return recovered;
}

[[nodiscard]] bool exerciseViewerReplacement(bloom::ui::verticalproof::Expectations& checks,
                                             bloom::ui::EditorArea& area,
                                             bloom::ui::ViewerEditor*& viewer,
                                             bloom::ui::CompositionSession& session,
                                             bloom::ui::CompositionPreviewController& controller,
                                             const std::shared_ptr<NativeTransitionGate>& gate) {
    using namespace bloom;
    using namespace std::chrono_literals;
    gate->submitted.store(false, std::memory_order_release);
    gate->armed.store(true, std::memory_order_release);
    checks.expect(session.setSelectedSolidColor(core::Color4d{0.6, 0.1, 0.8, 1.0}),
                  "replacement fixture authors an uncached source through commands");
    controller.requestRefresh();
    const bool submitted = ui::verticalproof::waitUntil(
        [&] { return gate->submitted.load(std::memory_order_acquire); }, 20s);
    if (!submitted) {
        gate->armed.store(false, std::memory_order_release);
        return false;
    }
    const bool accepted = area.setEditorId("bloom.properties");
    gate->armed.store(false, std::memory_order_release);
    const bool replaced = ui::verticalproof::waitUntil(
        [&] { return !area.isEditorChangePending() && area.editorId() == "bloom.properties"; },
        20s);
    checks.expect(accepted && replaced && area.lastNativeSurfaceDiagnostic().empty(),
                  "production EditorArea replacement waits for proven WSI retirement");
    // Do not retain a pointer to an outgoing editor that the production gate may have destroyed.
    viewer = area.findChild<ui::ViewerEditor*>();
    return replaced;
}

[[nodiscard]] bool exerciseInteractionResolution(
    bloom::ui::verticalproof::Expectations& checks, bloom::ui::EditorArea& area,
    bloom::ui::ViewerEditor& viewer, QWindow& window, QWidget& container, const QPointF centre,
    bloom::ui::CompositionSession& session, bloom::ui::CompositionPreviewController& controller,
    const std::shared_ptr<NativeTransitionGate>& gate,
    const std::shared_ptr<bloom::ui::PreviewFrameTrace>& trace) {
    using namespace bloom;
    using namespace std::chrono_literals;
    const auto revision = session.snapshot().revision();
    sendNativeMouse(window, container, QEvent::MouseButtonPress, centre, Qt::LeftButton,
                    Qt::LeftButton);
    gate->submitted.store(false, std::memory_order_release);
    gate->armed.store(true, std::memory_order_release);
    sendNativeMouse(window, container, QEvent::MouseMove, centre + QPointF(20.0, 10.0),
                    Qt::NoButton, Qt::LeftButton);
    checks.expect(session.transformInteractionActive(),
                  "resolution change begins during real native direct manipulation");
    controller.requestRefresh();
    const bool submitted = ui::verticalproof::waitUntil(
        [&] { return gate->submitted.load(std::memory_order_acquire); }, 20s);
    auto* dropdown = area.findChild<ui::kit::KDropdown*>("viewerResolutionDropdown");
    if (submitted && dropdown != nullptr)
        dropdown->setCurrentIndex(3);
    gate->armed.store(false, std::memory_order_release);
    const bool recovered =
        submitted && dropdown != nullptr && waitForTransitionIdentity(controller, trace);
    checks.expect(recovered, "active drag resolution change recovers exact native identity");
    // A real post-switch pointer event continues through the native input route. Escape then
    // explicitly chooses the documented no-command cancellation rather than guessing continuity.
    sendNativeMouse(window, container, QEvent::MouseMove, centre + QPointF(30.0, 15.0),
                    Qt::NoButton, Qt::LeftButton);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&viewer, &escape);
    sendNativeMouse(window, container, QEvent::MouseButtonRelease, centre, Qt::LeftButton,
                    Qt::NoButton);
    checks.expect(!session.transformInteractionActive() &&
                      session.snapshot().revision() == revision,
                  "Escape restores direct manipulation without a document command");
    const bool restored = waitForTransitionIdentity(controller, trace);
    checks.expect(restored, "cancelled interaction restores exact document-truth native identity");
    return recovered && restored;
}

[[nodiscard]] std::shared_ptr<const bloom::runtime::GpuDisplayProgramService>
generalDisplayPrograms() {
    const auto resolver = bloom::host::makePackagedGpuOcioResolver(
        std::filesystem::path(QCoreApplication::applicationFilePath().toStdString()));
    return resolver ? std::make_shared<const bloom::runtime::GpuDisplayProgramService>(resolver)
                    : nullptr;
}

[[nodiscard]] bool exerciseDisplayFailureRetirement(
    bloom::ui::verticalproof::Expectations& checks, bloom::ui::EditorArea& area,
    bloom::ui::CompositionSession& session, bloom::ui::CompositionPreviewController& controller,
    bloom::runtime::GpuPreviewDisplayService& service,
    const std::shared_ptr<NativeTransitionGate>& gate,
    const std::shared_ptr<bloom::ui::PreviewFrameTrace>& trace) {
    using namespace bloom;
    using namespace std::chrono_literals;
    const auto before = service.status().counters;
    gate->completionRetained.store(false, std::memory_order_release);
    gate->displayRetiring.store(false, std::memory_order_release);
    gate->displayArmed.store(true, std::memory_order_release);
    render::ocio_program_detail::setJobFenceUnknownForTest(true);
    checks.expect(session.setSelectedSolidColor(core::Color4d{0.25, 0.65, 0.35, 1.0}),
                  "display fault fixture submits uncached real document content");
    controller.requestRefresh();
    const bool retained = ui::verticalproof::waitUntil(
        [&] { return gate->displayRetiring.load(std::memory_order_acquire); }, 20s);
    checks.expect(retained && gate->completionRetained.load(std::memory_order_acquire),
                  "failed submitted display retains active stage and valid completion admission");
    auto* dropdown = area.findChild<ui::kit::KDropdown*>("viewerResolutionDropdown");
    if (dropdown != nullptr)
        dropdown->setCurrentIndex(2);
    render::ocio_program_detail::setJobFenceUnknownForTest(false);
    gate->displayArmed.store(false, std::memory_order_release);
    const bool recovered =
        retained && dropdown != nullptr && waitForTransitionIdentity(controller, trace);
    const auto after = service.status();
    checks.expect(after.counters.residentFailures == before.residentFailures + 1U,
                  "display failure is counted once across repeated retirement polls");
    checks.expect(after.counters.retirementUnprovenTeardowns ==
                          before.retirementUnprovenTeardowns &&
                      after.counters.cpuFallbacks == before.cpuFallbacks,
                  "failed display recovers after exact owner proof without terminal teardown or "
                  "CPU fallback");
    checks.expect(after.residentDetail.find("resident display failed") != std::string::npos,
                  "display failure diagnostics survive concurrent supersession");
    return recovered;
}

[[nodiscard]] bool armShutdownSubmission(bloom::ui::verticalproof::Expectations& checks,
                                         bloom::ui::CompositionSession& session,
                                         bloom::ui::CompositionPreviewController& controller,
                                         const std::shared_ptr<NativeTransitionGate>& gate) {
    using namespace std::chrono_literals;
    gate->submitted.store(false, std::memory_order_release);
    gate->armed.store(true, std::memory_order_release);
    checks.expect(session.addSolidLayer(QStringLiteral("Shutdown solid"),
                                        bloom::core::Color4d{0.15, 0.7, 0.2, 1.0}),
                  "shutdown fixture authors real uncached native work");
    controller.requestRefresh();
    const bool submitted = bloom::ui::verticalproof::waitUntil(
        [&] { return gate->submitted.load(std::memory_order_acquire); }, 20s);
    checks.expect(submitted, "controller shutdown begins with a genuine submitted job");
    return submitted;
}
