// Bounded native acceptance for the viewer's GPU-resident surface across a DOCUMENT REPLACEMENT
// (File > Open / New) performed while the real ViewerEditor already has a live native target.
//
// The reported live defect: blank -> create composition -> Open an existing project, and the native
// window is left misplaced while a stale CPU checkerboard occludes the viewport, with native clicks
// no longer activating the panel. This test reproduces that lifecycle through the real route:
//
//   * real blank project -> real first-composition/Solid commands -> real service resident frame,
//   * a real ViewerEditor inside a real EditorArea with a real Vulkan QWindow,
//   * then CompositionSession::rebind() to a SECOND project exactly as apps/bloom/main.cpp does on
//     ProjectHost::sessionReplaced (Open), plus a viewer resize that exercises the cover show/hide
//     path,
//   * and asserts genuine owner-observed native present progress for the new frame, that the native
//     CPU cover is hidden after the acknowledgement, that the native container stays inside the
//     viewer, and that native input still activates the EditorArea before and after the swap.
//
// No full-frame readback, no CPU fallback, no mailbox-admission-only proof. Native-only: skips with
// 77 without a Wayland platform, a packaged loader, or a resident-capable device.

#include "gpu_service_chain_fixture.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QPointF>
#include <QSettings>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace bloom;
using namespace bloom::ui::verticalproof;

struct HarnessOptions final {
    std::filesystem::path loader;
    bool requireDevice = false;
    bool valid = true;
};

[[nodiscard]] HarnessOptions parseHarnessOptions(const int argc, char** argv) {
    HarnessOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            options.loader = argv[++index];
        } else if (argument == "--require-device") {
            options.requireDevice = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] int unavailable(const HarnessOptions& options, const std::string& reason) {
    std::cout << "SKIP: " << reason << '\n';
    return options.requireDevice ? 1 : 77;
}

[[nodiscard]] bool isResidentFrame(const ui::PreparedPreviewFrameHandle& frame) {
    return frame != nullptr &&
           frame->provenance().provider == runtime::PreviewDisplayProvider::GpuResident;
}

[[nodiscard]] QWidget* nativeContainer(ui::ViewerEditor& viewer) {
    return viewer.gpuNativeContainerForTest();
}

[[nodiscard]] QWindow* nativeVulkanWindow(QWidget* container, QWidget* host) {
    QWindow* handle = container->windowHandle();
    for (QWindow* candidate : QGuiApplication::allWindows()) {
        if (!candidate->isVisible() || candidate->surfaceType() != QSurface::VulkanSurface) {
            continue;
        }
        if (handle != nullptr && candidate == handle) {
            return candidate;
        }
        if (host != nullptr) {
            for (QWindow* ancestor = candidate->parent(); ancestor != nullptr;
                 ancestor = ancestor->parent()) {
                if (ancestor == host->windowHandle()) {
                    return candidate;
                }
            }
        }
    }
    return handle;
}

void sendNativeMouse(QWindow& window, QWidget& container, const QEvent::Type type,
                     const QPointF& viewerLocal, const Qt::MouseButton button,
                     const Qt::MouseButtons buttons) {
    const QPointF local = viewerLocal - QPointF(container.pos());
    const QPointF global(container.mapToGlobal(local.toPoint()));
    QMouseEvent event(type, local, local, global, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &event);
    QCoreApplication::processEvents();
}

// Adds one composition to a blank project through the production transaction path, exactly as the
// real New Composition command does, and returns the activated session to it.
[[nodiscard]] bool seedComposition(ui::CompositionSession& session, const std::string& name,
                                   const document::CompositionFormat format) {
    commands::Transaction transaction("Create Composition", session.snapshot().revision());
    transaction.emplace<commands::AddComposition>(name, format,
                                                  core::RationalTime::fromInteger(10));
    const auto created = session.executeTransaction(std::move(transaction));
    const auto id = created.outputId<document::CompositionId>(commands::kAddCompositionOutput);
    return id.has_value() && session.setComposition(*id);
}

struct SecondProject final {
    document::Document document;
    commands::CommandStack commands;
    document::CompositionId compositionId;

