// Real Wayland native acceptance for ViewerGpuResidentController: a real
// GpuDevice, a real GpuResidentFrameLeaseRegistry/GpuPresentationCoordinator on
// a dedicated test owner thread, a real resident PreparedPreviewFrame built
// from a genuine lease, and the controller's truthful first-present
// acknowledgement, swapchain-resize gate, and retire/resume gate through the
// existing ViewerGpuPresenter.
//
// The device-loss/quarantine case cannot be induced on this hardware and is
// left unproven.

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/gpu_resident_frame_lease.hpp>
#include <bloom/runtime/gpu_resident_preview_product.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/editor_native_surface.hpp>
#include <bloom/ui/viewer_gpu_resident.hpp>

#include <QApplication>
#include <QColor>
#include <QEventLoop>
#include <QLabel>
#include <QPixmap>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace bloom;

struct TestOptions final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool visible_proof = false;
    bool valid = true;
};

[[nodiscard]] TestOptions parseOptions(const int argc, char** argv) {
    TestOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else if (argument == "--visible-proof") {
            options.visible_proof = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    return options;
}

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAIL: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

void pumpQt(const int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate, const int timeoutMilliseconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        pumpQt(4);
    }
    return predicate();
}

enum class Cmd { None, ProduceLease, SetGate, Barrier };

struct Shared final {
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    Cmd cmd = Cmd::None;
    bool gateValue = false;
    bool cmdDone = false;
    bool initDone = false;
    bool ran = false;
    std::string reason;
    std::shared_ptr<runtime::GpuPresentationClient> client;
    runtime::GpuResidentFrameLease lease;
};

[[nodiscard]] std::optional<render::ImageWindow> makeWindow(const std::int64_t x,
                                                            const std::int64_t y,
                                                            const std::uint64_t width,
                                                            const std::uint64_t height) {
    const auto created = render::ImageWindow::create(x, y, width, height);
    return created ? std::optional(*created.value()) : std::nullopt;
}

constexpr std::uint64_t kBudget = std::uint64_t{1} << 32;

[[nodiscard]] std::shared_ptr<const render::GpuDisplayImage>
produceDisplay(render::GpuSolid& solid, render::GpuResidentDisplay& display,
               Expectations& expectations) {
    const auto dataWindow = makeWindow(0, 0, 16, 8);
    const auto displayWindow = makeWindow(-2, 3, 16, 8);
    const auto aspect = core::PixelAspectRatio::create(4, 3);
    const auto pixel =
        render::solidPixelFromStraightLinearRec709Scene(core::Color4d{0.25, 0.5, 0.75, 1.0});
    if (!pixel || !dataWindow || !displayWindow || !aspect) {
        expectations.expect(false, "the solid primitive and geometry build");
        return nullptr;
    }
    const auto solidBegin = solid.begin(
        render::GpuSolidParameters{*pixel.value(), *dataWindow, *displayWindow, *aspect}, kBudget);
    if (solidBegin.code != render::GpuSolidDiagnosticCode::None) {
        expectations.expect(false, "the solid begin is accepted");
        return nullptr;
    }
    render::GpuSolidPollResult solidPoll = render::GpuSolidPollResult::Pending;
    while (solidPoll == render::GpuSolidPollResult::Pending) {
        solidPoll = solid.poll();
    }
    if (solidPoll != render::GpuSolidPollResult::Ready) {
        expectations.expect(false, "the solid job completes");
        return nullptr;
    }
    auto input = std::make_shared<const render::GpuImage>(solid.takeImage());
    const auto displayBegin = display.begin(input, kBudget);
    if (displayBegin.code != render::GpuResidentDisplayDiagnosticCode::None) {
        expectations.expect(false, "the resident display begin is accepted");
        return nullptr;
    }
    render::GpuResidentDisplayPollResult displayPoll =
        render::GpuResidentDisplayPollResult::Pending;
    while (displayPoll == render::GpuResidentDisplayPollResult::Pending) {
        displayPoll = display.poll();
    }
    if (displayPoll != render::GpuResidentDisplayPollResult::Ready) {
        expectations.expect(false, "the resident display job completes");
        return nullptr;
    }
    auto image = std::make_shared<const render::GpuDisplayImage>(display.takeImage());
    expectations.expect(image->isValid(), "the resident display image is valid");
    return image->isValid() ? image : nullptr;
}

