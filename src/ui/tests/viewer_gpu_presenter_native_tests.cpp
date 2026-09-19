// Real Wayland Qt acceptance fixture for ViewerGpuPresenter.
//
// A dedicated TEST owner thread owns the GpuDevice, the GpuResidentFrameLeaseRegistry, and the
// presentation coordinator, and drives pump(). The Qt/UI thread owns the QApplication, every
// QVulkanInstance (adopted from the coordinator client's borrowed instance), every QWindow, and
// every createWindowContainer widget. All commands cross the existing Qt-free/Vulkan-free runtime
// port; the test never touches a native target directly.
//
// What this proves (not a fake port):
//   * embed a real Vulkan QWindow inside a QWidget, attach a real target, present a real resident
//     lease, resize the real window, present again, then retire and only after surfaceSafeToDestroy
//     does the UI mutate/destroy the containing widget;
//   * the VkSurfaceKHR identity is stable across a resize (no silent recreation);
//   * a close/retire in flight never acks before the owner's proven Retired;
//   * reattachment after approved retirement gets a fresh target id and sequence;
//   * two independent viewers present one shared lease on one device;
//   * a missing client is an explicit Unsupported diagnostic with no window;
//   * the input callback sees real QTest mouse/wheel/key coordinates.
//
// The device-loss/quarantine case cannot be induced on this hardware and is left unproven rather
// than faked.

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_present_image.hpp>
#include <bloom/render/gpu_presentation_target.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/gpu_resident_frame_lease.hpp>
#include <bloom/ui/viewer_gpu_presenter.hpp>

#include <QApplication>
#include <QEventLoop>
#include <QPoint>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <QtTest/QtTest>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::core::PixelAspectRatio;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDisplayImage;
using bloom::render::GpuImage;
using bloom::render::GpuPresentationPlatform;
using bloom::render::GpuPresentBackground;
using bloom::render::GpuPresentChannel;
using bloom::render::GpuPresentImageParams;
using bloom::render::GpuPresentRect;
using bloom::render::GpuPresentSourceWindow;
using bloom::render::GpuResidentDisplay;
using bloom::render::GpuResidentDisplayDiagnosticCode;
using bloom::render::GpuResidentDisplayPollResult;
using bloom::render::GpuSolid;
using bloom::render::GpuSolidDiagnosticCode;
using bloom::render::GpuSolidParameters;
using bloom::render::GpuSolidPollResult;
using bloom::render::ImageWindow;
using bloom::runtime::GpuPresentationClient;
using bloom::runtime::GpuPresentationCoordinator;
using bloom::runtime::GpuPresentationCoordinatorOptions;
using bloom::runtime::GpuPresentationTargetId;
using bloom::runtime::GpuPresentationTargetState;
using bloom::runtime::GpuResidentFrameLease;
using bloom::runtime::GpuResidentFrameLeaseRegistry;
using bloom::ui::ViewerGpuInputEvent;
using bloom::ui::ViewerGpuInputKind;
using bloom::ui::ViewerGpuPresenter;

constexpr std::uint64_t kBudget = std::uint64_t{1} << 32;

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
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    return options;
}

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAIL: " << message << " (" << location.file_name() << ':' << location.line()
                  << ")\n";
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
    std::shared_ptr<GpuPresentationClient> client;
    GpuResidentFrameLease lease;
};

[[nodiscard]] std::optional<ImageWindow> makeWindow(const std::int64_t x, const std::int64_t y,
                                                    const std::uint64_t width,
                                                    const std::uint64_t height) {
    const auto created = ImageWindow::create(x, y, width, height);
    return created ? std::optional(*created.value()) : std::nullopt;
}

