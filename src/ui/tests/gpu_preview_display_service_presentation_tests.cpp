// Real Wayland acceptance for GpuPreviewDisplayService presentation ownership. The service owns its
// own thread, device, GpuResidentFrameLeaseRegistry, and GpuPresentationCoordinator; the UI thread
// owns only a QVulkanInstance and a QWindow and drives the Qt-free presentation client. The test
// never touches a native target directly, and the only native work is real: a Wayland swapchain on
// the service device and a real resident image produced through an actual scheduler GPU task.
//
// --loader pins an explicit loader; --require-device fails closed without a compatible presentable
// device. When presentation is genuinely unavailable the test proves the CPU/packed fallback takes
// no blank activation claim instead of fabricating one.

#include "gpu_preview_display_service_private.hpp"

#include "gpu_borrowed_instance.hpp"
#include "gpu_native_test_environment.hpp"

#include <bloom/core/color.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_resident_frame_lease.hpp>
#include <bloom/runtime/preview_cpu_stage.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QGuiApplication>
#include <QVulkanInstance>
#include <QWindow>

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
#include <sys/resource.h>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using bloom::render::GpuBorrowedInstanceView;
using bloom::render::GpuBorrowedSurface;
using bloom::render::GpuPresentBackground;
using bloom::render::GpuPresentChannel;
using bloom::render::GpuPresentImageParams;
using bloom::render::GpuPresentRect;
using bloom::render::GpuPresentSourceWindow;
using bloom::runtime::GpuPresentationClient;
using bloom::runtime::GpuPresentationPortCode;
using bloom::runtime::GpuPresentationTargetId;
using bloom::runtime::GpuPresentationTargetState;
using bloom::runtime::GpuPresentationUpdate;
using bloom::runtime::GpuPreviewDisplayService;
using bloom::runtime::GpuPreviewDisplayServiceOptions;
using bloom::runtime::GpuPreviewDisplayServicePresentationMode;
using bloom::runtime::GpuPreviewDisplayServiceState;
using bloom::runtime::GpuResidentFrameLease;
using bloom::runtime::PreviewCpuStageFunction;
using bloom::runtime::PreviewPreparationResultHandle;
using bloom::runtime::TaskContext;
using bloom::runtime::TaskHandle;
using bloom::runtime::TaskRequest;
using bloom::runtime::TaskResult;
using bloom::runtime::TaskScheduler;
using bloom::runtime::TaskSchedulerConfig;
using bloom::runtime::TaskState;

constexpr std::size_t kBudget = std::size_t{1} << 28;

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
    config.cpuWorkerCount = 1;
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

// The ordinary preview request used to prove coexistence. It is deliberately non-neutral (a default
// identity) so it takes the existing CPU/packed admission path; the stage reports Unsupported,
// which is a successful terminal outcome, never a fabricated frame.
[[nodiscard]] PreviewCpuStageFunction unsupportedStage() {
    return [](const bloom::document::Snapshot&, const bloom::runtime::PreviewRequestIdentity&,
              std::size_t, const std::vector<bloom::runtime::SnapshotParameterOverride>&,
              TaskContext&) -> TaskResult<bloom::runtime::PreviewCpuStageOutcomeHandle> {
        auto outcome = std::make_shared<const bloom::runtime::PreviewCpuStageOutcome>();
        return TaskResult<bloom::runtime::PreviewCpuStageOutcomeHandle>::succeeded(
            std::move(outcome));
    };
}

[[nodiscard]] bloom::runtime::PreviewCpuDisplayFallback unreachableFallback() {
    return [](const bloom::runtime::PreviewCpuStage&,
              TaskContext&) -> TaskResult<PreviewPreparationResultHandle> {
        return TaskResult<PreviewPreparationResultHandle>::failed(
            std::vector<bloom::runtime::TaskDiagnostic>{});
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

// Waits while driving the Qt event loop: a Wayland present completion/present fence can require the
// UI event queue to be dispatched, so a shutdown wait must not block it.
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
awaitResult(const TaskHandle<Value>& handle, const std::chrono::milliseconds timeout = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = handle.tryTakeResult()) {
            return result;
        }
        std::this_thread::sleep_for(500us);
    }
    return std::nullopt;
}

