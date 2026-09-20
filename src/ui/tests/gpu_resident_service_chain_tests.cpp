// Real GPU end-to-end vertical acceptance, service half: the acceptance steps.
//
// ONE real GpuPreviewDisplayService (resident overload, Wayland presentation) is driven with the
// UNMODIFIED production stage factories over a GENUINE document:
//   real Document + AssetRecord + ImageSource/Layer/Merge graph, compiled by the real
//   SnapshotCompiler -> real makeCompositionPreviewGpuSceneStage -> service owner thread ->
//   GpuSceneExecutor -> GpuResidentDisplay -> product factory -> opaque GpuResidentFrameLease ->
//   real Wayland QWindow/VkSurfaceKHR.
//
// The supported 1920x1080 scene has two Solid layers (Normal == source-over, a fractional
// translation, a 4:3 non-square PAR) and a genuine still-media ImageSource over a probed OpenEXR
// asset. The media upload is therefore part of the resident native operations, and the CPU oracle
// is the production CPU stage evaluated on the SAME snapshot. Unsupported and retirement paths are
// exercised on their own documents/requests, never by relabelling a hand-lowered plan.

#include "gpu_service_chain_fixture.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QVulkanInstance>
#include <QWindow>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

using namespace bloom::ui::verticalproof;
using namespace std::chrono_literals;