[[nodiscard]] std::shared_ptr<const GpuDisplayImage>
produceDisplay(GpuSolid& solid, GpuResidentDisplay& display, Expectations& expectations) {
    const auto dataWindow = makeWindow(0, 0, 16, 8);
    const auto displayWindow = makeWindow(-2, 3, 16, 8);
    const auto aspect = PixelAspectRatio::create(4, 3);
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.25, 0.5, 0.75, 1.0});
    if (!pixel || !dataWindow || !displayWindow || !aspect) {
        expectations.expect(false, "the solid primitive and geometry build");
        return nullptr;
    }
    const auto solidBegin = solid.begin(
        GpuSolidParameters{*pixel.value(), *dataWindow, *displayWindow, *aspect}, kBudget);
    if (solidBegin.code != GpuSolidDiagnosticCode::None) {
        expectations.expect(false, "the solid begin is accepted: " + solidBegin.message);
        return nullptr;
    }
    GpuSolidPollResult solidPoll = GpuSolidPollResult::Pending;
    while (solidPoll == GpuSolidPollResult::Pending) {
        solidPoll = solid.poll();
    }
    if (solidPoll != GpuSolidPollResult::Ready) {
        expectations.expect(false, "the solid job completes");
        return nullptr;
    }
    auto input = std::make_shared<const GpuImage>(solid.takeImage());
    const auto displayBegin = display.begin(input, kBudget);
    if (displayBegin.code != GpuResidentDisplayDiagnosticCode::None) {
        expectations.expect(false, "the resident display begin is accepted");
        return nullptr;
    }
    GpuResidentDisplayPollResult displayPoll = GpuResidentDisplayPollResult::Pending;
    while (displayPoll == GpuResidentDisplayPollResult::Pending) {
        displayPoll = display.poll();
    }
    if (displayPoll != GpuResidentDisplayPollResult::Ready) {
        expectations.expect(false, "the resident display job completes");
        return nullptr;
    }
    auto image = std::make_shared<const GpuDisplayImage>(display.takeImage());
    expectations.expect(image->isValid(), "the resident display image is valid");
    return image->isValid() ? image : nullptr;
}

void ownerMain(Shared& shared, const TestOptions options, Expectations& expectations) {
    GpuDeviceCreationOptions creation;
    creation.loader_path = options.loader_path;
    creation.request_presentation = true;
    creation.presentation_platform = GpuPresentationPlatform::Wayland;
    auto created = GpuDevice::create(creation);
    if (!created) {
        std::lock_guard lock(shared.mutex);
        shared.reason = "no presentable device: " + created.diagnostic.message;
        shared.initDone = true;
        shared.cv.notify_all();
        return;
    }
    auto device = std::move(created.device);
    if (device->presentationStatus().availability !=
        bloom::render::GpuPresentationAvailability::Ready) {
        std::lock_guard lock(shared.mutex);
        shared.reason = "presentation not ready: " + device->presentationStatus().detail;
        shared.initDone = true;
        shared.cv.notify_all();
        return;
    }
    auto solidResult = GpuSolid::create(*device);
    auto displayResult = GpuResidentDisplay::create(*device);
    auto registry = GpuResidentFrameLeaseRegistry::create(*device);
    if (!solidResult || !displayResult || !registry) {
        std::lock_guard lock(shared.mutex);
        shared.reason = "the native pipelines or lease registry could not be created";
        shared.initDone = true;
        shared.cv.notify_all();
        return;
    }
    std::unique_ptr<GpuSolid> solid = std::move(solidResult.solid);
    std::unique_ptr<GpuResidentDisplay> display = std::move(displayResult.display);
    GpuPresentationCoordinatorOptions coordinatorOptions;
    coordinatorOptions.maxTargets = 6U;
    // Small on purpose: the adapter must forget() every proven-terminal record.
    coordinatorOptions.maxRetainedTargets = 3U;
    auto coordinator =
        std::make_unique<GpuPresentationCoordinator>(*device, *registry, coordinatorOptions);
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
        bool acknowledged = false;
        switch (command) {
        case Cmd::SetGate:
            gated = gateValue;
            acknowledged = true;
            break;
        case Cmd::Barrier:
            acknowledged = true;
            break;
        case Cmd::ProduceLease: {
            auto image = produceDisplay(*solid, *display, expectations);
            GpuResidentFrameLease leaseValue;
            if (image != nullptr) {
                auto published = registry->publish(std::move(image));
                expectations.expect(published.hasValue(),
                                    "the resident image is published as a lease");
                leaseValue = published.lease;
            }
            std::lock_guard lock(shared.mutex);
            shared.lease = leaseValue;
            acknowledged = true;
            break;
        }
        case Cmd::None:
            break;
        }
        if (acknowledged) {
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
    expectations.expect(coordinator->shutdownStatus().drained,
                        "the owner shutdown drained every target");
    std::lock_guard lock(shared.mutex);
    shared.reason = coordinator->shutdownStatus().message;
}

[[nodiscard]] int skipOrFail(const TestOptions& options) { return options.require_device ? 1 : 0; }

struct CapturedInput final {
    std::mutex mutex;
    std::vector<ViewerGpuInputEvent> events;
    void append(const ViewerGpuInputEvent& event) {
        std::lock_guard lock(mutex);
        events.push_back(event);
    }
    [[nodiscard]] std::size_t count(const ViewerGpuInputKind kind) {
        std::lock_guard lock(mutex);
        std::size_t total = 0;
        for (const auto& event : events) {
            if (event.kind == kind) {
                ++total;
            }
        }
        return total;
    }
    [[nodiscard]] std::optional<ViewerGpuInputEvent> last(const ViewerGpuInputKind kind) {
        std::lock_guard lock(mutex);
        std::optional<ViewerGpuInputEvent> found;
        for (const auto& event : events) {
            if (event.kind == kind) {
                found = event;
            }
        }
        return found;
    }
};

} // namespace

