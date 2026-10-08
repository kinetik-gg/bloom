// Native resident viewer interaction, cancellation recovery and safe teardown acceptance.
#include "gpu_service_chain_fixture.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/gpu_viewer_bootstrap.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/preview_frame_trace.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QSettings>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "viewer_resident_interaction_support.hpp"
#include "viewer_resident_transition_support.hpp"

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    const HarnessOptions options = parseHarnessOptions(argc, argv);
    if (!options.valid || (options.baseline && options.trace.empty())) {
        return 2;
    }
    QApplication application(argc, argv);
    const QString platformName = QApplication::platformName();
    if (platformName != QStringLiteral("wayland") &&
        platformName != QStringLiteral("wayland-egl")) {
        return unavailable(options,
                           "resident interaction proof needs a real Wayland platform (platform=" +
                               platformName.toStdString() + ")");
    }
    if (options.loader.empty()) {
        return unavailable(options, "resident interaction proof needs --loader");
    }
    qputenv("QT_VULKAN_LIB", options.loader.string().c_str());

    const auto settingsRoot =
        std::filesystem::temp_directory_path() /
        ("bloom-resident-interaction-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(settingsRoot);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(settingsRoot.string()));
    QCoreApplication::setOrganizationName(QStringLiteral("BloomResidentInteraction"));
    QCoreApplication::setApplicationName(QStringLiteral("ResidentInteraction"));

    Expectations checks;
    const auto mediaDirectory =
        std::filesystem::temp_directory_path() /
        ("bloom-resident-interaction-media-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(mediaDirectory);
    Fixture fixture(mediaDirectory);
    if (fixture.processor == nullptr) {
        return unavailable(options, "the Bloom Neutral v1 processor is unavailable");
    }

    // A genuinely blank project, then the real first-composition command and the real Solid
    // authoring command. Nothing is seeded behind the session's back: every step goes through the
    // production transaction path the artist would use.
    auto newProject = document::makeNewProject("Resident Interaction");
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, document::CompositionId{});
    {
        commands::Transaction transaction("Create Composition", session.snapshot().revision());
        transaction.emplace<commands::AddComposition>("Main", format1920x1080NonSquare(),
                                                      core::RationalTime::fromInteger(10));
        const auto created = session.executeTransaction(std::move(transaction));
        const auto compositionId =
            created.outputId<document::CompositionId>(commands::kAddCompositionOutput);
        checks.expect(
            compositionId.has_value(),
            "the blank project gains its first composition through the production command");
        if (!compositionId.has_value()) {
            return 1;
        }
        checks.expect(session.setComposition(*compositionId),
                      "the session switches to the newly created composition");
    }
    checks.expect(session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
                  "the first Solid layer is created through the session command surface");

    if (options.baseline || options.transitions || options.shutdownInFlight) {
        const auto solidSelection = session.selection();
        checks.expect(
            session.addTextLayer(QStringLiteral("Baseline text"), QStringLiteral("Bloom G0"), 120.0,
                                 core::Color4d{1.0, 1.0, 1.0, 1.0}, document::Vec2d{600.0, 360.0}),
            "baseline text is authored");
        checks.expect(session.toggleKeyframe(document::kOpacityParameterRole),
                      "text opacity is animated");
        checks.expect(session.setCurrentTime(core::RationalTime::fromInteger(1)),
                      "animation end time");
        checks.expect(session.setSelectedOpacity(0.25), "text animation end key is authored");
        checks.expect(session.setCurrentTime(core::RationalTime::fromInteger(0)),
                      "animation returns to start");
        if (const auto* layer = std::get_if<document::LayerId>(&solidSelection.primary))
            session.selectLayer(*layer);
    }

    TaskScheduler scheduler((options.baseline || options.transitions || options.shutdownInFlight)
                                ? TaskSchedulerConfig::defaults()
                                : schedulerConfig());
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    auto gpuOptions = serviceOptions(options.loader);
    const auto frameCache = (options.baseline || options.transitions || options.shutdownInFlight)
                                ? std::make_shared<ui::PreviewFrameCache>()
                                : nullptr;
    if (options.baseline || options.transitions || options.shutdownInFlight) {
        // Same default budget alignment as the desktop root, with no saved preference overrides.
        gpuOptions = GpuPreviewDisplayServiceOptions{};
        gpuOptions.enabled = true;
        gpuOptions.loaderPath = options.loader;
        gpuOptions.presentation = runtime::GpuPreviewDisplayServicePresentationMode::Wayland;
        const auto budgetPlan = ui::gpuResidentBudgetPlanFor(frameCache->byteBudget());
        ui::applyGpuResidentBudgetPlan(gpuOptions, budgetPlan);
        ui::applyGpuResidentCacheLimits(*frameCache, budgetPlan);
    }
    const auto generalPrograms = options.general ? generalDisplayPrograms() : nullptr;
    if (options.general && !generalPrograms)
        return unavailable(options, "packaged GPU tools unavailable");
    const auto gpuStage =
        options.general
            ? ui::makeCompositionPreviewGpuSceneStage(fixture.compiler, *fixture.sceneBuilder,
                                                      fixture.provider, nullptr, generalPrograms)
            : fixture.productionGpuStage();
    GpuPreviewDisplayService service(scheduler, gpuStage, fixture.productionCpuStage(),
                                     fixture.productionCpuFallback(displayPreparer), gpuOptions);

    const bool terminal = waitUntil(
        [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; }, 90s);
    const auto status = service.status();
    const bool residentReady =
        terminal && status.state == GpuPreviewDisplayServiceState::Ready &&
        status.residentQualification != nullptr && status.residentQualification->eligible() &&
        status.presentationClient != nullptr &&
        status.presentationAvailability == render::GpuPresentationAvailability::Ready;
    if (!residentReady) {
        service.beginShutdown();
        return unavailable(options, "viewer resident route unavailable: " + status.residentDetail +
                                        " / " + status.presentationDetail);
    }
    const std::shared_ptr<GpuPresentationClient> client = status.presentationClient;
    const auto afterReady = status.counters;
    const auto core = runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    checks.expect(core != nullptr, "the service core is reachable");
    const auto epoch =
        core == nullptr
            ? 0U
            : runtime::detail::GpuPreviewDisplayServiceTestAccess::ownershipEpoch(*core);
    checks.expect(epoch > 0U, "the service publishes a genuine nonzero device ownership epoch");

    const auto transitionGate =
        (options.transitions || options.shutdownInFlight) ? installTransitionGate(core) : nullptr;
    ui::TaskUiBridge bridge(scheduler, nullptr,
                            (options.baseline || options.transitions || options.shutdownInFlight)
                                ? ui::TaskUiBridge::defaultPollingInterval
                                : 1ms);
    auto pipeline = ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                                       displayPreparer, fixture.provider);
    ui::CompositionPreviewSettings settings;
    if (!options.baseline)
        settings.pixelStorageByteLimit = kBudget;
    if (options.baseline) {
        // Explicit initial user preference in this fixture's isolated settings directory. The
        // Viewer restores it through its production preference path, so no initial Auto->Full
        // cancellation race is smuggled into a fixed-resolution timing sample.
        QSettings().setValue(QStringLiteral("viewer/resolution"), QStringLiteral("Full"));
        settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
    }
    auto trace = (options.baseline || options.transitions || options.shutdownInFlight)
                     ? std::make_shared<ui::PreviewFrameTrace>()
                     : nullptr;
    if (trace)
        trace->setPhase(ui::PreviewTracePhase::Cold);
    ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snap,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides)
        -> runtime::TaskSubmission<runtime::PreviewPreparationResultHandle> {
        return service.submit(std::move(request), snap, identity, limit, overrides);
    };
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                frameCache, nullptr, submitter, trace);

    // The production editor registry + EditorArea host the viewer and inject the live presentation
    // client exactly as the application does. EditorArea's event filter is what turns a click into
    // the blue active-panel activation, so its absence here is a real observable regression.
    ui::ViewerGpuDependencies gpuDependencies;
    gpuDependencies.presentationClient = [client]() -> std::shared_ptr<GpuPresentationClient> {
        return std::shared_ptr<GpuPresentationClient>(client);
    };
    gpuDependencies.scheduler = &scheduler;
    gpuDependencies.vulkanLoaderPath = options.loader.string();
    gpuDependencies.devicePixelRatio = 1.0;
    ui::EditorRegistry registry;
    checks.expect(ui::registerFoundationEditors(registry, session, controller, nullptr, nullptr,
                                                &gpuDependencies),
                  "the production editor registry registers the viewer with GPU dependencies");

    auto host = std::make_unique<QWidget>();
    host->setWindowTitle(QString::fromStdString(
        "BloomResidentInteraction-" + std::to_string(QCoreApplication::applicationPid())));
    host->resize(960, 640);
    auto* layout = new QVBoxLayout(host.get());
    layout->setContentsMargins(0, 0, 0, 0);
    auto* area =
        new ui::EditorArea(registry, "bloom.viewer", QStringLiteral("proof-viewer"), host.get());
    layout->addWidget(area);

    std::size_t activations = 0;
    QObject::connect(area, &ui::EditorArea::activationRequested, area,
                     [&activations](ui::EditorArea* requested) {
                         requested->setAreaActive(true);
                         ++activations;
                     });
    host->show();
    ui::ViewerEditor* viewer = area->findChild<ui::ViewerEditor*>();
    QSize nativeViewportSize;
    const int runResult = [&]() -> int {
        checks.expect(viewer != nullptr, "the EditorArea hosts the real ViewerEditor");
        if (viewer == nullptr) {
            return 1;
        }
        viewer->show();
        QApplication::processEvents();
        if (!options.baseline)
            controller.requestRefresh();

        const auto compositionFormat = session.composition()->format();
        const bool firstResident =
            options.baseline
                ? waitUntil(
                      [&] {
                          const auto& state = controller.state();
                          const auto& ack = trace->lastNativeAcknowledgement();
                          return state.desiredIdentity && state.frame &&
                                 isResidentFrame(state.frame) &&
                                 std::holds_alternative<runtime::CompositionFormatResolution>(
                                     state.desiredIdentity->resolution) &&
                                 state.desiredIdentity->resolutionPolicy ==
                                     runtime::PreviewResolutionPolicy::Full &&
                                 state.frame->desiredIdentity() == *state.desiredIdentity && ack &&
                                 ack->identity ==
                                     ui::PreviewFrameTrace::identity(*state.desiredIdentity);
                      },
                      30s)
                : waitForResidentRevision(controller, *viewer, session.snapshot().revision(), 30s);
        const auto displayed = controller.state().frame;
        checks.expect(firstResident && displayed != nullptr,
                      "the controller produced and the viewer presented a resident frame");
        checks.expect(isResidentFrame(displayed),
                      "the displayed frame is the genuine GPU-resident arm");
        checks.expect(displayed != nullptr && !displayed->displayBufferView().has_value(),
                      "the genuine resident frame carries NO CPU display buffer view");
        checks.expect(viewer->residentPresentationActiveForTest(),
                      "the real ViewerEditor genuinely presented the resident frame");
        if (!firstResident || displayed == nullptr || displayed->displayBufferView().has_value()) {
            std::cerr << "PHASE resident-frame-failed frame=" << (displayed != nullptr ? 1 : 0)
                      << " residentDetail=" << service.status().residentDetail
                      << " presentationDetail=" << service.status().presentationDetail << '\n';
            return 1;
        }
        if (options.failAfterPresent) {
            checks.expect(false, "intentional assertion failure after genuine native presentation");
            return 1;
        }
        const auto afterCold = service.status().counters;
        checks.expect(afterCold.nativeDispatches > afterReady.nativeDispatches,
                      "the cold resident viewer frame performed real native dispatches");
        checks.expect(afterCold.fullFrameReadbacks == 0U,
                      "the resident viewer frame performed no full-frame readback");
        const auto geometry = ui::residentFrameGeometry(*displayed);
        checks.expect(geometry.has_value() && geometry->lease.isValid(),
                      "the displayed frame carries a valid genuine native lease");

        if (options.baseline) {
            checks.expect(geometry && geometry->displayExtent.width() == 1920 &&
                              geometry->displayExtent.height() == 1080,
                          "the baseline evaluates at full 1920x1080 (not an Auto proxy)");
            const auto waitForTracedFrame = [&](const core::RationalTime expectedTime) {
                return waitUntil(
                    [&] {
                        const auto& desired = controller.state().desiredIdentity;
                        const auto& ack = trace->lastNativeAcknowledgement();
                        return desired && desired->time == expectedTime && ack &&
                               ack->identity.generation == desired->requestGeneration &&
                               ack->identity == ui::PreviewFrameTrace::identity(*desired);
                    },
                    30s);
            };
            checks.expect(waitForTracedFrame(core::RationalTime::fromInteger(0)),
                          "the first cold frame has its own native owner acknowledgement");
            for (const auto phase : {ui::PreviewTracePhase::Cold, ui::PreviewTracePhase::Warm}) {
                trace->setPhase(phase);
                for (int index = phase == ui::PreviewTracePhase::Cold ? 1 : 0; index < 4; ++index) {
                    const auto time = core::RationalTime::create(index, 24);
                    checks.expect(time && session.setCurrentTime(*time),
                                  "baseline advances exact animation time");
                    checks.expect(
                        time && waitForTracedFrame(*time),
                        "each time sample has an identity-matched native acknowledgement");
                }
            }
            // Gesture begins at a settled frame, and remains the existing native QWindow input
            // path.
            trace->setPhase(ui::PreviewTracePhase::Manipulation);
        }

        if (options.transitions &&
            !exerciseResolutionTransitions(checks, *area, *viewer, session, controller, service,
                                           transitionGate, trace))
            return 1;

        const QRectF displayRect = viewerDisplayRect(*viewer, compositionFormat);
        checks.expect(!displayRect.isEmpty(),
                      "the resident composition has a non-empty display rect");

        const auto* positionParameter =
            session.parameterForSelection(document::kPositionParameterRole);
        checks.expect(positionParameter != nullptr, "the Solid exposes a position parameter");
        if (positionParameter == nullptr) {
            return 1;
        }
        const auto positionId = positionParameter->id;
        const auto base = session.constantVec2Value(positionId);
        checks.expect(base.has_value(), "the Solid position starts as a resolvable constant");
        if (!base.has_value()) {
            return 1;
        }

        // Clear the creation selection so the ONLY way the layer gets selected is the native
        // viewer's own mouse hit-test against the resident frame.
        session.clearSelection();
        checks.expect(session.selectedNodes().empty(),
                      "nothing is selected before the viewer click");

        QWidget* container = nullptr;
        const bool containerReady = waitUntil(
            [&] {
                container = nativeContainer(*viewer);
                return container != nullptr && container->isVisible() &&
                       !container->geometry().isEmpty();
            },
            15s);
        checks.expect(containerReady,
                      "the presenter's real native GPU container exists and is visible");
        if (!containerReady || container == nullptr) {
            return 1;
        }
        nativeViewportSize = container->size();
        QWindow* nativeWindow = nativeVulkanWindow(container, host.get());
        checks.expect(nativeWindow != nullptr &&
                          nativeWindow->surfaceType() == QSurface::VulkanSurface,
                      "the presenter owns a real visible Vulkan QWindow");
        if (nativeWindow == nullptr) {
            return 1;
        }

        const QPointF centre = displayRect.center();
        const auto countersBeforeGesture = service.status().counters;
        const auto revisionBeforeSelect = session.snapshot().revision();

        // Native hit mapping / selection through the real Vulkan QWindow. The viewer is focused
        // BEFORE the click so the pre-fix handler's setFocus() is a no-op: the only thing that can
        // activate the EditorArea is the mouse press itself reaching the receiver's event filters,
        // which the direct-handler bypass never did.
        viewer->setFocus(Qt::OtherFocusReason);
        QApplication::processEvents();
        area->setAreaActive(false);
        activations = 0;
        sendNativeMouse(*nativeWindow, *container, QEvent::MouseButtonPress, centre, Qt::LeftButton,
                        Qt::LeftButton);
        checks.expect(
            session.transformInteractionActive(),
            "a native click on the resident layer begins the direct-manipulation interaction");
        document::LayerId selectedLayer;
        bool hasSelectedLayer = false;
        if (const auto* primary = std::get_if<document::LayerId>(&session.selection().primary)) {
            selectedLayer = *primary;
            hasSelectedLayer = true;
        }
        checks.expect(hasSelectedLayer,
                      "the native click selects the layer through viewer hit mapping");
        checks.expect(
            !controller.selectedLayerBounds().empty(),
            "the selected layer's evaluated bounds are available for the selection overlay");
        checks.expect(
            activations > 0U && area->isAreaActive(),
            "the native mouse press reaches the EditorArea event filter and activates the panel");
        sendNativeMouse(*nativeWindow, *container, QEvent::MouseButtonRelease, centre,
                        Qt::LeftButton, Qt::NoButton);
        checks.expect(!session.transformInteractionActive(),
                      "the selection click ends its interaction");
        checks.expect(session.snapshot().revision() == revisionBeforeSelect,
                      "the selection click commits no document transaction");

        // The recorded resident overlay must carry the composition frame border (the CPU paint's
        // own composition boundary) and the selection overlay for the selected layer.
        {
            const OverlayProbe probe = probeResidentOverlay(*viewer);
            checks.expect(probe.valid, "the resident present request recorded a real overlay");
            checks.expect(probe.framePixels() >= 4U,
                          "the resident overlay draws the composition frame border");
        }

        if (options.transitions &&
            !exerciseInteractionResolution(checks, *area, *viewer, *nativeWindow, *container,
                                           centre, session, controller, transitionGate, trace))
            return 1;

        // Native drag: press, move, release. Exactly one undoable commit.
        const auto revisionBeforeDrag = session.snapshot().revision();
        const QPointF release = centre + QPointF(60.0, -40.0);
        const QPointF delta = release - centre;
        sendNativeMouse(*nativeWindow, *container, QEvent::MouseButtonPress, centre, Qt::LeftButton,
                        Qt::LeftButton);
        checks.expect(session.transformInteractionActive(),
                      "the native drag press begins the interaction");
        sendNativeMouse(*nativeWindow, *container, QEvent::MouseMove, release, Qt::NoButton,
                        Qt::LeftButton);
        checks.expect(session.transformInteractionActive(),
                      "the native drag move survives the mid-gesture mapping revalidation");
        sendNativeMouse(*nativeWindow, *container, QEvent::MouseButtonRelease, release,
                        Qt::LeftButton, Qt::NoButton);
        checks.expect(!session.transformInteractionActive(), "release ends the interaction");
        checks.expect(session.snapshot().revision().value() == revisionBeforeDrag.value() + 1,
                      "release commits exactly one document transaction -- one undo step");

        const document::Vec2d expected{base->x + delta.x() / displayRect.width() * 1920.0,
                                       base->y + delta.y() / displayRect.height() * 1080.0};
        checks.expect(session.constantVec2Value(positionId) == expected,
                      "the committed position is the base plus the mapped pointer displacement");

        checks.expect(session.undo(), "the drag's single commit undoes cleanly");
        checks.expect(session.constantVec2Value(positionId) == *base,
                      "undo restores the exact pre-drag position");
        checks.expect(
            waitForResidentRevision(controller, *viewer, session.snapshot().revision(), 30s),
            "the resident frame catches up to the undone revision");

        const auto countersAfterGesture = service.status().counters;
        checks.expect(countersAfterGesture.fullFrameReadbacks == 0U,
                      "the whole gesture performed no full-frame readback");
        checks.expect(countersAfterGesture.cpuFallbacks == countersBeforeGesture.cpuFallbacks,
                      "the whole gesture forced no CPU fallback");
        checks.expect(viewer->residentPresentationActiveForTest(),
                      "the viewer still presents the genuine resident arm after the gesture");

        if (!options.baseline && !options.transitions && !options.shutdownInFlight) {
            // A second, DISTINCT resident lease: change the Solid colour so the requested content
            // genuinely changes. LIMIT, stated plainly: this is a live authoring edit producing a
            // new revision and a new resident lease -- it is NOT a sequence-playback proof (no time
            // is advanced). The genuine native proof below is the owner-published applied
            // sequence/present count, never the mailbox admission counters. The CPU oracle proves
            // the two requests differ; a native readback is forbidden, so this does NOT claim the
            // presented pixels equal the oracle.
            const auto beforeFrame = controller.state().frame;
            const auto beforeSnapshot = session.snapshot();
            const auto beforeIdentity = beforeFrame != nullptr ? beforeFrame->desiredIdentity()
                                                               : runtime::PreviewRequestIdentity{};
            const auto beforeLease =
                beforeFrame != nullptr && ui::residentFrameGeometry(*beforeFrame).has_value()
                    ? ui::residentFrameGeometry(*beforeFrame)->lease
                    : runtime::GpuResidentFrameLease{};
            const std::uint64_t appliedBefore = viewer->gpuNativeAppliedSequenceForTest();
            const std::uint64_t enqueuedBefore = viewer->gpuNativeLastEnqueuedSequenceForTest();
            const std::uint64_t presentCountBefore = viewer->gpuNativePresentCountForTest();
            if (hasSelectedLayer) {
                session.selectLayer(selectedLayer);
            }
            checks.expect(session.setSelectedSolidColor(core::Color4d{0.95, 0.1, 0.85, 1.0}),
                          "the Solid colour change is one document transaction");
            checks.expect(
                waitForResidentRevision(controller, *viewer, session.snapshot().revision(), 30s),
                "a second distinct resident frame reaches the viewer");
            const auto second = controller.state().frame;
            checks.expect(isResidentFrame(second) && second != nullptr &&
                              !second->displayBufferView().has_value(),
                          "the second frame is a genuine CPU-buffer-free resident frame");
            // Genuine native present progress: the owner must have applied a NEW enqueued present
            // sequence (applied >= enqueued) and advanced its applied present count. The mailbox
            // admission counters are deliberately not used here: admission is not a native present.
            const bool secondGenuinelyPresented = waitUntil(
                [&] {
                    const std::uint64_t enqueued = viewer->gpuNativeLastEnqueuedSequenceForTest();
                    const std::uint64_t applied = viewer->gpuNativeAppliedSequenceForTest();
                    return enqueued > enqueuedBefore && applied >= enqueued &&
                           viewer->gpuNativePresentCountForTest() > presentCountBefore;
                },
                20s);
            checks.expect(
                secondGenuinelyPresented,
                "the second resident lease was genuinely applied natively (applied >= enqueued "
                "and the owner present count advanced)");
            const std::uint64_t enqueuedAfter = viewer->gpuNativeLastEnqueuedSequenceForTest();
            const std::uint64_t appliedAfter = viewer->gpuNativeAppliedSequenceForTest();
            checks.expect(enqueuedAfter > enqueuedBefore,
                          "a new native present sequence was enqueued for the second frame");
            checks.expect(appliedAfter >= enqueuedAfter,
                          "the owner applied the second frame's enqueued present sequence");
            checks.expect(viewer->gpuNativePresentCountForTest() > presentCountBefore,
                          "the owner's applied present count advanced for the second frame");
            checks.expect(appliedAfter > appliedBefore,
                          "the owner's applied sequence advanced past the first frame");
            if (second != nullptr) {
                const auto secondLease = ui::residentFrameGeometry(*second);
                checks.expect(secondLease.has_value() && secondLease->lease.isValid(),
                              "the second resident frame carries a valid genuine native lease");
                checks.expect(secondLease.has_value() &&
                                  secondLease->lease.id() != beforeLease.id(),
                              "the second frame is a NEW native lease, not a reused/frozen one");
                checks.expect(second->desiredIdentity() != beforeIdentity,
                              "the second frame's request identity actually changed");
            }
            // Online CPU oracle: the two requested contents differ, so a frozen re-presentation of
            // the first is impossible.
            {
                const auto firstOracle =
                    snapshotCpuReference(fixture, beforeSnapshot, beforeIdentity);
                const auto secondOracle = second != nullptr
                                              ? snapshotCpuReference(fixture, session.snapshot(),
                                                                     second->desiredIdentity())
                                              : nullptr;
                checks.expect(
                    firstOracle != nullptr && secondOracle != nullptr &&
                        !samePixels(firstOracle, secondOracle),
                    "the two resident requests resolve to genuinely different CPU content");
                checks.expect(firstOracle != nullptr &&
                                  firstOracle->displayBufferView().has_value(),
                              "the CPU oracle for the first frame carries real pixels");
            }
            checks.expect(service.status().counters.fullFrameReadbacks == 0U,
                          "the second resident frame performed no full-frame readback");

            // Delete the final layer through the viewer's own Delete key route: the composition
            // stays valid with empty geometry, and its empty-state invitation is part of the
            // resident overlay.
            if (hasSelectedLayer) {
                session.selectLayer(selectedLayer);
            }
            const auto revisionBeforeDelete = session.snapshot().revision();
            sendDeleteKey(*viewer);
            checks.expect(session.timelineMerge()->entries().empty(),
                          "Delete removes the final layer through the viewer route");
            checks.expect(session.snapshot().revision().value() == revisionBeforeDelete.value() + 1,
                          "the layer delete is one document transaction");
            checks.expect(session.composition() != nullptr,
                          "the composition itself survives deleting its final layer");
            checks.expect(session.composition()->graph().layerOutputs().empty(),
                          "the composition now has empty layer geometry");
            checks.expect(
                waitForResidentRevision(controller, *viewer, session.snapshot().revision(), 30s),
                "a resident frame is produced for the empty composition revision");
            {
                const ui::ResidentPresentRequest request =
                    viewer->buildResidentPresentRequestForTest();
                checks.expect(!request.destination.isEmpty(),
                              "the empty composition still yields valid display geometry");
                const OverlayProbe probe = probeResidentOverlay(*viewer);
                checks.expect(probe.valid, "the empty composition records a resident overlay");
                checks.expect(probe.framePixels() >= 4U,
                              "the empty composition still draws its composition frame border");
                checks.expect(
                    probe.centreInkPixels() >= 10U,
                    "the empty composition records the 'Create a layer to begin' invitation");
            }
        }
        if (options.transitions) {
            checks.expect(exerciseConcurrentCancellationFailure(checks, *area, session, controller,
                                                                service, transitionGate, trace),
                          "concurrent cancellation preserves native fault diagnostics");
            checks.expect(exerciseDisplayFailureRetirement(checks, *area, session, controller,
                                                           service, transitionGate, trace),
                          "failed submitted display retires safely and recovers");
            checks.expect(exerciseViewerReplacement(checks, *area, viewer, session, controller,
                                                    transitionGate),
                          "native viewer replacement preserves submitted-work ownership");
        }
        if (options.shutdownInFlight &&
            !armShutdownSubmission(checks, session, controller, transitionGate))
            return 1;
        return 0;
    }();
    // Every ordinary success or assertion failure takes this same proven retirement path.
    controller.beginShutdown();
    if (transitionGate)
        transitionGate->armed.store(false, std::memory_order_release);
    // Retire the native surface before teardown, exactly like the production host.
    bool viewerRetired = false;
    if (viewer != nullptr)
        static_cast<void>(viewer->prepareNativeSurfaceMutation(
            91, [&](const std::uint64_t generation,
                    const ui::EditorNativeSurface::PrepareResult& result) {
                checks.expect(generation == 91, "the viewer mutation generation is echoed");
                viewerRetired = result.safeToMutate;
            }));
    if (viewer == nullptr)
        viewerRetired = true;
    const bool safe = waitUntil([&] { return viewerRetired; }, 20s);
    checks.expect(safe, "the ViewerEditor native surface retired before teardown");
    if (!safe) {
        // Keep ALL stack and widget owners alive. Running RAII destructors would invalidate the
        // unproved WSI target. This bounded fatal outcome is never used for assertion failures.
        std::cerr << "FATAL: unproved WSI retirement; retaining entire fixture until process exit\n"
                  << std::flush;
        std::_Exit(1);
    }
    if (viewer != nullptr)
        viewer->setGpuPresentationDependencies(nullptr, nullptr, std::string{}, 1.0);
    host.reset();

    bridge.beginShutdown();
    checks.expect(waitUntil([&] { return scheduler.isQuiescent(); }, 10s),
                  "all controller tasks quiesce before service shutdown");
    service.beginShutdown();
    checks.expect(
        waitUntil([&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; },
                  30s),
        "the native service completes bounded safe shutdown");
    checks.expect(service.status().counters.fullFrameReadbacks == 0U,
                  "the whole resident interaction run performed no full-frame readback");
    std::error_code ignored;
    std::filesystem::remove_all(mediaDirectory, ignored);
    std::filesystem::remove_all(settingsRoot, ignored);

    if (trace && !options.trace.empty()) {
        verifyAndExportTrace(checks, trace, options.trace);
        std::cout << (options.baseline ? "BASELINE" : "TRANSITIONS")
                  << " output_format=1920x1080 par=4/3 requested_host=960x640 native_viewport="
                  << nativeViewportSize.width() << 'x' << nativeViewportSize.height()
                  << " device_pixel_ratio=1 bridge_poll_ms=" << bridge.pollingInterval().count()
                  << " foreground_poll_ms=4 scheduler=defaults resolution_policy="
                  << static_cast<unsigned>(controller.settings().resolutionPolicy) << " validation="
                  << (qEnvironmentVariable("VK_INSTANCE_LAYERS").contains("validation") ? 1 : 0)
                  << " request_byte_limit=" << settings.pixelStorageByteLimit
                  << " cache_byte_budget=" << frameCache->byteBudget()
                  << " native_dispatches=" << service.status().counters.nativeDispatches
                  << " gpu_cache_hits=" << service.status().counters.gpuCacheHits
                  << " gpu_cache_misses=" << service.status().counters.gpuCacheMisses
                  << " cpu_fallbacks=" << service.status().counters.cpuFallbacks
                  << " full_frame_readbacks=" << service.status().counters.fullFrameReadbacks
                  << '\n';
    }

    if (runResult != 0 || checks.failures() != 0) {
        std::cerr << "resident interaction proof: " << checks.failures()
                  << " verification failure(s)\n";
        return 1;
    }
    std::cout << "PASS: resident interaction proof (epoch=" << epoch
              << ", nativeDispatches=" << service.status().counters.nativeDispatches
              << ", activations=" << activations << ")\n";
    return 0;
}