    explicit SecondProject(document::NewProject project)
        : document(std::move(project.project)), commands(document),
          compositionId(project.initialCompositionId) {}
};

[[nodiscard]] bool waitForResidentComposition(ui::CompositionPreviewController& controller,
                                              ui::ViewerEditor& viewer,
                                              const document::CompositionId compositionId,
                                              const std::chrono::milliseconds timeout) {
    return waitUntil(
        [&] {
            const auto frame = controller.state().frame;
            return isResidentFrame(frame) && viewer.residentPresentationActiveForTest() &&
                   frame->desiredIdentity().compositionId == compositionId &&
                   controller.state().activity == ui::PreviewActivity::Ready;
        },
        timeout);
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    const HarnessOptions options = parseHarnessOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    QApplication application(argc, argv);
    const QString platformName = QApplication::platformName();
    if (platformName != QStringLiteral("wayland") &&
        platformName != QStringLiteral("wayland-egl")) {
        return unavailable(options, "project-swap proof needs a real Wayland platform (platform=" +
                                        platformName.toStdString() + ")");
    }
    if (options.loader.empty()) {
        return unavailable(options, "project-swap proof needs --loader");
    }
    qputenv("QT_VULKAN_LIB", options.loader.string().c_str());

    const auto settingsRoot =
        std::filesystem::temp_directory_path() /
        ("bloom-resident-swap-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(settingsRoot);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(settingsRoot.string()));
    QCoreApplication::setOrganizationName(QStringLiteral("BloomResidentSwap"));
    QCoreApplication::setApplicationName(QStringLiteral("ResidentSwap"));

    Expectations checks;
    const auto mediaDirectory =
        std::filesystem::temp_directory_path() /
        ("bloom-resident-swap-media-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(mediaDirectory);
    Fixture fixture(mediaDirectory);
    if (fixture.processor == nullptr) {
        return unavailable(options, "the Bloom Neutral v1 processor is unavailable");
    }

    auto newProject = document::makeNewProject("Resident Swap");
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, document::CompositionId{});
    checks.expect(seedComposition(session, "First", format1920x1080NonSquare()),
                  "the blank project gains its first composition through the production command");
    checks.expect(session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
                  "the first Solid is authored through the session command surface");

    TaskScheduler scheduler(schedulerConfig());
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    GpuPreviewDisplayService service(
        scheduler, fixture.productionGpuStage(), fixture.productionCpuStage(),
        fixture.productionCpuFallback(displayPreparer), serviceOptions(options.loader));

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

    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds(1));
    auto pipeline = ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                                       displayPreparer, fixture.provider);
    ui::CompositionPreviewSettings settings;
    settings.pixelStorageByteLimit = kBudget;
    ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snap,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides)
        -> runtime::TaskSubmission<runtime::PreviewPreparationResultHandle> {
        return service.submit(std::move(request), snap, identity, limit, overrides);
    };
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                nullptr, nullptr, submitter);

    ui::ViewerGpuDependencies gpuDependencies;
    gpuDependencies.presentationClient = [client]() -> std::shared_ptr<GpuPresentationClient> {
        return client;
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
        "BloomResidentSwap-" + std::to_string(QCoreApplication::applicationPid())));
    host->resize(960, 640);
    auto* layout = new QVBoxLayout(host.get());
    layout->setContentsMargins(0, 0, 0, 0);
    auto* area =
        new ui::EditorArea(registry, "bloom.viewer", QStringLiteral("swap-viewer"), host.get());
    layout->addWidget(area);

    std::size_t activations = 0;
    QObject::connect(area, &ui::EditorArea::activationRequested, area,
                     [&activations](ui::EditorArea* requested) {
                         requested->setAreaActive(true);
                         ++activations;
                     });
    host->show();
    ui::ViewerEditor* viewer = area->findChild<ui::ViewerEditor*>();
    checks.expect(viewer != nullptr, "the EditorArea hosts the real ViewerEditor");
    if (viewer == nullptr) {
        return 1;
    }
    viewer->show();
    QApplication::processEvents();
    controller.requestRefresh();

    const auto firstCompositionId = session.compositionId();
    const bool firstResident =
        waitForResidentComposition(controller, *viewer, firstCompositionId, 30s);
    checks.expect(firstResident, "the viewer presented the first project's resident frame");
    if (!firstResident) {
        viewer->setGpuPresentationDependencies(nullptr, nullptr, std::string{}, 1.0);
        return 1;
    }
    const bool firstAckHidden =
        waitUntil([&] { return !viewer->gpuCpuCoverVisibleForTest(); }, 15s);
    checks.expect(firstAckHidden, "the native CPU cover is hidden after the first present ack");

    QWidget* container = nullptr;
    const bool containerReady = waitUntil(
        [&] {
            container = nativeContainer(*viewer);
            return container != nullptr && container->isVisible() &&
                   !container->geometry().isEmpty();
        },
        15s);
    checks.expect(containerReady,
                  "the presenter's native container is visible after the first ack");
    if (!containerReady || container == nullptr) {
        viewer->setGpuPresentationDependencies(nullptr, nullptr, std::string{}, 1.0);
        return 1;
    }
    checks.expect(viewer->rect().contains(container->geometry()),
                  "the native container stays inside the viewer before the swap");
    QWindow* nativeWindow = nativeVulkanWindow(container, host.get());
    checks.expect(nativeWindow != nullptr && nativeWindow->surfaceType() == QSurface::VulkanSurface,
                  "the presenter owns a real visible Vulkan QWindow before the swap");
    if (nativeWindow == nullptr) {
        viewer->setGpuPresentationDependencies(nullptr, nullptr, std::string{}, 1.0);
        return 1;
    }

    const QPointF firstCentre(viewer->canvasRectForTest().center());
    viewer->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    area->setAreaActive(false);
    activations = 0;
    sendNativeMouse(*nativeWindow, *container, QEvent::MouseButtonPress, firstCentre,
                    Qt::LeftButton, Qt::LeftButton);
    checks.expect(activations > 0U && area->isAreaActive(),
                  "native input activates the panel before the swap");
    sendNativeMouse(*nativeWindow, *container, QEvent::MouseButtonRelease, firstCentre,
                    Qt::LeftButton, Qt::NoButton);

    // ---- Document replacement (File > Open): rebind the session to a second project, exactly as
    // apps/bloom/main.cpp does on ProjectHost::sessionReplaced, while the native target is live.
    // ----
    const auto swapFormat = document::CompositionFormat::create(1280, 720);
    checks.expect(swapFormat.has_value(), "the second project's format is valid");
    if (!swapFormat.has_value()) {
        return 1;
    }
    auto secondProject = document::makeNewProject("Resident Swap Second", "Main",
                                                  core::RationalTime::fromInteger(5), *swapFormat);
    SecondProject second(std::move(secondProject));
    session.rebind(second.document, second.commands, second.compositionId);
    checks.expect(session.compositionId() == second.compositionId,
                  "the session rebinds to the second project's document/stack");
    checks.expect(
        session.addSolidLayer(QStringLiteral("Second Solid"), core::Color4d{0.1, 0.8, 0.2, 1.0}),
        "the second project's Solid is authored after the rebind");
    controller.requestRefresh();

    const auto afterSwapCounters = service.status().counters;
    const std::uint64_t presentCountBefore = viewer->gpuNativePresentCountForTest();
    const bool secondResident =
        waitForResidentComposition(controller, *viewer, second.compositionId, 30s);
    checks.expect(secondResident, "the viewer presents the second project's resident frame");
    if (!secondResident) {
        std::cerr << "PHASE second-resident-failed residentDetail="
                  << service.status().residentDetail
                  << " presentationDetail=" << service.status().presentationDetail << '\n';
        viewer->setGpuPresentationDependencies(nullptr, nullptr, std::string{}, 1.0);
        return 1;
    }

    // Genuine owner-observed native present for the new frame (never mailbox admission).
    const bool secondGenuinelyPresented = waitUntil(
        [&] {
            const std::uint64_t enqueued = viewer->gpuNativeLastEnqueuedSequenceForTest();
            const std::uint64_t applied = viewer->gpuNativeAppliedSequenceForTest();
            return enqueued > 0U && applied >= enqueued &&
                   viewer->gpuNativePresentCountForTest() > presentCountBefore;
        },
        20s);
    checks.expect(secondGenuinelyPresented,
                  "the swapped resident frame was genuinely applied natively (applied >= enqueued "
                  "and the owner present count advanced)");
    checks.expect(service.status().counters.fullFrameReadbacks ==
                      afterSwapCounters.fullFrameReadbacks,
                  "the project swap performed no full-frame readback");
    checks.expect(service.status().counters.cpuFallbacks == afterSwapCounters.cpuFallbacks,
                  "the project swap forced no CPU fallback");

    const bool coverHiddenAfterSwap =
        waitUntil([&] { return !viewer->gpuCpuCoverVisibleForTest(); }, 15s);
    checks.expect(coverHiddenAfterSwap,
                  "the native CPU cover is hidden after the swapped frame's present ack");

    // The container may briefly hide while the presenter re-attaches the new document; the
    // user-visible invariant is that it settles visible and inside the viewer.
    QWidget* swappedContainer = nullptr;
    const bool swappedContainerSettled = waitUntil(
        [&] {
            swappedContainer = nativeContainer(*viewer);
            return swappedContainer != nullptr && swappedContainer->isVisible() &&
                   !swappedContainer->geometry().isEmpty() &&
                   viewer->rect().contains(swappedContainer->geometry());
        },
        15s);
    checks.expect(swappedContainerSettled,
                  "the native container settles visible and inside the viewer after the swap");
    QWindow* swappedWindow = nullptr;
    const bool swappedWindowReady = waitUntil(
        [&] {
            swappedWindow = nativeVulkanWindow(swappedContainer, host.get());
            return swappedWindow != nullptr &&
                   swappedWindow->surfaceType() == QSurface::VulkanSurface;
        },
        15s);
    checks.expect(swappedWindowReady,
                  "the presenter still owns its real Vulkan QWindow after the swap");

    // A host resize exercises the cover show/hide path: the presenter must re-present and the cover
    // must be hidden again once the new present is acknowledged.
    const std::uint64_t resizePresentBefore = viewer->gpuNativePresentCountForTest();
    host->resize(host->width() + 60, host->height() + 40);
    QApplication::processEvents();
    const bool resizedAndRepresented = waitUntil(
        [&] {
            const std::uint64_t enqueued = viewer->gpuNativeLastEnqueuedSequenceForTest();
            const std::uint64_t applied = viewer->gpuNativeAppliedSequenceForTest();
            return applied >= enqueued &&
                   viewer->gpuNativePresentCountForTest() > resizePresentBefore &&
                   !viewer->gpuCpuCoverVisibleForTest();
        },
        20s);
    checks.expect(resizedAndRepresented,
                  "a viewer resize re-presents the resident frame and hides the cover after ack");
    checks.expect(swappedContainer == nullptr ||
                      viewer->rect().contains(swappedContainer->geometry()),
                  "the native container stays inside the viewer after the resize");

    // Native input still activates the panel after the swap.
    QWidget* activeContainer = nativeContainer(*viewer);
    QWindow* activeWindow = nativeVulkanWindow(activeContainer, host.get());
    checks.expect(activeContainer != nullptr && activeWindow != nullptr,
                  "the native container/window are available after the swap");
    if (activeContainer != nullptr && activeWindow != nullptr) {
        viewer->setFocus(Qt::OtherFocusReason);
        QApplication::processEvents();
        area->setAreaActive(false);
        activations = 0;
        const QPointF centre(viewer->canvasRectForTest().center());
        sendNativeMouse(*activeWindow, *activeContainer, QEvent::MouseButtonPress, centre,
                        Qt::LeftButton, Qt::LeftButton);
        checks.expect(activations > 0U && area->isAreaActive(),
                      "native input activates the panel after the swap");
        sendNativeMouse(*activeWindow, *activeContainer, QEvent::MouseButtonRelease, centre,
                        Qt::LeftButton, Qt::NoButton);
    }

    // ---- Hide/show transition: rebind to a composition-less document (blank New), so the viewer
    // has no frame at all; the native container and cover must both hide. Then rebind back to a
    // composition and the native surface must return. ----
    document::Document blankDocument(
        document::Project(document::ProjectId::fromRaw(91), "Resident Swap Blank"));
    commands::CommandStack blankCommands(blankDocument);
    session.rebind(blankDocument, blankCommands, document::CompositionId{});
    checks.expect(session.composition() == nullptr,
                  "the session rebinds to a composition-less document");
    QApplication::processEvents();
    const bool hiddenOnBlank = waitUntil(
        [&] {
            QWidget* current = nativeContainer(*viewer);
            return !viewer->residentPresentationActiveForTest() &&
                   (current == nullptr || !current->isVisible()) &&
                   !viewer->gpuCpuCoverVisibleForTest();
        },
        15s);
    if (!hiddenOnBlank) {
        QWidget* current = nativeContainer(*viewer);
        std::cerr << "PHASE blank-hide-failed residentActive="
                  << viewer->residentPresentationActiveForTest()
                  << " container=" << (current != nullptr ? 1 : 0)
                  << " containerVisible=" << ((current != nullptr && current->isVisible()) ? 1 : 0)
                  << " coverVisible=" << (viewer->gpuCpuCoverVisibleForTest() ? 1 : 0)
                  << " composition=" << (session.composition() != nullptr ? 1 : 0) << '\n';
    }
    checks.expect(hiddenOnBlank,
                  "with no composition the native container and cover both hide (CPU-only state)");

    auto thirdProject = document::makeNewProject("Resident Swap Third", "Main",
                                                 core::RationalTime::fromInteger(5), *swapFormat);
    SecondProject third(std::move(thirdProject));
    session.rebind(third.document, third.commands, third.compositionId);
    checks.expect(session.compositionId() == third.compositionId,
                  "the session rebinds back to a composition-bearing document");
    checks.expect(
        session.addSolidLayer(QStringLiteral("Third Solid"), core::Color4d{0.9, 0.7, 0.1, 1.0}),
        "the third project's Solid is authored");
    controller.requestRefresh();
    const bool thirdResident =
        waitForResidentComposition(controller, *viewer, third.compositionId, 30s);
    checks.expect(thirdResident, "the native resident surface returns after the blank rebind");
    checks.expect(waitUntil([&] { return !viewer->gpuCpuCoverVisibleForTest(); }, 15s),
                  "the native CPU cover is hidden again after the returned present ack");

    bool viewerRetired = false;
    static_cast<void>(viewer->prepareNativeSurfaceMutation(
        92,
        [&](const std::uint64_t generation, const ui::EditorNativeSurface::PrepareResult& result) {
            checks.expect(generation == 92, "the viewer mutation generation is echoed");
            viewerRetired = result.safeToMutate;
        }));
    checks.expect(waitUntil([&] { return viewerRetired; }, 20s),
                  "the ViewerEditor native surface retired before teardown");
    viewer->setGpuPresentationDependencies(nullptr, nullptr, std::string{}, 1.0);
    host.reset();

    controller.beginShutdown();
    bridge.beginShutdown();
    (void)waitUntil([&] { return scheduler.isQuiescent(); }, 10s);
    service.beginShutdown();
    (void)waitUntil(
        [&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; }, 30s);
    checks.expect(service.status().counters.fullFrameReadbacks == 0U,
                  "the whole project-swap run performed no full-frame readback");
    std::error_code ignored;
    std::filesystem::remove_all(mediaDirectory, ignored);
    std::filesystem::remove_all(settingsRoot, ignored);

    if (checks.failures() != 0) {
        std::cerr << "resident project-swap proof: " << checks.failures()
                  << " verification failure(s)\n";
        return 1;
    }
    std::cout << "PASS: resident project-swap proof (nativeDispatches="
              << service.status().counters.nativeDispatches << ", activations=" << activations
              << ")\n";
    return 0;
}