int main(int argc, char** argv) {
    const TestOptions options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    QApplication application(argc, argv);
    if (options.loader_path.empty()) {
        std::cout << "SKIP: --loader <libvulkan.so.1> is required\n";
        return skipOrFail(options);
    }

    Expectations expectations;
    Shared shared;
    std::thread owner(ownerMain, std::ref(shared), options, std::ref(expectations));

    const bool initialized = waitUntil(
        [&shared] {
            std::lock_guard lock(shared.mutex);
            return shared.initDone;
        },
        30'000);
    if (!initialized || !shared.ran) {
        owner.join();
        std::string reason;
        {
            std::lock_guard lock(shared.mutex);
            reason = shared.reason;
        }
        std::cout << "SKIP: " << (reason.empty() ? "the owner did not initialize" : reason) << '\n';
        return skipOrFail(options);
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
            [&shared] {
                std::lock_guard lock(shared.mutex);
                return shared.cmdDone;
            },
            10'000);
    };
    const auto waitActive = [&](ViewerGpuPresenter& presenter, const int timeout) {
        return waitUntil([&] { return presenter.state() == ViewerGpuPresenter::State::Active; },
                         timeout);
    };
    const auto makeParams = [](const std::uint32_t width, const std::uint32_t height) {
        GpuPresentImageParams params;
        params.targetWidth = width;
        params.targetHeight = height;
        params.destination =
            GpuPresentRect{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)};
        params.source = GpuPresentSourceWindow{0.0, 0.0, 16.0, 8.0};
        params.channel = GpuPresentChannel::Rgba;
        params.background = GpuPresentBackground::Solid;
        return params;
    };

    // 1. Missing client is an explicit Unsupported diagnostic with no window.
    {
        auto missing = std::make_shared<GpuPresentationClient>();
        ViewerGpuPresenter presenter(
            missing, ViewerGpuPresenter::Config{options.loader_path.string(), 320U, 240U, 1.0});
        expectations.expect(!presenter.initialize(), "a missing client does not initialize");
        expectations.expect(presenter.state() == ViewerGpuPresenter::State::Unsupported,
                            "a missing client reports Unsupported");
        expectations.expect(presenter.container() == nullptr,
                            "a missing client creates no container");
        expectations.expect(!presenter.diagnostic().empty(),
                            "a missing client reports a diagnostic");
    }

    // 2. A real embedded viewer.
    QWidget host;
    host.resize(800, 600);
    auto* layout = new QVBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    host.show();

    ViewerGpuPresenter::Config config{options.loader_path.string(), 320U, 240U, 1.0};
    auto presenter = std::make_unique<ViewerGpuPresenter>(client, config);
    bool ready = false;
    presenter->setReadyCallback([&](const bool value, const std::string&) { ready = value; });
    expectations.expect(presenter->initialize(), "the real viewer initializes");
    expectations.expect(presenter->container() != nullptr, "the real viewer has a container");
    layout->addWidget(presenter->container());
    expectations.expect(waitActive(*presenter, 15'000), "the real viewer becomes Active");
    expectations.expect(ready, "the ready callback reported readiness");
    expectations.expect(presenter->attached(), "the real viewer attached a target");
    const GpuPresentationTargetId firstTarget = presenter->targetId();
    expectations.expect(firstTarget != bloom::runtime::kInvalidPresentationTarget,
                        "the first target id is valid");

    // 3. Input forwarding through the real QTest events on the container.
    CapturedInput captured;
    presenter->setInputCallback(
        [&captured](const ViewerGpuInputEvent& event) { captured.append(event); });
    // QTest only synthesizes a move while a button is held; press/move/release exercises it.
    QTest::mousePress(presenter->container(), Qt::LeftButton, Qt::NoModifier, QPoint(11, 22));
    QTest::mouseMove(presenter->container(), QPoint(15, 26));
    QTest::mouseRelease(presenter->container(), Qt::LeftButton, Qt::NoModifier, QPoint(15, 26));
    QTest::mouseClick(presenter->container(), Qt::LeftButton, Qt::NoModifier, QPoint(33, 44));
    QTest::keyClick(presenter->container(), Qt::Key_B, Qt::ShiftModifier);
    {
        const QPointF local(7.0, 9.0);
        const QPointF global = presenter->container()->mapToGlobal(QPoint(7, 9));
        QWheelEvent wheel(local, global, QPoint(3, 0), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(presenter->container(), &wheel);
    }
    pumpQt(20);
    const auto move = captured.last(ViewerGpuInputKind::MouseMove);
    const QPointF expectedGlobal = presenter->container()->mapToGlobal(QPoint(15, 26));
    expectations.expect(move.has_value() && std::abs(move->local.x() - 15.0) < 1.0 &&
                            std::abs(move->local.y() - 26.0) < 1.0 &&
                            std::abs(move->global.x() - expectedGlobal.x()) < 1.0 &&
                            std::abs(move->global.y() - expectedGlobal.y()) < 1.0,
                        "the mouse move local/global coordinates are preserved");
    expectations.expect(captured.count(ViewerGpuInputKind::MousePress) >= 1U &&
                            captured.count(ViewerGpuInputKind::MouseRelease) >= 1U,
                        "the mouse press/release are forwarded");
    const auto key = captured.last(ViewerGpuInputKind::KeyPress);
    expectations.expect(key.has_value() && key->key == Qt::Key_B &&
                            key->modifiers.testFlag(Qt::ShiftModifier) &&
                            key->text.compare(QStringLiteral("B"), Qt::CaseInsensitive) == 0,
                        "the key press is forwarded with its text and modifiers");
    const auto wheel = captured.last(ViewerGpuInputKind::Wheel);
    expectations.expect(wheel.has_value() && wheel->angleDelta == QPoint(0, 120) &&
                            wheel->pixelDelta == QPoint(3, 0) &&
                            std::abs(wheel->local.x() - 7.0) < 1.0,
                        "the wheel pixel and angle deltas are preserved");

    // 4. Present, resize, present.
    runCommand(Cmd::ProduceLease);
    const GpuResidentFrameLease lease = shared.lease;
    expectations.expect(lease.isValid(), "a resident lease is produced");
    expectations.expect(presenter->present(lease, makeParams(320U, 240U), nullptr),
                        "the first present is admitted");
    expectations.expect(
        waitUntil([&] { return client->status(firstTarget).appliedSequence >= 1U; }, 15'000),
        "the first present applied");
    const std::uint64_t surfaceBefore = presenter->surfaceBits();
    expectations.expect(surfaceBefore != 0U, "the adapter recorded its VkSurfaceKHR");

    expectations.expect(presenter->requestResize(400U, 300U), "the resize is admitted");
    expectations.expect(waitUntil(
                            [&] {
                                return presenter->state() == ViewerGpuPresenter::State::Active &&
                                       client->status(firstTarget).info.width == 400U &&
                                       client->status(firstTarget).info.height == 300U;
                            },
                            15'000),
                        "the owner resized to the real window extent");
    expectations.expect(presenter->surfaceBits() == surfaceBefore,
                        "the VkSurfaceKHR identity is stable across the resize");
    expectations.expect(presenter->present(lease, makeParams(400U, 300U), nullptr),
                        "an update after resize is admitted");
    expectations.expect(
        waitUntil([&] { return client->status(firstTarget).appliedSequence >= 3U; }, 15'000),
        "the resized present applied");

    // 5. Retire in flight never acks before proven Retired; no update during retirement.
    runCommand(Cmd::SetGate, true);
    bool mutationCalled = false;
    bool mutationSafe = false;
    expectations.expect(
        presenter->prepareForMutation([&](const ViewerGpuPresenter::MutationResult& result) {
            mutationCalled = true;
            mutationSafe = result.outcome == ViewerGpuPresenter::MutationOutcome::SafeToMutate;
        }),
        "the retire before UI mutation is admitted");
    pumpQt(150);
    expectations.expect(!mutationCalled, "a close in flight does not ack before proven retirement");
    expectations.expect(presenter->state() == ViewerGpuPresenter::State::Retiring,
                        "the adapter reports Retiring while the retire is in flight");
    expectations.expect(!presenter->present(lease, makeParams(400U, 300U), nullptr),
                        "a present during retirement is refused");
    runCommand(Cmd::SetGate, false);
    expectations.expect(waitUntil([&] { return mutationCalled; }, 15'000),
                        "the mutation callback fires after the owner proves retirement");
    expectations.expect(mutationSafe, "the mutation outcome is SafeToMutate");
    expectations.expect(presenter->surfaceSafeToDestroy(),
                        "the surface is safe to destroy after proven retirement");
    // The record may already have been reclaimed through forget(); Gone therefore also proves the
    // owner reached proven Retired before the UI mutated.
    const GpuPresentationTargetState firstState = client->status(firstTarget).state;
    expectations.expect(firstState == GpuPresentationTargetState::Retired ||
                            firstState == GpuPresentationTargetState::Gone,
                        "the owner published Retired before the UI mutated");

    // Host mutation only after SafeToMutate: reparent the container, then destroy the presenter.
    QWidget secondHost;
    secondHost.resize(400, 300);
    auto* secondLayout = new QVBoxLayout(&secondHost);
    secondLayout->setContentsMargins(0, 0, 0, 0);
    secondHost.show();
    layout->removeWidget(presenter->container());
    secondLayout->addWidget(presenter->container());
    pumpQt(30);
    expectations.expect(presenter->surfaceSafeToDestroy(),
                        "the surface stays safe after the approved reparent");
    presenter.reset();

    // 6. Reattachment after approved retirement gets a fresh target id and sequence.
    auto reattached = std::make_unique<ViewerGpuPresenter>(client, config);
    expectations.expect(reattached->initialize(), "the reattached viewer initializes");
    layout->addWidget(reattached->container());
    expectations.expect(waitActive(*reattached, 15'000), "the reattached viewer becomes Active");
    expectations.expect(reattached->targetId() != firstTarget,
                        "reattachment creates a fresh target id");
    expectations.expect(reattached->lastSequence() <= 1U, "reattachment starts a fresh sequence");
    expectations.expect(reattached->present(lease, makeParams(320U, 240U), nullptr),
                        "the reattached viewer presents");
    expectations.expect(
        waitUntil([&] { return client->status(reattached->targetId()).appliedSequence >= 1U; },
                  15'000),
        "the reattached present applied");

    // 7. Two independent viewers share one lease and one device.
    auto viewerA = std::make_unique<ViewerGpuPresenter>(client, config);
    auto viewerB = std::make_unique<ViewerGpuPresenter>(client, config);
    QWidget hostA;
    QWidget hostB;
    auto* layoutA = new QVBoxLayout(&hostA);
    auto* layoutB = new QVBoxLayout(&hostB);
    hostA.show();
    hostB.show();
    expectations.expect(viewerA->initialize() && viewerB->initialize(),
                        "both independent viewers initialize");
    layoutA->addWidget(viewerA->container());
    layoutB->addWidget(viewerB->container());
    expectations.expect(waitActive(*viewerA, 15'000) && waitActive(*viewerB, 15'000),
                        "both independent viewers become Active");
    expectations.expect(viewerA->present(lease, makeParams(320U, 240U), nullptr) &&
                            viewerB->present(lease, makeParams(320U, 240U), nullptr),
                        "both independent viewers present the shared lease");
    expectations.expect(waitUntil(
                            [&] {
                                return client->status(viewerA->targetId()).appliedSequence >= 1U &&
                                       client->status(viewerB->targetId()).appliedSequence >= 1U;
                            },
                            15'000),
                        "both independent viewers applied the shared lease");

    // 8. Retire every remaining viewer and destroy only after proven retirement.
    const auto retireSafe = [&](ViewerGpuPresenter& presenter, const std::string& label) {
        bool called = false;
        bool safe = false;
        expectations.expect(
            presenter.prepareForMutation([&](const ViewerGpuPresenter::MutationResult& result) {
                called = true;
                safe = result.outcome == ViewerGpuPresenter::MutationOutcome::SafeToMutate;
            }),
            label + ": the retire is admitted");
        expectations.expect(waitUntil([&] { return called; }, 15'000),
                            label + ": the mutation callback fires");
        expectations.expect(safe, label + ": the final outcome is SafeToMutate");
    };
    const GpuPresentationTargetId reattachedId = reattached->targetId();
    const GpuPresentationTargetId viewerAId = viewerA->targetId();
    const GpuPresentationTargetId viewerBId = viewerB->targetId();
    retireSafe(*reattached, "reattached");
    retireSafe(*viewerA, "viewer A");
    retireSafe(*viewerB, "viewer B");
    reattached.reset();
    viewerA.reset();
    viewerB.reset();
    expectations.expect(presenter == nullptr || presenter->surfaceSafeToDestroy(),
                        "the first viewer was already safely retired");
    // Terminal records were acknowledged through forget(): status is Gone, not Retired.
    expectations.expect(client->status(reattachedId).state == GpuPresentationTargetState::Gone &&
                            client->status(viewerAId).state == GpuPresentationTargetState::Gone &&
                            client->status(viewerBId).state == GpuPresentationTargetState::Gone,
                        "the terminal records were reclaimed through forget()");

    // 9. Repeated retire/reattach beyond maxRetainedTargets (3): forget() must release each
    //    terminal record, or the fourth attach would be refused as TooManyTargets.
    for (int cycle = 0; cycle < 6; ++cycle) {
        auto viewer = std::make_unique<ViewerGpuPresenter>(client, config);
        expectations.expect(viewer->initialize(), "cycle viewer initializes");
        layout->addWidget(viewer->container());
        expectations.expect(waitActive(*viewer, 15'000), "cycle viewer becomes Active");
        const GpuPresentationTargetId id = viewer->targetId();
        expectations.expect(viewer->present(lease, makeParams(320U, 240U), nullptr),
                            "cycle viewer presents");
        expectations.expect(
            waitUntil([&] { return client->status(id).appliedSequence >= 1U; }, 15'000),
            "cycle present applied");
        bool called = false;
        bool safe = false;
        expectations.expect(
            viewer->prepareForMutation([&](const ViewerGpuPresenter::MutationResult& result) {
                called = true;
                safe = result.outcome == ViewerGpuPresenter::MutationOutcome::SafeToMutate;
            }),
            "cycle retire is admitted");
        expectations.expect(waitUntil([&] { return called; }, 15'000),
                            "cycle mutation callback fires");
        expectations.expect(safe, "cycle outcome is SafeToMutate");
        expectations.expect(client->status(id).state == GpuPresentationTargetState::Gone,
                            "cycle terminal record was reclaimed");
        viewer.reset();
    }

    {
        std::lock_guard lock(shared.mutex);
        shared.stop = true;
    }
    shared.cv.notify_all();
    owner.join();

    expectations.expect(GpuPresentationCoordinator::quarantinedGenerationCount() == 0U,
                        "the normal lifecycle left no quarantined generation");

    if (expectations.failures() == 0) {
        std::cout << "PASS: Wayland embedded QWindow attach/present/resize/retire-gate/reattach/"
                     "two-viewers/input/forget-beyond-maxRetained through ViewerGpuPresenter\n";
    }
    return expectations.failures() == 0 ? 0 : 1;
}
