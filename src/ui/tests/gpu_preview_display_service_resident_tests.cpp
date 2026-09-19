// Real Wayland acceptance for the GpuPreviewDisplayService RESIDENT route.
//
// It builds the genuine Bloom Neutral v1 processor and a real solid CompiledCompositionPlan, then
// drives the existing service with the additive resident overload:
//   real snapshot/identity -> real PreviewGpuSceneStageFunction (real CpuGpuSceneBuilder) ->
//   service owner thread -> real GpuSceneExecutor -> real GpuResidentDisplay -> real product
//   factory
//   -> opaque GpuResidentFrameLease -> the real service presentation registry/coordinator ->
//   a real Wayland QWindow/VkSurfaceKHR.
//
// Assertions cover: genuine resident qualification; a resident prepared frame with GpuResident
// provenance and NO CPU buffer and NO full-frame readback; a warm identical request served from the
// content cache with zero additional native operations; an unsupported GPU-subset request taking
// the full original CPU path; a deterministic cancellation during the GPU-scene CPU stage; and a
// host-ordered shutdown where the lease stays valid until its target is retired.
//
// --loader pins an explicit loader; --require-device fails closed without a compatible presentable
// device. Nothing here fabricates an image or a qualification report.

#include "gpu_preview_display_service_private.hpp"

#include "gpu_borrowed_instance.hpp"

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_resident_frame_lease.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/preview_cpu_stage.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QGuiApplication>
#include <QVulkanInstance>
#include <QWindow>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using bloom::core::Color4d;
using bloom::core::RationalTime;
using bloom::render::GpuBorrowedInstanceView;
using bloom::render::GpuBorrowedSurface;
using bloom::render::GpuPresentBackground;
using bloom::render::GpuPresentChannel;
using bloom::render::GpuPresentRect;
using bloom::render::GpuPresentSourceWindow;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::CpuQualifiedDisplayPreparer;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::EvaluationQuality;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::GpuPresentationClient;
using bloom::runtime::GpuPresentationPortCode;
using bloom::runtime::GpuPresentationTargetState;
using bloom::runtime::GpuPresentationUpdate;
using bloom::runtime::GpuPreviewDisplayService;
using bloom::runtime::GpuPreviewDisplayServiceOptions;
using bloom::runtime::GpuPreviewDisplayServicePresentationMode;
using bloom::runtime::GpuPreviewDisplayServiceState;
using bloom::runtime::GpuResidentFrameLease;
using bloom::runtime::PreviewCpuDisplayFallback;
using bloom::runtime::PreviewCpuStage;
using bloom::runtime::PreviewCpuStageFunction;
using bloom::runtime::PreviewCpuStageOutcome;
using bloom::runtime::PreviewCpuStageOutcomeHandle;
using bloom::runtime::PreviewCpuStageStatus;
using bloom::runtime::PreviewGpuSceneStage;
using bloom::runtime::PreviewGpuSceneStageFunction;
using bloom::runtime::PreviewGpuSceneStageOutcome;
using bloom::runtime::PreviewGpuSceneStageOutcomeHandle;
using bloom::runtime::PreviewGpuSceneStageStatus;
using bloom::runtime::PreviewPreparationResult;
using bloom::runtime::PreviewPreparationResultHandle;
using bloom::runtime::PreviewRequestIdentity;
using bloom::runtime::PreviewResolutionPolicy;
using bloom::runtime::ProcessFrame;
using bloom::runtime::TaskContext;
using bloom::runtime::TaskDiagnostic;
using bloom::runtime::TaskHandle;
using bloom::runtime::TaskRequest;
using bloom::runtime::TaskResult;
using bloom::runtime::TaskScheduler;
using bloom::runtime::TaskSchedulerConfig;
using bloom::runtime::TaskState;