namespace {

using namespace bloom;

// Builds the genuine supported document: two Solid layers + one real ImageSource layer over the
// probed EXR asset. The asset is inserted into the project and the layer is added through the real
// AddImageLayer command, so the SnapshotCompiler lowers a real image-source node.
struct SupportedDocument final {
    std::unique_ptr<document::Document> document;
    std::unique_ptr<commands::CommandStack> commands;
    std::unique_ptr<ui::CompositionSession> session;
    document::CompositionId compositionId;
};

[[nodiscard]] SupportedDocument buildSupportedDocument(Fixture& fixture, Expectations& checks) {
    auto newProject = document::makeNewProject(
        "Vertical Proof", "Main", core::RationalTime::fromInteger(10), format1920x1080NonSquare());
    static_cast<void>(newProject.project.addAsset(fixture.mediaAsset));
    SupportedDocument built;
    built.compositionId = newProject.initialCompositionId;
    built.document = std::make_unique<document::Document>(std::move(newProject.project));
    built.commands = std::make_unique<commands::CommandStack>(*built.document);
    built.session = std::make_unique<ui::CompositionSession>(*built.document, *built.commands,
                                                             built.compositionId);
    checks.expect(
        built.session->addSolidLayer(QStringLiteral("Lower"), core::Color4d{0.15, 0.35, 0.85, 1.0}),
        "the lower Solid layer is created");
    checks.expect(built.session->setSelectedPosition(0.0, 0.0),
                  "the lower Solid is placed at the origin");
    checks.expect(
        built.session->addSolidLayer(QStringLiteral("Upper"), core::Color4d{0.9, 0.25, 0.1, 1.0}),
        "the upper Solid layer is created");
    checks.expect(built.session->setSelectedPosition(13.5, -7.25),
                  "the upper Solid carries a fractional translation");
    checks.expect(built.session->setSelectedBlendMode(core::BlendMode::Normal),
                  "the upper Solid composites source-over (Normal)");
    commands::Transaction addImage("Add image layer");
    addImage.emplace<commands::AddImageLayer>(built.compositionId, fixture.mediaAsset.id);
    const auto result = built.commands->execute(std::move(addImage));
    checks.expect(result.succeeded(),
                  "the genuine image-source layer is added through the real AddImageLayer command");
    return built;
}

// Builds the genuine UNSUPPORTED document: a text layer is outside the prepared GPU subset.
struct UnsupportedDocument final {
    std::unique_ptr<document::Document> document;
    std::unique_ptr<commands::CommandStack> commands;
    std::unique_ptr<ui::CompositionSession> session;
    document::CompositionId compositionId;
};

[[nodiscard]] UnsupportedDocument buildUnsupportedDocument(Expectations& checks) {
    auto newProject =
        document::makeNewProject("Vertical Unsupported", "Main",
                                 core::RationalTime::fromInteger(10), format1920x1080NonSquare());
    UnsupportedDocument built;
    built.compositionId = newProject.initialCompositionId;
    built.document = std::make_unique<document::Document>(std::move(newProject.project));
    built.commands = std::make_unique<commands::CommandStack>(*built.document);
    built.session = std::make_unique<ui::CompositionSession>(*built.document, *built.commands,
                                                             built.compositionId);
    checks.expect(built.session->addTextLayer(QStringLiteral("Title"), QStringLiteral("Bloom")),
                  "the unsupported text layer is created");
    return built;
}

[[nodiscard]] std::string hexOf(const render::Rgba8 c) {
    char buffer[10];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", c.red, c.green, c.blue, c.alpha);
    return buffer;
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    const TestOptions options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    QGuiApplication application(argc, argv);
    phase("start", "platform=" + std::string(QGuiApplication::platformName().toUtf8().constData()));
    if (options.loader_path.empty()) {
        std::cout << "SKIP: --loader <libvulkan.so.1> is required\n";
        return options.require_device ? 1 : 0;
    }
    qputenv("QT_VULKAN_LIB", options.loader_path.string().c_str());

    Expectations checks;
    const auto mediaDirectory =
        std::filesystem::temp_directory_path() /
        ("bloom-vertical-proof-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(mediaDirectory);
    Fixture fixture(mediaDirectory);
    if (fixture.processor == nullptr) {
        std::cout << "SKIP: the Bloom Neutral v1 processor is unavailable\n";
        return options.require_device ? 1 : 0;
    }

    SupportedDocument supported = buildSupportedDocument(fixture, checks);
    const auto supportedSnapshot = supported.session->snapshot();
    phase("scene", "extent=1920x1080 par=4:3 solids=2 blend=source-over translation=fractional "
                   "image-source=1 asset=probed-EXR");

    TaskScheduler scheduler(schedulerConfig());
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    GpuPreviewDisplayService service(
        scheduler, fixture.productionGpuStage(), fixture.productionCpuStage(),
        fixture.productionCpuFallback(displayPreparer), serviceOptions(options.loader_path));

    const bool terminal = waitUntil(
        [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; }, 90s);
    const auto status = service.status();
    const bool residentReady =
        terminal && status.state == GpuPreviewDisplayServiceState::Ready &&
        status.residentQualification != nullptr && status.residentQualification->eligible() &&
        status.presentationClient != nullptr &&
        status.presentationAvailability == render::GpuPresentationAvailability::Ready;
    if (!residentReady) {
        checks.expect(
            !options.require_device,
            "a presentable Wayland device with a qualified resident route is required but "
            "unavailable: " +
                status.residentDetail + " / " + status.presentationDetail);
        service.beginShutdown();
        if (checks.failures() == 0) {
            std::cout << "SKIP: resident route unavailable; no native success claimed\n";
        }
        return checks.failures() == 0 ? 0 : 1;
    }
    phase("service-ready", "state=Ready presentation=Ready");
    checks.expect(status.counters.fullFrameReadbacks == 0U,
                  "no full-frame readback during startup/qualification");

    const std::shared_ptr<GpuPresentationClient> client = status.presentationClient;
    const GpuBorrowedInstanceView view = client->instanceView();
    checks.expect(view.valid, "the service client publishes the borrowed instance view");
    if (!view.valid) {
        return 1;
    }
    QVulkanInstance instance;
    // clang-format off
    instance.setVkInstance(reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(view.instance_bits)));  // NOLINT(performance-no-int-to-ptr)
    // clang-format on
    if (!instance.create() || !instance.isValid()) {
        std::cout << "SKIP: QVulkanInstance could not adopt the borrowed instance\n";
        return options.require_device ? 1 : 0;
    }

    UiSurface surface;
    surface.window = new QWindow();
    surface.window->setSurfaceType(QSurface::VulkanSurface);
    surface.window->setVulkanInstance(&instance);
    const std::string windowTitle =
        "BloomVerticalProof-" + std::to_string(QCoreApplication::applicationPid());
    surface.window->setTitle(QString::fromStdString(windowTitle));
    surface.window->resize(320, 240);
    surface.window->show();
    phase("window", "title=" + windowTitle + " extent=320x240");
    surface.width = 320U;
    surface.height = 240U;
    checks.expect(waitUntil(
                      [&] {
                          surface.surface = QVulkanInstance::surfaceForWindow(surface.window);
                          return surface.surface != VK_NULL_HANDLE;
                      },
                      10s),
                  "the UI QWindow produced a Wayland VkSurfaceKHR");
    if (surface.surface == VK_NULL_HANDLE) {
        return 1;
    }
    GpuBorrowedSurface borrowed;
    borrowed.surface_bits = surface.bits();
    borrowed.epoch = view.epoch;
    const auto attached = client->attach(borrowed, 320U, 240U);
    checks.expect(attached.code == GpuPresentationPortCode::Accepted,
                  "the service client admitted the attach");
    checks.expect(waitUntil(
                      [&] {
                          return client->status(attached.target).state ==
                                 GpuPresentationTargetState::Active;
                      },
                      15s),
                  "the service owner created and activated the swapchain");
    phase("surface-attached", "target-active=1");

    TaskOwner taskOwner;
    taskOwner.kind = TaskOwnerKind::Composition;
    taskOwner.id = TaskOwnerId::fromRaw(7);

    // --- 1. Genuine supported document: resident, media uploads included
    // ---------------------------
    const auto supportedIdentity = makeIdentity(1, supportedSnapshot, supported.compositionId);
    const auto supportedReference =
        snapshotCpuReference(fixture, supportedSnapshot, supportedIdentity);
    checks.expect(supportedReference != nullptr && hasCpuPixels(supportedReference),
                  "the production CPU stage evaluates the genuine document into real pixels");
    const auto beforeSupported = service.status().counters;
    auto supportedRequest = service.submit(TaskRequest("vertical supported", taskOwner),
                                           supportedSnapshot, supportedIdentity, kBudget, {});
    checks.expect(supportedRequest.status == TaskSubmissionStatus::Accepted,
                  "the supported document request was admitted");
    const auto supportedResult = awaitResult(supportedRequest.handle);
    const auto supportedFrame =
        supportedResult.has_value() && supportedResult->state() == TaskState::Succeeded
            ? residentFrameOf(*supportedResult)
            : nullptr;
    checks.expect(supportedFrame != nullptr, "the genuine document produced a prepared frame: " +
                                                 service.status().residentDetail);
    checks.expect(isResident(supportedFrame),
                  "the genuine two-Solid + ImageSource document published the resident arm: " +
                      service.status().residentDetail);
    checks.expect(!hasCpuPixels(supportedFrame),
                  "the resident document frame retains no CPU display buffer");
    checks.expect(supportedFrame != nullptr &&
                      supportedFrame->desiredIdentity() == supportedIdentity,
                  "the resident frame carries the genuine document identity");
    const auto afterSupported = service.status().counters;
    checks.expect(afterSupported.residentGraphJobs == beforeSupported.residentGraphJobs + 1U,
                  "exactly one resident graph job was admitted");
    checks.expect(
        afterSupported.nativeDispatches > beforeSupported.nativeDispatches,
        "the resident graph performed real native operations (the image upload included)");
    checks.expect(afterSupported.fullFrameReadbacks == 0U,
                  "the resident document graph performed no full-frame readback");
    checks.expect(afterSupported.displayStatusReads == beforeSupported.displayStatusReads + 1U,
                  "the resident display invalidated exactly its 4-byte status word");

    // Known media sample: the image layer is centred and its opaque green quadrant maps to the
    // composition centre. Print the CPU oracle colour at that composition pixel for the external
    // capture comparison; the in-process assertion is the genuine resident arm above.
    if (supportedReference != nullptr) {
        const auto viewRef = supportedReference->displayBufferView();
        if (viewRef.has_value() && viewRef->displayWindow.extent().width() > 0 &&
            viewRef->displayWindow.extent().height() > 0) {
            const auto width = viewRef->displayWindow.extent().width();
            const auto height = viewRef->displayWindow.extent().height();
            const auto originX = viewRef->displayWindow.originX();
            const auto originY = viewRef->displayWindow.originY();
            const auto sampleX = originX + static_cast<std::int64_t>(width / 2);
            const auto sampleY = originY + static_cast<std::int64_t>(height / 2);
            const auto index = static_cast<std::size_t>(sampleY - originY) * width +
                               static_cast<std::size_t>(sampleX - originX);
            if (index < viewRef->pixels.size()) {
                phase("media-sample", "composition=" + std::to_string(sampleX) + "," +
                                          std::to_string(sampleY) +
                                          " cpuReference=" + hexOf(viewRef->pixels[index]) +
                                          " resident=" + (isResident(supportedFrame) ? "1" : "0"));
            }
        }
    }

    std::optional<GpuResidentFrameLease> supportedLease;
    if (isResident(supportedFrame)) {
        supportedLease = supportedFrame->residentFrame()->lease();
        checks.expect(supportedLease->isValid(), "the resident document lease is valid");
    }
    if (!supportedLease.has_value()) {
        service.beginShutdown();
        return 1;
    }
    checks.expect(
        client->update(attached.target, 1U, makeUpdate(*supportedLease, 320U, 240U)).accepted(),
        "the resident present update is admitted");
    checks.expect(
        waitUntil([&] { return client->status(attached.target).appliedSequence >= 1U; }, 15s),
        "the resident present was applied on the owner thread");
    checks.expect(client->status(attached.target).presentCount == 1U,
                  "exactly one resident present completed");
    phase("present-ack", "presentCount=1");

    // --- 2. Warm identical request: content-cache reuse, zero additional native operations
    // ---------
    const auto beforeWarm = service.status().counters;
    auto warmRequest =
        service.submit(TaskRequest("vertical warm", taskOwner), supportedSnapshot,
                       makeIdentity(2, supportedSnapshot, supported.compositionId), kBudget, {});
    checks.expect(warmRequest.status == TaskSubmissionStatus::Accepted,
                  "the warm identical request was admitted");
    const auto warmResult = awaitResult(warmRequest.handle);
    const auto warmFrame = warmResult.has_value() && warmResult->state() == TaskState::Succeeded
                               ? residentFrameOf(*warmResult)
                               : nullptr;
    checks.expect(isResident(warmFrame), "the warm identical request produced a resident frame");
    const auto afterWarm = service.status().counters;
    checks.expect(afterWarm.nativeDispatches == beforeWarm.nativeDispatches,
                  "a warm identical request performed zero additional native operations");
    checks.expect(afterWarm.gpuCacheHits > beforeWarm.gpuCacheHits,
                  "a warm identical request was served from the content cache");
    phase("warm-reuse", "nativeDispatches=" + std::to_string(afterWarm.nativeDispatches) +
                            " cacheHits=" + std::to_string(afterWarm.gpuCacheHits));

    // --- 3. Genuine unsupported document: full original CPU path with real pixels
    // ------------------
    UnsupportedDocument unsupported = buildUnsupportedDocument(checks);
    const auto unsupportedSnapshot = unsupported.session->snapshot();
    const auto unsupportedIdentity =
        makeIdentity(3, unsupportedSnapshot, unsupported.compositionId);
    const auto beforeUnsupported = service.status().counters;
    auto unsupportedRequest = service.submit(TaskRequest("vertical unsupported", taskOwner),
                                             unsupportedSnapshot, unsupportedIdentity, kBudget, {});
    checks.expect(unsupportedRequest.status == TaskSubmissionStatus::Accepted,
                  "the unsupported document request was admitted");
    const auto unsupportedResult = awaitResult(unsupportedRequest.handle);
    const auto unsupportedFrame =
        unsupportedResult.has_value() && unsupportedResult->state() == TaskState::Succeeded
            ? residentFrameOf(*unsupportedResult)
            : nullptr;
    checks.expect(unsupportedFrame != nullptr,
                  "the unsupported document still produced a prepared frame");
    checks.expect(!isResident(unsupportedFrame) && hasCpuPixels(unsupportedFrame),
                  "the unsupported document took the full CPU path with real pixels (never blank)");
    checks.expect(service.status().counters.cpuFallbacks > beforeUnsupported.cpuFallbacks,
                  "the unsupported document incremented the CPU fallback counter");
    phase("unsupported-fallback", "cpuFallback=1");

    // --- 4. Stalled/unknown-fence retirement: parent/admission retained until proven
    // ---------------
    const auto core = runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    checks.expect(core != nullptr, "the service core is reachable");
    core->residentDisplayUnretiredOverride = [](const render::GpuResidentDisplay&) noexcept {
        return true;
    };
    core->residentDisplayPollOverride = [](render::GpuResidentDisplay&) noexcept {
        return render::GpuResidentDisplayPollResult::Pending;
    };
    auto stalledRequest =
        service.submit(TaskRequest("vertical stalled", taskOwner), supportedSnapshot,
                       makeIdentity(4, supportedSnapshot, supported.compositionId), kBudget, {});
    checks.expect(stalledRequest.status == TaskSubmissionStatus::Accepted,
                  "the stalled resident request was admitted");
    checks.expect(
        waitUntil(
            [&] {
                return runtime::detail::GpuPreviewDisplayServiceTestAccess::residentRetiring(*core);
            },
            30s),
        "the stalled submission entered the explicit Retiring phase");
    checks.expect(!stalledRequest.handle.tryTakeResult().has_value(),
                  "the parent completion is retained while the fence is unretired");
    const auto stalledResult = awaitResult(stalledRequest.handle, 30s);
    const auto stalledFrame =
        stalledResult.has_value() && stalledResult->state() == TaskState::Succeeded
            ? residentFrameOf(*stalledResult)
            : nullptr;
    checks.expect(!isResident(stalledFrame) && hasCpuPixels(stalledFrame),
                  "the stalled submission was released only via the CPU fallback with real pixels");
    checks.expect(service.status().counters.retirementUnprovenTeardowns >= 1U,
                  "the bounded retirement budget forced an explicit owner-thread teardown");
    checks.expect(runtime::detail::GpuPreviewDisplayServiceTestAccess::residentRouteTerminal(*core),
                  "an unprovable fence marks the resident route terminal");
    core->residentDisplayUnretiredOverride = {};
    core->residentDisplayPollOverride = {};
    phase("retired", "unprovenTeardown=1 terminal=1");

    // --- 5. Host-ordered shutdown; the presented lease stays valid until its target retires
    // --------
    checks.expect(supportedLease->isValid(),
                  "the presented lease is still valid before retirement");
    static_cast<void>(client->retire(attached.target, 5U));
    checks.expect(
        waitUntil([&] { return client->status(attached.target).surfaceSafeToDestroy; }, 30s),
        "the resident target proved retirement and its surface is safe to destroy");
    surface.window->hide();
    delete surface.window;
    surface.window = nullptr;

    service.beginShutdown();
    checks.expect(
        waitUntil([&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; },
                  30s),
        "the resident service owner drained and stopped");
    const auto finalStatus = service.status();
    checks.expect(finalStatus.presentationClient == nullptr,
                  "the stopped resident service publishes no live presentation client");
    checks.expect(finalStatus.presentationShutdown.drained,
                  "the resident service shutdown drained");
    checks.expect(finalStatus.presentationShutdown.unprovenTargets == 0U &&
                      finalStatus.presentationShutdown.quarantinedTargets == 0U,
                  "the resident service shutdown retained no unproven target");
    checks.expect(finalStatus.counters.fullFrameReadbacks == 0U,
                  "the whole resident run performed no full-frame readback");
    checks.expect(!supportedLease->isValid(),
                  "the lease is invalid after the owning registry is destroyed");
    instance.destroy();
    phase("shutdown", "drained=1");
    std::error_code ignored;
    std::filesystem::remove_all(mediaDirectory, ignored);

    if (checks.failures() == 0) {
        std::cout
            << "PASS: genuine two-Solid + ImageSource document is GPU-resident with uploads; "
               "warm reuse; unsupported CPU fallback; safe retirement; host-ordered shutdown\n";
    }
    return checks.failures() == 0 ? 0 : 1;
}