// Isolated-fixture total-process CPU. The caller measures this across a quiet window in which the
// UI thread only sleeps, so a busy owner service thread would show up as roughly one full core.
[[nodiscard]] double processCpuSeconds() {
    struct rusage usage{};
    static_cast<void>(getrusage(RUSAGE_SELF, &usage));
    return static_cast<double>(usage.ru_utime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec) * 1e-6 +
           static_cast<double>(usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_stime.tv_usec) * 1e-6;
}

[[nodiscard]] bloom::document::Snapshot makeSnapshot(const std::uint64_t id, std::string name) {
    bloom::document::Document document(
        bloom::document::Project(bloom::document::ProjectId::fromRaw(id), std::move(name)));
    return document.snapshot();
}

[[nodiscard]] GpuPresentationUpdate makeUpdate(const GpuResidentFrameLease& lease,
                                               const std::uint32_t width,
                                               const std::uint32_t height) {
    GpuPresentationUpdate update;
    update.lease = lease;
    update.params.targetWidth = width;
    update.params.targetHeight = height;
    update.params.destination =
        GpuPresentRect{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)};
    update.params.source = GpuPresentSourceWindow{0.0, 0.0, 16.0, 8.0};
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

// Concurrency check: the ordinary CPU/packed request must complete while the presentation target is
// live on the same service, using the same root admission and cancellation contract.
[[nodiscard]] bool submitOrdinaryPreview(GpuPreviewDisplayService& service,
                                         Expectations& expectations) {
    const auto snapshot = makeSnapshot(7U, "service-presentation-test");
    bloom::runtime::TaskOwner taskOwner;
    taskOwner.kind = bloom::runtime::TaskOwnerKind::Composition;
    taskOwner.id = bloom::runtime::TaskOwnerId::fromRaw(7);
    auto submission = service.submit(TaskRequest("ordinary preview", taskOwner), snapshot,
                                     bloom::runtime::PreviewRequestIdentity{}, kBudget, {});
    if (submission.status != bloom::runtime::TaskSubmissionStatus::Accepted) {
        expectations.expect(false, "the ordinary preview request was admitted");
        return false;
    }
    const auto result = awaitResult(submission.handle);
    const auto* carried =
        result.has_value() && result->state() == TaskState::Succeeded ? &result->value() : nullptr;
    const bool finished = carried != nullptr && carried->has_value();
    expectations.expect(finished, "the ordinary preview request completed on the CPU path");
    if (carried != nullptr && carried->has_value()) {
        expectations.expect(
            (**carried)->status() == bloom::runtime::PreviewPreparationStatus::Unsupported,
            "the non-neutral ordinary request made no GPU/presentation activation claim");
    }
    return finished;
}

// Proves the missing-presentation path in-process (no skip): a service that requested Wayland but
// could not start a device publishes no client, never claims Ready, and still serves the ordinary
// CPU/packed request.
[[nodiscard]] int provePortableFallback() {
    Expectations expectations;
    TaskScheduler scheduler(schedulerConfig());
    GpuPreviewDisplayServiceOptions options;
    options.enabled = true;
    options.loaderPath = "/nonexistent/bloom-loader.so";
    options.presentation = GpuPreviewDisplayServicePresentationMode::Wayland;
    options.previewByteAllowance = kBudget;
    GpuPreviewDisplayService service(scheduler, unsupportedStage(), unreachableFallback(), options);
    static_cast<void>(waitUntil(
        [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; },
        30s));
    const auto status = service.status();
    expectations.expect(status.presentationClient == nullptr,
                        "an unavailable presentation publishes no client");
    expectations.expect(status.presentationAvailability !=
                            bloom::render::GpuPresentationAvailability::Ready,
                        "an unavailable presentation never claims Ready/blank activation");
    static_cast<void>(submitOrdinaryPreview(service, expectations));
    service.beginShutdown();
    return expectations.failures();
}

} // namespace