constexpr std::size_t kBudget = std::size_t{1} << 28;
constexpr auto kProjectId = bloom::document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = bloom::document::CompositionId::fromRaw(2);
constexpr auto kSolid = bloom::document::NodeId::fromRaw(10);
constexpr auto kLayerNode = bloom::document::NodeId::fromRaw(11);
constexpr auto kStack = bloom::document::NodeId::fromRaw(12);
constexpr auto kOutput = bloom::document::NodeId::fromRaw(13);
constexpr auto kLayer = bloom::document::LayerId::fromRaw(20);
constexpr auto kSlot = bloom::document::LayerSlotId::fromRaw(30);
constexpr auto kColorP = bloom::document::ParameterId::fromRaw(40);
constexpr auto kPosP = bloom::document::ParameterId::fromRaw(41);
constexpr auto kOpacityP = bloom::document::ParameterId::fromRaw(42);
constexpr auto kAnchorP = bloom::document::ParameterId::fromRaw(43);
constexpr auto kScaleP = bloom::document::ParameterId::fromRaw(44);
constexpr auto kRotP = bloom::document::ParameterId::fromRaw(45);
constexpr auto kBlendP = bloom::document::ParameterId::fromRaw(46);

// Generations used to select behavior in the injected stage function.
constexpr std::uint64_t kGenerationResident = 1;
constexpr std::uint64_t kGenerationUnsupportedSubset = 2;
constexpr std::uint64_t kGenerationGated = 3;

struct TestOptions final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool valid = true;
};

[[nodiscard]] TestOptions parseOptions(const int argc, char** argv) {
    TestOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                std::cerr << "--loader requires a path argument\n";
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

class Expectations final {
  public:
    void expect(const bool ok, const std::string& message,
                const std::source_location loc = std::source_location::current()) {
        if (!ok) {
            ++failures_;
            std::cerr << loc.file_name() << ':' << loc.line() << ": " << message << '\n';
        }
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] TaskSchedulerConfig schedulerConfig() {
    TaskSchedulerConfig config;
    config.cpuWorkerCount = 2;
    config.rowBandWorkerCount = bloom::runtime::kSerialRowBandWorkers;
    config.blockingIoWorkerCount = 1;
    config.cpuQueueCapacity = 64;
    config.blockingIoQueueCapacity = 8;
    config.gpuPendingQueueCapacity = 8;
    config.gpuAdmittedStateCapacity = 8;
    config.gpuLiveContinuationCapacity = 4;
    config.gpuQueuedCommandByteCapacity = std::size_t{1} << 30U;
    config.gpuRequestOwnedByteCapacity = std::size_t{1} << 30U;
    config.terminalHistoryCapacity = 64;
    config.diagnosticsPerTask = 16;
    config.groupRegistryCapacity = 16;
    return config;
}

[[nodiscard]] bloom::document::CompositionFormat format(const std::uint32_t w,
                                                        const std::uint32_t h) {
    const auto value = bloom::document::CompositionFormat::create(w, h);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

#include "gpu_preview_display_service_resident_fixture.ipp"
[[nodiscard]] PreviewCpuDisplayFallback
residentFallback(std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle> processor) {
    return [processor = std::move(processor)](
               const PreviewCpuStage& stage,
               TaskContext& context) -> TaskResult<PreviewPreparationResultHandle> {
        using R = TaskResult<PreviewPreparationResultHandle>;
        if (context.isCancellationRequested()) {
            return R::cancelled();
        }
        CpuQualifiedDisplayPreparer preparer(*processor);
        bloom::runtime::QualifiedDisplayPreparationRequest request;
        request.aggregatePixelStorageByteLimit = stage.pixelStorageByteLimit();
        auto prepared = preparer.prepare(stage.processFrame(), request, context.cancellation());
        if (prepared.status() != bloom::runtime::QualifiedDisplayPreparationStatus::Prepared ||
            prepared.frame() == nullptr) {
            return R::failed(std::vector<TaskDiagnostic>{failDiag("harness.cpu-display-failed")});
        }
        auto frame = bloom::runtime::PreparedPreviewFrame::createQualified(
            stage.desiredIdentity().requestGeneration, prepared.frame());
        if (!frame.has_value()) {
            return R::failed(std::vector<TaskDiagnostic>{failDiag("harness.cpu-frame-failed")});
        }
        auto result = PreviewPreparationResult::prepared(
            std::make_shared<const bloom::runtime::PreparedPreviewFrame>(std::move(*frame)));
        if (!result.has_value()) {
            return R::failed(std::vector<TaskDiagnostic>{failDiag("harness.cpu-result-failed")});
        }
        return R::succeeded(std::make_shared<const PreviewPreparationResult>(std::move(*result)));
    };
}

[[nodiscard]] GpuPreviewDisplayServiceOptions serviceOptions(const std::filesystem::path& loader) {
    GpuPreviewDisplayServiceOptions options;
    options.enabled = true;
    options.loaderPath = loader;
    options.presentation = GpuPreviewDisplayServicePresentationMode::Wayland;
    options.previewByteAllowance = kBudget;
    options.presentationCoordinator.maxTargets = 3U;
    options.presentationCoordinator.maxRetainedTargets = 64U;
    options.presentationCoordinator.shutdownDrainPumps = 600U;
    return options;
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(500us);
    }
    return predicate();
}

template <typename Predicate>
[[nodiscard]] bool waitUntilEvents(Predicate predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        QGuiApplication::processEvents();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    QGuiApplication::processEvents();
    return predicate();
}

template <typename Value>
[[nodiscard]] std::optional<TaskResult<Value>>
awaitResult(const TaskHandle<Value>& handle, const std::chrono::milliseconds timeout = 30s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = handle.tryTakeResult()) {
            return result;
        }
        std::this_thread::sleep_for(500us);
    }
    return std::nullopt;
}