void ownerMain(Shared& shared, const TestOptions& options, Expectations& expectations) {
    render::GpuDeviceCreationOptions creation;
    creation.loader_path = options.loader_path;
    creation.request_presentation = true;
    creation.presentation_platform = render::GpuPresentationPlatform::Wayland;
    auto created = render::GpuDevice::create(creation);
    if (!created) {
        std::lock_guard lock(shared.mutex);
        shared.reason = "no presentable device: " + created.diagnostic.message;
        shared.initDone = true;
        shared.cv.notify_all();
        return;
    }
    auto device = std::move(created.device);
    auto solidResult = render::GpuSolid::create(*device);
    auto displayResult = render::GpuResidentDisplay::create(*device);
    auto registry = runtime::GpuResidentFrameLeaseRegistry::create(*device);
    if (!solidResult || !displayResult || !registry) {
        std::lock_guard lock(shared.mutex);
        shared.reason = "the native pipelines or lease registry could not be created";
        shared.initDone = true;
        shared.cv.notify_all();
        return;
    }
    std::unique_ptr<render::GpuSolid> solid = std::move(solidResult.solid);
    std::unique_ptr<render::GpuResidentDisplay> display = std::move(displayResult.display);
    auto coordinator = std::make_unique<runtime::GpuPresentationCoordinator>(*device, *registry);
    {
        std::lock_guard lock(shared.mutex);
        shared.client = coordinator->client();
        shared.ran = shared.client != nullptr;
        shared.initDone = true;
        shared.cv.notify_all();
    }
    bool gated = false;
    for (;;) {
        Cmd command = Cmd::None;
        bool gateValue = false;
        {
            std::unique_lock lock(shared.mutex);
            shared.cv.wait_for(lock, std::chrono::milliseconds(2),
                               [&] { return shared.stop || shared.cmd != Cmd::None; });
            if (shared.stop) {
                break;
            }
            command = shared.cmd;
            gateValue = shared.gateValue;
            shared.cmd = Cmd::None;
        }
        if (command == Cmd::SetGate) {
            gated = gateValue;
        } else if (command == Cmd::ProduceLease) {
            auto image = produceDisplay(*solid, *display, expectations);
            runtime::GpuResidentFrameLease leaseValue;
            if (image != nullptr) {
                auto published = registry->publish(std::move(image));
                expectations.expect(published.hasValue(), "the resident image publishes a lease");
                leaseValue = published.lease;
            }
            std::lock_guard lock(shared.mutex);
            shared.lease = leaseValue;
        }
        {
            std::lock_guard lock(shared.mutex);
            shared.cmdDone = true;
            shared.cv.notify_all();
        }
        if (!gated) {
            coordinator->pump();
        }
    }
    coordinator->beginShutdown();
    for (int attempt = 0; attempt < 600 && !coordinator->shutdownStatus().drained; ++attempt) {
        coordinator->pump();
    }
    expectations.expect(coordinator->shutdownStatus().drained, "the owner shutdown drained");
}

[[nodiscard]] std::shared_ptr<const runtime::PreparedPreviewFrame>
makeResidentFrame(const runtime::GpuResidentFrameLease& lease) {
    runtime::PreviewRequestIdentity identity;
    identity.requestGeneration = 1;
    const runtime::ProcessFrameIdentity process =
        runtime::GpuResidentDisplayProductRequest{}.processIdentity;
    auto built = runtime::detail::buildResidentPreviewFrame(identity, process, lease, nullptr, {});
    if (!built.has_value()) {
        return nullptr;
    }
    return std::make_shared<const runtime::PreparedPreviewFrame>(std::move(*built));
}