int runTests(int argc, char** argv) {
    const TestOptions options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    const auto native_environment = bloom::ui::test::NativeWaylandEnvironment::inspect();
    if (!native_environment.available()) {
        return native_environment.exitStatus(options.require_device);
    }
    QGuiApplication application(argc, argv);
    if (options.loader_path.empty()) {
        std::cout << "SKIP: --loader <libvulkan.so.1> is required\n";
        return options.require_device ? 1 : 0;
    }
    qputenv("QT_VULKAN_LIB", options.loader_path.string().c_str());

    Expectations expectations;
    // In-process, always-run proof that a requested-but-unavailable presentation takes the
    // CPU/packed path and claims no blank activation.
    expectations.expect(provePortableFallback() == 0,
                        "the missing-presentation CPU/packed fallback is proven in-process");
    TaskScheduler scheduler(schedulerConfig());
    GpuPreviewDisplayService service(scheduler, unsupportedStage(), unreachableFallback(),
                                     serviceOptions(options.loader_path));

    const bool terminal = waitUntil(
        [&] {
            const auto state = service.status().state;
            return state != GpuPreviewDisplayServiceState::Initializing;
        },
        60s);
    auto status = service.status();
    const bool presentationReady =
        terminal && status.presentationClient != nullptr &&
        status.presentationAvailability == bloom::render::GpuPresentationAvailability::Ready;
    if (!presentationReady) {
        // No blank activation claim: with no live presentation generation the host must use the
        // CPU/packed path. Verify the ordinary path still completes.
        static_cast<void>(submitOrdinaryPreview(service, expectations));
        service.beginShutdown();
        expectations.expect(!options.require_device,
                            "a presentable Wayland device is required but unavailable: " +
                                status.presentationDetail);
        if (expectations.failures() == 0) {
            std::cout << "SKIP: presentation unavailable; CPU/packed fallback verified\n";
        }
        return expectations.failures() == 0 ? 0 : 1;
    }

    const auto core = bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    expectations.expect(core != nullptr, "the service core is reachable");
    if (core == nullptr) {
        return 1;
    }
    expectations.expect(
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::presentationReady(*core),
        "the owner created the registry and coordinator on the service device/thread");
    const std::uint64_t ownershipEpoch =
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::ownershipEpoch(*core);
    expectations.expect(ownershipEpoch != 0U, "the device has a nonzero ownership epoch");

    const std::shared_ptr<GpuPresentationClient> client = status.presentationClient;
    const GpuBorrowedInstanceView view = client->instanceView();
    expectations.expect(view.valid, "the service client publishes the borrowed instance view");
    expectations.expect(view.epoch.value == ownershipEpoch,
                        "the published view is the service device's own ownership epoch");
    if (!view.valid) {
        return 1;
    }
    QVulkanInstance instance;
    instance.setVkInstance(bloom::ui::test::borrowedInstance(view.instance_bits));
    if (!instance.create() || !instance.isValid()) {
        std::cout << "SKIP: QVulkanInstance could not adopt the borrowed instance\n";
        return options.require_device ? 1 : 0;
    }

    const auto createSurface = [&](const std::uint32_t width, const std::uint32_t height) {
        UiSurface surface;
        surface.window = new QWindow();
        surface.window->setSurfaceType(QSurface::VulkanSurface);
        surface.window->setVulkanInstance(&instance);
        surface.window->resize(static_cast<int>(width), static_cast<int>(height));
        surface.window->show();
        surface.width = width;
        surface.height = height;
        if (!waitUntil(
                [&surface] {
                    surface.surface = QVulkanInstance::surfaceForWindow(surface.window);
                    return surface.surface != VK_NULL_HANDLE;
                },
                10s)) {
            return std::optional<UiSurface>{};
        }
        return std::optional<UiSurface>{surface};
    };

    auto mainSurface = createSurface(320U, 240U);
    expectations.expect(mainSurface.has_value(), "the UI QWindow produced a Wayland VkSurfaceKHR");
    if (!mainSurface) {
        return 1;
    }
    GpuBorrowedSurface borrowed;
    borrowed.surface_bits = mainSurface->bits();
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

    // Real owner-thread resident image through an actual scheduler GPU task.
    bloom::runtime::detail::PresentationTestLeaseResult leaseResult;
    const bool leaseOk =
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::requestPresentationTestLease(
            service, leaseResult, 30s);
    expectations.expect(leaseOk && leaseResult.ran && leaseResult.lease.isValid(),
                        "the owner published a real resident lease on the service owner thread "
                        "(the owner-thread check is enforced inside the GPU task): " +
                            leaseResult.diagnostic);
    if (!leaseOk || !leaseResult.lease.isValid()) {
        return 1;
    }
    const GpuResidentFrameLease lease = leaseResult.lease;
    expectations.expect(
        lease.registryEpoch() ==
            bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::registryEpoch(*core),
        "the lease is bound to the service registry epoch");

    expectations.expect(
        client->update(attached.target, 1U, makeUpdate(lease, 320U, 240U)).accepted(),
        "the first present update is admitted");
    expectations.expect(
        waitUntil([&] { return client->status(attached.target).appliedSequence >= 1U; }, 15s),
        "the first present was applied on the owner thread");
    expectations.expect(client->status(attached.target).presentCount == 1U,
                        "exactly one present completed");

    // Resize through the real QWindow and present again.
    mainSurface->window->resize(400, 300);
    for (int tick = 0; tick < 100; ++tick) {
        QGuiApplication::processEvents();
        std::this_thread::sleep_for(5ms);
    }
    expectations.expect(
        client->update(attached.target, 2U, makeUpdate(lease, 400U, 300U)).accepted(),
        "the resize present update is admitted");
    expectations.expect(
        waitUntil([&] { return client->status(attached.target).appliedSequence >= 2U; }, 15s),
        "the resize present was applied");

    // A concurrent ordinary preview request on the same service, admitted before shutdown.
    static_cast<void>(submitOrdinaryPreview(service, expectations));

    // Foreign-registry lease rejection: a valid lease from a second registry on the same device
    // must not advance the present count and must not quarantine the target.
    const std::uint64_t presentsBefore = client->status(attached.target).presentCount;
    bloom::runtime::detail::PresentationTestLeaseResult foreignResult;
    const bool foreignOk =
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::requestPresentationTestLease(
            service, foreignResult, 30s, /*foreign=*/true);
    expectations.expect(foreignOk && foreignResult.lease.isValid(),
                        "the owner published a valid foreign-registry lease");
    if (foreignOk && foreignResult.lease.isValid()) {
        expectations.expect(
            client->update(attached.target, 3U, makeUpdate(foreignResult.lease, 320U, 240U))
                .accepted(),
            "the foreign lease update is admitted and rejected on the owner pump");
        std::this_thread::sleep_for(100ms);
        const auto snapshot = client->status(attached.target);
        expectations.expect(snapshot.presentCount == presentsBefore,
                            "a foreign-registry lease did not present");
        expectations.expect(snapshot.state == GpuPresentationTargetState::Active,
                            "a rejected foreign lease did not quarantine the target");
    }

    // Idle acceptance: frames presented, target Active, no pending request. The owner must wait on
    // its wake hook rather than poll at full rate. The UI thread only sleeps here, so the isolated
    // fixture's total process CPU over the quiet window is a direct busy-spin probe.
    const double cpuBeforeIdle = processCpuSeconds();
    const auto idleStart = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(2500ms);
    const auto idleEnd = std::chrono::steady_clock::now();
    const double idleCpuSeconds = processCpuSeconds() - cpuBeforeIdle;
    const double idleWallSeconds = std::chrono::duration<double>(idleEnd - idleStart).count();
    const double idleCoreFraction = idleCpuSeconds / idleWallSeconds;
    expectations.expect(idleCoreFraction < 0.05,
                        "the idle Active target owner used <5% of one core (" +
                            std::to_string(idleCoreFraction * 100.0) + "% over " +
                            std::to_string(idleWallSeconds) + "s)");
    expectations.expect(client->status(attached.target).state == GpuPresentationTargetState::Active,
                        "the idle target stayed Active across the quiet window");
    std::cout << "idle owner CPU: " << idleCoreFraction * 100.0 << "% of one core over "
              << idleWallSeconds << "s\n";

    // Prompt progress after idle: a new frame is woken and applied within a generous timeout, so a
    // missing wake surfaces as a hang rather than a tight latency race.
    expectations.expect(
        client->update(attached.target, 5U, makeUpdate(lease, 400U, 300U)).accepted(),
        "a new frame after idle is admitted");
    expectations.expect(
        waitUntil([&] { return client->status(attached.target).appliedSequence >= 5U; }, 15s),
        "the new frame after idle was applied (the idle wait was woken, not missed)");

    // Cancel path after idle. Whether the cancel wins the race against the owner pump or reports a
    // stale sequence, the non-brittle contract is: an accepted cancel never presents, and a raced
    // cancel is reported as stale.
    const std::uint64_t presentsBeforeCancel = client->status(attached.target).presentCount;
    expectations.expect(
        client->update(attached.target, 6U, makeUpdate(lease, 400U, 300U)).accepted(),
        "the to-be-cancelled update is admitted");
    const auto cancelResult = client->cancel(attached.target, 6U);
    if (cancelResult.code == GpuPresentationPortCode::Accepted) {
        std::this_thread::sleep_for(200ms);
        expectations.expect(client->status(attached.target).presentCount == presentsBeforeCancel,
                            "an accepted cancel suppressed its present");
    } else {
        expectations.expect(cancelResult.code == GpuPresentationPortCode::StaleSequence,
                            "a cancel that lost the race reports a stale sequence");
    }
    expectations.expect(client->status(attached.target).state == GpuPresentationTargetState::Active,
                        "the cancel path did not quarantine the target");

    // Documented host order: retire the first live target and prove its surface safe to destroy
    // while the service is still running.
    static_cast<void>(client->retire(attached.target, 4U));
    expectations.expect(
        waitUntilEvents([&] { return client->status(attached.target).surfaceSafeToDestroy; }, 30s),
        "the target proved retirement and its surface is safe to destroy");
    mainSurface->window->hide();
    delete mainSurface->window;
    mainSurface.reset();

    // The host contract is documented and enforced by this ordering: every Qt surface is retired
    // and destroyed before the irreversible service shutdown begins. (Starting the service's
    // shutdown with a live Qt surface would leave the UI unable to destroy that surface once the
    // owner device is gone; that is the hazard the contract exists to prevent.)
    service.beginShutdown();
    auto rejectedSubmission = service.submit(
        TaskRequest("post-shutdown preview", bloom::runtime::TaskOwner{}),
        makeSnapshot(8U, "svc-shutdown"), bloom::runtime::PreviewRequestIdentity{}, kBudget, {});
    expectations.expect(rejectedSubmission.status != bloom::runtime::TaskSubmissionStatus::Accepted,
                        "beginShutdown refuses a new service root");
    expectations.expect(
        waitUntilEvents([&] { return !service.status().presentationShutdown.accepting; }, 15s),
        "the owner published a non-accepting presentation shutdown snapshot");

    // A new attach after shutdown must be refused, not admitted. A nonzero fake surface is enough:
    // the client refuses before it ever validates or touches the borrowed surface, and no QWindow
    // is created after shutdown.
    GpuBorrowedSurface refused;
    refused.surface_bits = 0x5EEDU;
    refused.epoch = view.epoch;
    const auto refusedAttach = client->attach(refused, 64U, 64U);
    expectations.expect(refusedAttach.code == GpuPresentationPortCode::ShuttingDown ||
                            refusedAttach.code == GpuPresentationPortCode::OwnerGone,
                        "attach after beginShutdown is refused, never admitted");

    expectations.expect(
        waitUntilEvents(
            [&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; }, 30s),
        "the service owner drained and stopped");
    const auto finalStatus = service.status();
    expectations.expect(finalStatus.presentationClient == nullptr,
                        "the stopped service publishes no live presentation client");
    const auto& shutdown = finalStatus.presentationShutdown;
    const std::string counts = " (drained=" + std::to_string(shutdown.drained ? 1 : 0) +
                               " retired=" + std::to_string(shutdown.retiredTargets) +
                               " unproven=" + std::to_string(shutdown.unprovenTargets) +
                               " quarantined=" + std::to_string(shutdown.quarantinedTargets) + ")";
    expectations.expect(shutdown.drained, "the clean, host-ordered shutdown drained" + counts);
    expectations.expect(shutdown.unprovenTargets == 0U && shutdown.quarantinedTargets == 0U,
                        "a clean shutdown retained no unproven target" + counts);
    expectations.expect(bloom::runtime::GpuPresentationCoordinator::quarantinedGenerationCount() ==
                            0U,
                        "no native generation was quarantined on a clean shutdown");
    instance.destroy();
    if (expectations.failures() == 0) {
        std::cout << "PASS: service-owned Wayland attach/present/resize/retire with a concurrent "
                     "ordinary preview request, foreign-lease rejection, and bounded shutdown\n";
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