[[nodiscard]] bloom::document::Snapshot makeSnapshot(const std::uint64_t id, std::string name) {
    bloom::document::Document document(
        bloom::document::Project(bloom::document::ProjectId::fromRaw(id), std::move(name)));
    return document.snapshot();
}

[[nodiscard]] GpuPresentationUpdate makeUpdate(const GpuResidentFrameLease& lease,
                                               const std::uint32_t targetWidth,
                                               const std::uint32_t targetHeight) {
    GpuPresentationUpdate update;
    update.lease = lease;
    update.params.targetWidth = targetWidth;
    update.params.targetHeight = targetHeight;
    update.params.destination = GpuPresentRect{0.0F, 0.0F, static_cast<float>(targetWidth),
                                               static_cast<float>(targetHeight)};
    const auto window = lease.displayWindow();
    update.params.source =
        window.has_value() ? GpuPresentSourceWindow{static_cast<double>(window->originX()),
                                                    static_cast<double>(window->originY()),
                                                    static_cast<double>(window->extent().width()),
                                                    static_cast<double>(window->extent().height())}
                           : GpuPresentSourceWindow{0.0, 0.0, static_cast<double>(lease.width()),
                                                    static_cast<double>(lease.height())};
    update.params.channel = GpuPresentChannel::Rgba;
    update.params.background = GpuPresentBackground::Solid;
    return update;
}

struct UiSurface final {
    QWindow* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    [[nodiscard]] std::uint64_t bits() const noexcept {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(surface));
    }
};

} // namespace