[[nodiscard]] ui::ResidentPresentRequest makeRequest(const double width, const double height) {
    ui::ResidentPresentRequest request;
    request.containerRect = QRectF(0.0, 0.0, width, height);
    request.destination = QRectF(0.0, 0.0, width, height);
    request.devicePixelRatio = 1.0;
    request.channel = render::GpuPresentChannel::Rgba;
    request.background = render::GpuPresentBackground::Solid;
    return request;
}

} // namespace

int main(int argc, char** argv) {
    const TestOptions options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    QApplication application(argc, argv);
    if (options.loader_path.empty()) {
        std::cout << "SKIP: --loader <libvulkan.so.1> is required\n";
        return options.require_device ? 1 : 0;
    }

    Expectations expectations;
    Shared shared;
    std::thread owner(ownerMain, std::ref(shared), options, std::ref(expectations));
    if (!waitUntil(
            [&] {
                std::lock_guard lock(shared.mutex);
                return shared.initDone;
            },
            30'000) ||
        !shared.ran) {
        owner.join();
        std::string reason;
        {
            std::lock_guard lock(shared.mutex);
            reason = shared.reason;
        }
        std::cout << "SKIP: " << (reason.empty() ? "the owner did not initialize" : reason) << '\n';
        return options.require_device ? 1 : 0;
    }
    const auto client = shared.client;

    const auto runCommand = [&](const Cmd command, const bool gateValue = false) {
        {
            std::lock_guard lock(shared.mutex);
            shared.gateValue = gateValue;
            shared.cmdDone = false;
            shared.cmd = command;
        }
        shared.cv.notify_all();
        (void)waitUntil(
            [&] {
                std::lock_guard lock(shared.mutex);
                return shared.cmdDone;
            },
            10'000);
    };

    QWidget host;
    host.resize(800, 600);
    auto* layout = new QVBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    host.show();

    runtime::TaskScheduler scheduler({.cpuWorkerCount = 1,
                                      .blockingIoWorkerCount = 1,
                                      .cpuQueueCapacity = 16,
                                      .blockingIoQueueCapacity = 4,
                                      .terminalHistoryCapacity = 32,
                                      .diagnosticsPerTask = 8,
                                      .groupRegistryCapacity = 8});

    auto controller = std::make_unique<ui::ViewerGpuResidentController>();
    bool fallback = false;
    bool ackSeen = false;
    bool ackPresented = false;
    std::atomic<int> coverCaptures{0};
    controller->setCpuFallback([&fallback] { fallback = true; });
    controller->setPresentAck([&](const bool presented, const std::string&) {
        ackSeen = true;
        ackPresented = presented;
    });
    controller->setCpuCoverSnapshot([&]() -> QPixmap {
        coverCaptures.fetch_add(1);
        QPixmap snapshot(320, 240);
        snapshot.setDevicePixelRatio(2.0);
        snapshot.fill(Qt::red);
        return snapshot;
    });
    ui::ViewerGpuResidentController::Dependencies dependencies;
    dependencies.client = client;
    dependencies.scheduler = &scheduler;
    dependencies.containerParent = &host;
    dependencies.vulkanLoaderPath = options.loader_path.string();
    dependencies.devicePixelRatio = 1.0;
    controller->setDependencies(std::move(dependencies));

    runCommand(Cmd::ProduceLease);
    const runtime::GpuResidentFrameLease lease = shared.lease;
    expectations.expect(lease.isValid(), "a real resident lease is produced");
    const auto frame = makeResidentFrame(lease);
    expectations.expect(frame != nullptr,
                        "a real resident PreparedPreviewFrame builds from the lease");
    if (frame == nullptr) {
        owner.join();
        return 1;
    }

    if (options.visible_proof) {
        // A deliberately delayed first present: the owner pump is gated so the
        // present is admitted but never applied. The native CPU cover (a
        // distinctive marker) must be the visible pixel until the gate opens and
        // the owner publishes a genuine present.
        host.setWindowTitle(QStringLiteral("bloom-resident-visible-proof"));
        controller->setCpuCoverSnapshot([]() -> QPixmap {
            QPixmap snapshot(320, 240);
            snapshot.setDevicePixelRatio(2.0);
            snapshot.fill(QColor(0, 255, 0));
            return snapshot;
        });
        runCommand(Cmd::SetGate, true);
        expectations.expect(controller->present(*frame, makeRequest(320.0, 240.0)),
                            "the delayed first present is admitted");
        expectations.expect(waitUntil(
                                [&] {
                                    controller->poll();
                                    return controller->cpuCoverForTest() != nullptr &&
                                           controller->cpuCoverForTest()->isVisible();
                                },
                                10'000),
                            "the CPU cover is visible during the delayed first present");
        expectations.expect(controller->cpuCoverForTest() != nullptr &&
                                controller->cpuCoverForTest()->size() == QSize(320, 240),
                            "the native CPU cover fills the requested 320x240 present extent "
                            "before ack");
        std::cout << "PHASE_COVER\n" << std::flush;
        pumpQt(2500);
        runCommand(Cmd::SetGate, false);
        expectations.expect(waitUntil(
                                [&] {
                                    controller->poll();
                                    return controller->presentationAcknowledged();
                                },
                                20'000),
                            "the delayed first present is genuinely acknowledged "
                            "after the gate opens");
        std::cout << "PHASE_GPU\n" << std::flush;
        pumpQt(2500);
        // Retire the live target before destroying the presenter (host order).
        bool safe = false;
        static_cast<void>(controller->prepareForMutation(
            99, [&](const std::uint64_t, const ui::EditorNativeSurface::PrepareResult& r) {
                safe = r.safeToMutate;
            }));
        expectations.expect(waitUntil(
                                [&] {
                                    controller->poll();
                                    return safe;
                                },
                                20'000),
                            "the visible-proof surface retired before teardown");
        controller.reset();
        {
            std::lock_guard lock(shared.mutex);
            shared.stop = true;
        }
        shared.cv.notify_all();
        owner.join();
        return expectations.failures() == 0 ? 0 : 1;
    }

    // 1. First present: the mailbox admission is NOT the acknowledgement.
    // presentationAcknowledged()
    //    must become true only after the owner publishes
    //    presentCount/appliedSequence.
    expectations.expect(controller->present(*frame, makeRequest(320.0, 240.0)),
                        "the first present is admitted");
    // Before the owner acknowledges, the native CPU cover must be visible above
    // the native child (the parent backing store alone would be occluded).
    expectations.expect(controller->cpuCoverForTest() != nullptr &&
                            controller->cpuCoverForTest()->isVisible(),
                        "the native CPU cover is visible during the pending first present");
    if (const auto* coverLabel = qobject_cast<const QLabel*>(controller->cpuCoverForTest())) {
        const QPixmap handoff = coverLabel->pixmap(Qt::ReturnByValue);
        expectations.expect(!handoff.isNull(), "the native CPU cover holds the handoff snapshot");
        expectations.expect(qFuzzyCompare(handoff.devicePixelRatio(), 2.0),
                            "the native CPU cover preserves the DPR2 handoff snapshot");
        expectations.expect(handoff.width() == 320 && handoff.height() == 240,
                            "the native CPU cover keeps the full physical pixmap, not a half crop");
        expectations.expect(coverLabel->hasScaledContents(),
                            "the native CPU cover lets QLabel scale the full device pixmap");
    } else {
        expectations.expect(false, "the native CPU cover is a kit QLabel");
    }
    expectations.expect(
        controller->cpuCoverForTest() != nullptr &&
            controller->cpuCoverForTest()->size() == QSize(320, 240),
        "the native CPU cover fills the requested 320x240 present extent before ack");
    expectations.expect(waitUntil(
                            [&] {
                                controller->poll();
                                return controller->presentationAcknowledged();
                            },
                            20'000),
                        "the owner genuinely acknowledged the first present");
    expectations.expect(!controller->cpuCoverForTest()->isVisible(),
                        "the native CPU cover is hidden after the genuine present ack");
    expectations.expect(!controller->cpuCoverVisibleForTest(),
                        "the cover visibility query agrees after the genuine ack");
    // concealCpuCover() must expose the host paint without destroying the surface.
    controller->concealCpuCover();
    expectations.expect(!controller->cpuCoverVisibleForTest(),
                        "concealCpuCover keeps the cover hidden");
    const int capturesAfterFirstAck = coverCaptures.load();
    // Ordinary native presents (a new frame / overlay / pan / zoom) must NOT
    // recreate the cover or re-snapshot the UI; the last acknowledged swapchain
    // image is kept until the new present.
    for (int index = 0; index < 3; ++index) {
        expectations.expect(controller->present(*frame, makeRequest(320.0, 240.0)),
                            "an ordinary native present is admitted");
        expectations.expect(waitUntil(
                                [&] {
                                    controller->poll();
                                    return controller->presentationAcknowledged();
                                },
                                20'000),
                            "an ordinary native present is acknowledged");
    }
    expectations.expect(coverCaptures.load() == capturesAfterFirstAck,
                        "no cover capture happens across ordinary native presents");
    expectations.expect(ackSeen && ackPresented, "the present ack reported a real present");
    expectations.expect(controller->presentedSequence() >= 1U, "a real present sequence advanced");
    expectations.expect(controller->container() != nullptr, "the native container exists");
    expectations.expect(controller->container()->parent() == &host,
                        "the container was parented before first attach (no live reparent)");

    // 2. Swapchain resize gate: resize retires the old extent and present resumes
    // on the new one.
    expectations.expect(controller->requestTargetResize(640U, 480U), "the resize gate is admitted");
    expectations.expect(waitUntil(
                            [&] {
                                controller->poll();
                                return controller->presenterState() ==
                                       ui::ViewerGpuPresenter::State::Active;
                            },
                            20'000),
                        "the target becomes Active again after the resize");
    expectations.expect(controller->present(*frame, makeRequest(640.0, 480.0)),
                        "present resumes at the new extent");
    expectations.expect(waitUntil(
                            [&] {
                                controller->poll();
                                return controller->presentationAcknowledged();
                            },
                            20'000),
                        "the post-resize present is acknowledged");

    // 3. Retire-before-mutation gate and resume on the surviving tree.
    bool mutationSafe = false;
    const auto outcome = controller->prepareForMutation(
        7, [&](const std::uint64_t generation, const ui::EditorNativeSurface::PrepareResult& r) {
            expectations.expect(generation == 7, "the mutation generation is echoed");
            mutationSafe = r.safeToMutate;
        });
    expectations.expect(outcome == ui::EditorNativeSurface::PrepareOutcome::RetirePending ||
                            outcome == ui::EditorNativeSurface::PrepareOutcome::NoLiveTarget,
                        "the mutation gate is admitted");
    expectations.expect(waitUntil(
                            [&] {
                                controller->poll();
                                return mutationSafe;
                            },
                            20'000),
                        "the surface genuinely retired before mutation");
    controller->resumeAfterMutation();
    expectations.expect(controller->present(*frame, makeRequest(320.0, 240.0)),
                        "a surviving editor re-presents after resume");
    expectations.expect(waitUntil(
                            [&] {
                                controller->poll();
                                return controller->presentationAcknowledged();
                            },
                            20'000),
                        "the resumed present is acknowledged");

    // 4. Settle the live target before destroying the controller
    // (retire-before-mutation contract).
    bool finalSafe = false;
    static_cast<void>(controller->prepareForMutation(
        8, [&](const std::uint64_t, const ui::EditorNativeSurface::PrepareResult& r) {
            finalSafe = r.safeToMutate;
        }));
    expectations.expect(waitUntil(
                            [&] {
                                controller->poll();
                                return finalSafe;
                            },
                            20'000),
                        "the final surface retirement is proven before teardown");

    static_cast<void>(fallback);
    // Destroy the controller and its native surface while the presentation owner
    // is still alive; the borrowed VkInstance is only valid until the owner
    // thread ends.
    controller.reset();
    {
        std::lock_guard lock(shared.mutex);
        shared.stop = true;
    }
    shared.cv.notify_all();
    owner.join();

    if (expectations.failures() == 0) {
        std::cout << "PASS: native ViewerGpuResidentController "
                     "present/ack/resize/resume\n";
        return 0;
    }
    std::cerr << expectations.failures() << " failure(s)\n";
    return 1;
}