int runTests(int argc, char** argv) {
    const TestOptions options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    QGuiApplication application(argc, argv);
    if (options.loader_path.empty()) {
        std::cout << "SKIP: --loader <libvulkan.so.1> is required\n";
        return options.require_device ? 1 : 0;
    }
    qputenv("QT_VULKAN_LIB", options.loader_path.string().c_str());

    Expectations expectations;

    const auto plan = makePlan(1920U, 1080U);
    const auto processor = makeProcessor();
    if (processor == nullptr) {
        std::cout << "SKIP: the Bloom Neutral v1 processor is unavailable\n";
        return options.require_device ? 1 : 0;
    }
    auto builder = std::make_shared<CpuGpuSceneBuilder>();
    auto evaluator = std::make_shared<CpuCompositionEvaluator>();
    auto gateEntered = std::make_shared<std::atomic<bool>>(false);
    auto gateRelease = std::make_shared<std::atomic<bool>>(false);

    TaskScheduler scheduler(schedulerConfig());
    GpuPreviewDisplayService service(
        scheduler, residentStageFunction(plan, processor, builder, gateEntered, gateRelease),
        residentCpuStageFunction(plan, processor, evaluator), residentFallback(processor),
        serviceOptions(options.loader_path));

    const bool terminal = waitUntil(
        [&] {
            const auto state = service.status().state;
            return state != GpuPreviewDisplayServiceState::Initializing;
        },
        90s);
    const auto status = service.status();
    const bool residentReady =
        terminal && status.state == GpuPreviewDisplayServiceState::Ready &&
        status.residentQualification != nullptr && status.residentQualification->eligible() &&
        status.presentationClient != nullptr &&
        status.presentationAvailability == bloom::render::GpuPresentationAvailability::Ready;
    if (!residentReady) {
        expectations.expect(!options.require_device,
                            "a presentable Wayland device with a qualified resident route is "
                            "required but unavailable: " +
                                status.residentDetail + " / " + status.presentationDetail);
        service.beginShutdown();
        if (expectations.failures() == 0) {
            std::cout << "SKIP: resident route unavailable; no native success claimed\n";
        }
        return expectations.failures() == 0 ? 0 : 1;
    }

    const auto core = bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    expectations.expect(core != nullptr, "the service core is reachable");
    expectations.expect(
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::residentRouteAvailable(*core),
        "the owner created the resident cache/executor/display on the service device");
    expectations.expect(
        status.counters.fullFrameReadbacks == 0U,
        "the resident route performed no full-frame readback during startup/qualification");

    const std::shared_ptr<GpuPresentationClient> client = status.presentationClient;
    const GpuBorrowedInstanceView view = client->instanceView();
    expectations.expect(view.valid, "the service client publishes the borrowed instance view");
    if (!view.valid) {
        return 1;
    }
    QVulkanInstance instance;
    instance.setVkInstance(bloom::ui::test::borrowedInstance(view.instance_bits));
    if (!instance.create() || !instance.isValid()) {
        std::cout << "SKIP: QVulkanInstance could not adopt the borrowed instance\n";
        return options.require_device ? 1 : 0;
    }

    UiSurface surface;
    surface.window = new QWindow();
    surface.window->setSurfaceType(QSurface::VulkanSurface);
    surface.window->setVulkanInstance(&instance);
    surface.window->resize(320, 240);
    surface.window->show();
    surface.width = 320U;
    surface.height = 240U;
    expectations.expect(waitUntil(
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
    expectations.expect(attached.code == GpuPresentationPortCode::Accepted,
                        "the service client admitted the attach");
    expectations.expect(waitUntil(
                            [&] {
                                return client->status(attached.target).state ==
                                       GpuPresentationTargetState::Active;
                            },
                            15s),
                        "the service owner created and activated the swapchain");

    // --- Resident request: real prepared GPU scene -> resident product ---------------------------
    const auto snapshot = makeSnapshot(7U, "service-resident-test");
    const auto identity = makeIdentity(*plan, kGenerationResident);
    bloom::runtime::TaskOwner taskOwner;
    taskOwner.kind = bloom::runtime::TaskOwnerKind::Composition;
    taskOwner.id = bloom::runtime::TaskOwnerId::fromRaw(7);
    const auto beforeFirst = service.status().counters;
    auto first =
        service.submit(TaskRequest("resident preview", taskOwner), snapshot, identity, kBudget, {});
    expectations.expect(first.status == bloom::runtime::TaskSubmissionStatus::Accepted,
                        "the resident preview request was admitted");
    const auto firstResult = awaitResult(first.handle);
    const auto* firstCarried =
        firstResult.has_value() && firstResult->state() == TaskState::Succeeded
            ? &firstResult->value()
            : nullptr;
    const bool firstPrepared =
        firstCarried != nullptr && firstCarried->has_value() &&
        (**firstCarried)->status() == bloom::runtime::PreviewPreparationStatus::Prepared;
    expectations.expect(firstPrepared, "the resident request produced a prepared frame");
    std::optional<GpuResidentFrameLease> presentedLease;
    if (firstCarried != nullptr && firstCarried->has_value()) {
        const auto& frame = (**firstCarried)->frame();
        expectations.expect(frame != nullptr, "the prepared resident frame is present");
        if (frame != nullptr) {
            const bool isResident =
                frame->provenance().provider == bloom::runtime::PreviewDisplayProvider::GpuResident;
            expectations.expect(isResident, "the frame is the resident arm: " +
                                                service.status().residentDetail);
            if (isResident) {
                const auto resident = frame->residentFrame();
                expectations.expect(frame->displayBufferView() == std::nullopt,
                                    "the resident frame retains no CPU pixel buffer");
                expectations.expect(!frame->hasProcessFrame(),
                                    "the resident frame retains no process frame");
                expectations.expect(resident->displayProvider() ==
                                        bloom::runtime::PreviewDisplayProvider::GpuResident,
                                    "the resident display provenance is GpuResident");
                expectations.expect(resident->processProvider() ==
                                        bloom::runtime::EvaluationProvider::GpuResident,
                                    "the GPU-evaluated process provenance is GpuResident");
                expectations.expect(resident->lease().isValid(),
                                    "the resident lease is valid and bound to the registry");
                expectations.expect(
                    resident->lease().registryEpoch() ==
                        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::registryEpoch(
                            *core),
                    "the lease is bound to the service registry epoch");
                presentedLease = resident->lease();
            }
        }
    }
    const auto afterFirst = service.status().counters;
    expectations.expect(afterFirst.residentGraphJobs == beforeFirst.residentGraphJobs + 1U,
                        "exactly one resident graph job was admitted");
    expectations.expect(afterFirst.nativeDispatches > beforeFirst.nativeDispatches,
                        "the cold resident graph performed real native operations");
    expectations.expect(afterFirst.fullFrameReadbacks == 0U,
                        "the cold resident graph performed no full-frame readback");
    expectations.expect(afterFirst.displayStatusReads == beforeFirst.displayStatusReads + 1U,
                        "the resident display invalidated exactly its 4-byte status word");

    if (!presentedLease.has_value()) {
        service.beginShutdown();
        return 1;
    }

    // Present the genuine resident lease through the real Wayland client.
    expectations.expect(
        client->update(attached.target, 1U, makeUpdate(*presentedLease, 320U, 240U)).accepted(),
        "the resident present update is admitted");
    expectations.expect(
        waitUntil([&] { return client->status(attached.target).appliedSequence >= 1U; }, 15s),
        "the resident present was applied on the owner thread");
    expectations.expect(client->status(attached.target).presentCount == 1U,
                        "exactly one resident present completed");

    // --- Warm identical request: content-cache reuse, no new native operations -------------------
    const auto beforeWarm = service.status().counters;
    auto warm = service.submit(TaskRequest("resident preview warm", taskOwner), snapshot, identity,
                               kBudget, {});
    expectations.expect(warm.status == bloom::runtime::TaskSubmissionStatus::Accepted,
                        "the warm resident request was admitted");
    const auto warmResult = awaitResult(warm.handle);
    const bool warmPrepared = warmResult.has_value() &&
                              warmResult->state() == TaskState::Succeeded &&
                              warmResult->value().has_value() &&
                              warmResult->value().value()->status() ==
                                  bloom::runtime::PreviewPreparationStatus::Prepared &&
                              warmResult->value().value()->frame() != nullptr &&
                              warmResult->value().value()->frame()->provenance().provider ==
                                  bloom::runtime::PreviewDisplayProvider::GpuResident;
    expectations.expect(warmPrepared, "the warm identical request produced a resident frame");
    const auto afterWarm = service.status().counters;
    expectations.expect(afterWarm.nativeDispatches == beforeWarm.nativeDispatches,
                        "a warm identical request performed zero additional native operations");
    expectations.expect(afterWarm.gpuCacheHits > beforeWarm.gpuCacheHits,
                        "a warm identical request was served from the content cache");

    // --- Unsupported GPU subset: full original CPU path ------------------------------------------
    const auto beforeUnsupported = service.status().counters;
    const auto unsupportedIdentity = makeIdentity(*plan, kGenerationUnsupportedSubset);
    auto unsupported = service.submit(TaskRequest("resident unsupported", taskOwner), snapshot,
                                      unsupportedIdentity, kBudget, {});
    expectations.expect(unsupported.status == bloom::runtime::TaskSubmissionStatus::Accepted,
                        "the unsupported-subset request was admitted");
    const auto unsupportedResult = awaitResult(unsupported.handle);
    const bool unsupportedCpu =
        unsupportedResult.has_value() && unsupportedResult->state() == TaskState::Succeeded &&
        unsupportedResult->value().has_value() &&
        unsupportedResult->value().value()->status() ==
            bloom::runtime::PreviewPreparationStatus::Prepared &&
        unsupportedResult->value().value()->frame() != nullptr &&
        unsupportedResult->value().value()->frame()->provenance().provider !=
            bloom::runtime::PreviewDisplayProvider::GpuResident &&
        unsupportedResult->value().value()->frame()->displayBufferView().has_value();
    expectations.expect(unsupportedCpu,
                        "an unsupported GPU subset took the full CPU path with CPU pixels");
    expectations.expect(service.status().counters.cpuFallbacks > beforeUnsupported.cpuFallbacks,
                        "the unsupported GPU subset incremented the CPU fallback counter");
    expectations.expect(service.status().counters.fullFrameReadbacks == 0U,
                        "the unsupported fallback performed no full-frame readback");

    // --- Deterministic cancellation during the GPU-scene CPU stage -------------------------------
    const auto cancelIdentity = makeIdentity(*plan, kGenerationGated);
    auto gated = service.submit(TaskRequest("resident gated", taskOwner), snapshot, cancelIdentity,
                                kBudget, {});
    expectations.expect(gated.status == bloom::runtime::TaskSubmissionStatus::Accepted,
                        "the gated resident request was admitted");
    expectations.expect(
        waitUntil([&] { return gateEntered->load(std::memory_order_acquire); }, 15s),
        "the GPU-scene CPU stage started on a CPU worker");
    static_cast<void>(gated.handle.cancel());
    gateRelease->store(true, std::memory_order_release);
    const auto gatedResult = awaitResult(gated.handle);
    const bool cancelled = gatedResult.has_value() && gatedResult->state() == TaskState::Cancelled;
    expectations.expect(cancelled, "cancellation during the GPU-scene CPU stage is terminal");

    // --- Stalled/unknown-fence retirement: the parent/admission must be retained until proven ----
    // Force the resident display to report an unretired, permanently-pending submission. The
    // production Retiring code must retain the stage, its completion and the request-owned
    // admission until the bounded retirement budget forces the owner-thread pipeline teardown; it
    // must never complete the parent while a submission may still reference native memory.
    core->residentDisplayUnretiredOverride = [](const bloom::render::GpuResidentDisplay&) noexcept {
        return true;
    };
    core->residentDisplayPollOverride = [](bloom::render::GpuResidentDisplay&) noexcept {
        return bloom::render::GpuResidentDisplayPollResult::Pending;
    };
    auto stalled =
        service.submit(TaskRequest("resident stalled", taskOwner), snapshot, identity, kBudget, {});
    expectations.expect(stalled.status == bloom::runtime::TaskSubmissionStatus::Accepted,
                        "the stalled resident request was admitted");
    expectations.expect(
        waitUntil(
            [&] {
                return bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::residentRetiring(
                    *core);
            },
            30s),
        "the stalled submission entered the explicit Retiring phase");
    // The parent completion and its request-owned admission are still held while Retiring.
    expectations.expect(!stalled.handle.tryTakeResult().has_value(),
                        "the parent completion is retained while the fence is unretired");
    const auto stalledResult = awaitResult(stalled.handle, 30s);
    const bool stalledFellBack =
        stalledResult.has_value() && stalledResult->state() == TaskState::Succeeded &&
        stalledResult->value().has_value() &&
        stalledResult->value().value()->status() ==
            bloom::runtime::PreviewPreparationStatus::Prepared &&
        stalledResult->value().value()->frame() != nullptr &&
        stalledResult->value().value()->frame()->provenance().provider !=
            bloom::runtime::PreviewDisplayProvider::GpuResident &&
        stalledResult->value().value()->frame()->displayBufferView().has_value();
    expectations.expect(stalledFellBack,
                        "the stalled submission was released only via the CPU fallback");
    const auto afterStalled = service.status().counters;
    expectations.expect(afterStalled.retirementUnprovenTeardowns >= 1U,
                        "the bounded retirement budget forced an explicit owner-thread teardown");
    expectations.expect(
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::residentRouteTerminal(*core),
        "an unprovable fence marks the resident route terminal");
    expectations.expect(afterStalled.fullFrameReadbacks == 0U,
                        "the stalled retirement performed no full-frame readback");
    core->residentDisplayUnretiredOverride = {};
    core->residentDisplayPollOverride = {};

    // --- Host-ordered shutdown; the presented lease stays valid until its target retires ---------
    expectations.expect(presentedLease->isValid(),
                        "the presented lease is still valid before retirement");
    static_cast<void>(client->retire(attached.target, 5U));
    expectations.expect(
        waitUntilEvents([&] { return client->status(attached.target).surfaceSafeToDestroy; }, 30s),
        "the resident target proved retirement and its surface is safe to destroy");
    surface.window->hide();
    delete surface.window;
    surface.window = nullptr;

    service.beginShutdown();
    expectations.expect(
        waitUntilEvents(
            [&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; }, 30s),
        "the resident service owner drained and stopped");
    const auto finalStatus = service.status();
    expectations.expect(finalStatus.presentationClient == nullptr,
                        "the stopped resident service publishes no live presentation client");
    expectations.expect(finalStatus.presentationShutdown.drained,
                        "the resident service shutdown drained");
    expectations.expect(finalStatus.presentationShutdown.unprovenTargets == 0U &&
                            finalStatus.presentationShutdown.quarantinedTargets == 0U,
                        "the resident service shutdown retained no unproven target");
    expectations.expect(finalStatus.counters.fullFrameReadbacks == 0U,
                        "the whole resident run performed no full-frame readback");
    expectations.expect(finalStatus.counters.retirementUnprovenTeardowns >= 1U,
                        "the injected stalled fence exercised the bounded unproven-fence teardown");
    expectations.expect(!presentedLease->isValid(),
                        "the lease is invalid after the owning registry is destroyed");
    instance.destroy();

    if (expectations.failures() == 0) {
        std::cout << "PASS: real resident qualification -> resident prepared frame -> GpuResident "
                     "provenance -> Wayland present; warm cache reuse with zero native operations; "
                     "unsupported GPU-subset CPU fallback; cancellation; host-ordered shutdown\n";
    }
    return expectations.failures() == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    try {
        return runTests(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
