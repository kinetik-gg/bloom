// Real Wayland Qt fixture for the runtime presentation coordinator. The main/UI thread owns one
// QVulkanInstance and a DISTINCT QWindow per presentation target (each with its own real
// VkSurfaceKHR); a dedicated TEST owner thread owns the GpuDevice, the
// GpuResidentFrameLeaseRegistry, and the coordinator, and drives pump(). Every command crosses the
// Qt-free/Vulkan-free public port; the test never touches a native target directly.
//
// Two non-retired swapchains may never share one Wayland surface, so A/B/C each get a real window
// and each window is held until that target's own proven Retired ack. Resize drives the real
// QWindow and processes its resize before expecting a matching swapchain extent.
//
// --loader pins an explicit loader and --require-device fails closed without a compatible device.
// The normal fixture destroys each native target (via Retired, on the owner) before the window
// before the instance before the device. A device-loss/quarantine case cannot be induced on this
// hardware and is documented as unproven rather than faked.

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

#include "gpu_borrowed_instance.hpp"

#include <QGuiApplication>
#include <QTimer>
#include <QVulkanInstance>
#include <QWindow>

#include <array>
#include <atomic>
#include <chrono>
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
using bloom::render::GpuBorrowedInstanceView;
using bloom::render::GpuBorrowedSurface;
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
using bloom::runtime::GpuPresentationOverlay;
using bloom::runtime::GpuPresentationPortCode;
using bloom::runtime::GpuPresentationTargetId;
using bloom::runtime::GpuPresentationTargetState;
using bloom::runtime::GpuPresentationUpdate;
using bloom::runtime::GpuResidentFrameLease;
using bloom::runtime::GpuResidentFrameLeaseRegistry;

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
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (!condition) {
            std::lock_guard lock(mutex_);
            ++failures_;
            std::cerr << "FAIL: " << message << " (" << location.file_name() << ':'
                      << location.line() << ")\n";
        }
    }
    [[nodiscard]] int failures() const noexcept { return failures_.load(); }

  private:
    std::mutex mutex_;
    std::atomic<int> failures_{0};
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
        pumpQt(8);
    }
    return predicate();
}

enum class Cmd { None, ProduceLease, ProduceForeignLease, SetGate, Barrier };

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
    GpuResidentFrameLease foreignLease;
};

// One real UI surface: a distinct QWindow owning its own VkSurfaceKHR, on the main/UI thread.
struct UiSurface final {
    QWindow* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] std::uint64_t bits() const noexcept {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(surface));
    }
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

#include "gpu_presentation_coordinator_owner.ipp"

} // namespace

int main(int argc, char** argv) {
    const TestOptions options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    QGuiApplication application(argc, argv);
    if (options.loader_path.empty()) {
        std::cout << "SKIP: --loader <libvulkan.so.1> is required\n";
        return skipOrFail(options);
    }
    qputenv("QT_VULKAN_LIB", options.loader_path.string().c_str());

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
    const auto waitState = [&](const GpuPresentationTargetId id,
                               const GpuPresentationTargetState state, const int timeout) {
        return waitUntil([&] { return client->status(id).state == state; }, timeout);
    };
    const auto waitApplied = [&](const GpuPresentationTargetId id, const std::uint64_t sequence,
                                 const int timeout) {
        return waitUntil([&] { return client->status(id).appliedSequence >= sequence; }, timeout);
    };

    const GpuBorrowedInstanceView view = client->instanceView();
    if (!view.valid) {
        runCommand(Cmd::Barrier);
        owner.join();
        std::cerr << "FAIL: no borrowed instance view\n";
        return 1;
    }
    QVulkanInstance instance;
    instance.setVkInstance(bloom::ui::test::borrowedInstance(view.instance_bits));
    if (!instance.create() || !instance.isValid()) {
        owner.join();
        std::cout << "SKIP: QVulkanInstance could not adopt the borrowed instance\n";
        return skipOrFail(options);
    }

    // Create one real QWindow/surface per target on the main/UI thread. Each stays alive until its
    // own target has published Retired (or its rejection proved no native target was created).
    const auto createSurface = [&](const std::uint32_t width, const std::uint32_t height) {
        UiSurface ui;
        ui.window = new QWindow();
        ui.window->setSurfaceType(QSurface::VulkanSurface);
        ui.window->setVulkanInstance(&instance);
        ui.window->resize(static_cast<int>(width), static_cast<int>(height));
        ui.window->show();
        ui.width = width;
        ui.height = height;
        if (!waitUntil(
                [&ui] {
                    ui.surface = QVulkanInstance::surfaceForWindow(ui.window);
                    return ui.surface != VK_NULL_HANDLE;
                },
                10'000)) {
            return std::optional<UiSurface>{};
        }
        return std::optional<UiSurface>{ui};
    };

    auto surfaceA = createSurface(320U, 240U);
    auto surfaceB = createSurface(320U, 240U);
    auto surfaceC = createSurface(320U, 240U);
    if (!surfaceA || !surfaceB || !surfaceC) {
        owner.join();
        std::cerr << "FAIL: a QWindow did not produce a Wayland VkSurfaceKHR\n";
        return 1;
    }

    const auto attach = [&](const UiSurface& ui, const std::uint32_t width,
                            const std::uint32_t height, const std::uint64_t epochValue) {
        GpuBorrowedSurface borrowed;
        borrowed.surface_bits = ui.bits();
        borrowed.epoch = view.epoch;
        borrowed.epoch.value = epochValue;
        return client->attach(borrowed, width, height);
    };
    const auto makeUpdate = [](const GpuResidentFrameLease& lease,
                               const std::shared_ptr<const GpuPresentationOverlay>& overlay,
                               const std::uint32_t width, const std::uint32_t height) {
        GpuPresentationUpdate update;
        update.lease = lease;
        update.overlay = overlay;
        update.params.targetWidth = width;
        update.params.targetHeight = height;
        update.params.destination =
            GpuPresentRect{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)};
        update.params.source = GpuPresentSourceWindow{0.0, 0.0, 16.0, 8.0};
        update.params.channel = GpuPresentChannel::Rgba;
        update.params.background = GpuPresentBackground::Solid;
        return update;
    };

    // Stale epoch is rejected before any native target exists, and owns no surface.
    {
        auto staleSurface = createSurface(320U, 240U);
        expectations.expect(staleSurface.has_value(), "the stale-epoch surface was created");
        if (staleSurface) {
            const auto stale = attach(*staleSurface, 320U, 240U, view.epoch.value + 1U);
            expectations.expect(
                waitState(stale.target, GpuPresentationTargetState::Rejected, 10'000),
                "the stale epoch was rejected");
            const auto snapshot = client->status(stale.target);
            expectations.expect(snapshot.surfaceSafeToDestroy,
                                "a stale-epoch rejection is safe to destroy the surface");
            staleSurface->window->hide();
            delete staleSurface->window;
        }
    }

    const auto targetA = attach(*surfaceA, 320U, 240U, view.epoch.value);
    expectations.expect(targetA.code == GpuPresentationPortCode::Accepted, "target A attaches");
    expectations.expect(waitState(targetA.target, GpuPresentationTargetState::Active, 10'000),
                        "target A becomes Active");

    runCommand(Cmd::ProduceLease);
    const GpuResidentFrameLease lease = shared.lease;
    expectations.expect(lease.isValid(), "a resident lease is produced");

    // First present.
    expectations.expect(
        client->update(targetA.target, 1U, makeUpdate(lease, nullptr, 320U, 240U)).accepted(),
        "the first update is admitted");
    expectations.expect(waitApplied(targetA.target, 1U, 10'000), "the first update is presented");
    expectations.expect(client->status(targetA.target).presentCount == 1U,
                        "exactly one present completed");

    // Repeated overlay updates.
    const auto overlayBytes = std::vector<std::uint8_t>(std::size_t{4} * 4U * 4U, 128U);
    const auto overlayOne = GpuPresentationOverlay::create(overlayBytes, 4U, 4U, 0U, 1U, kBudget);
    const auto overlayTwo = GpuPresentationOverlay::create(overlayBytes, 4U, 4U, 0U, 2U, kBudget);
    expectations.expect(overlayOne != nullptr && overlayTwo != nullptr, "the overlays build");
    expectations.expect(
        client->update(targetA.target, 2U, makeUpdate(lease, overlayOne, 320U, 240U)).accepted(),
        "the first overlay update is admitted");
    expectations.expect(waitApplied(targetA.target, 2U, 10'000), "the first overlay presents");
    expectations.expect(
        client->update(targetA.target, 3U, makeUpdate(lease, overlayTwo, 320U, 240U)).accepted(),
        "the second overlay update is admitted");
    expectations.expect(waitApplied(targetA.target, 3U, 10'000), "the second overlay presents");

    // Latest-only coalescing: gate the owner, enqueue three updates, then release one pump.
    const std::uint64_t presentsBefore = client->status(targetA.target).presentCount;
    runCommand(Cmd::SetGate, true);
    expectations.expect(
        client->update(targetA.target, 10U, makeUpdate(lease, nullptr, 320U, 240U)).accepted(),
        "coalescing update 10 admitted");
    expectations.expect(
        client->update(targetA.target, 11U, makeUpdate(lease, nullptr, 320U, 240U)).code ==
            GpuPresentationPortCode::Coalesced,
        "coalescing update 11 supersedes 10");
    expectations.expect(
        client->update(targetA.target, 12U, makeUpdate(lease, nullptr, 320U, 240U)).code ==
            GpuPresentationPortCode::Coalesced,
        "coalescing update 12 supersedes 11");
    runCommand(Cmd::SetGate, false);
    expectations.expect(waitApplied(targetA.target, 12U, 10'000), "only the newest update applies");
    expectations.expect(client->status(targetA.target).presentCount == presentsBefore + 1U,
                        "coalescing produced exactly one present");

    // Cancellation of a pending update.
    const std::uint64_t presentsBeforeCancel = client->status(targetA.target).presentCount;
    runCommand(Cmd::SetGate, true);
    expectations.expect(
        client->update(targetA.target, 20U, makeUpdate(lease, nullptr, 320U, 240U)).accepted(),
        "the cancellable update is admitted");
    expectations.expect(client->cancel(targetA.target, 20U).code ==
                            GpuPresentationPortCode::Accepted,
                        "the pending update is cancelled");
    runCommand(Cmd::SetGate, false);
    runCommand(Cmd::Barrier);
    expectations.expect(client->status(targetA.target).appliedSequence == 12U,
                        "a cancelled update is never applied");
    expectations.expect(client->status(targetA.target).presentCount == presentsBeforeCancel,
                        "cancellation issued no present");

    // A foreign-registry lease is refused by the owner without killing the target.
    runCommand(Cmd::ProduceForeignLease);
    expectations.expect(shared.foreignLease.isValid(), "a foreign lease is produced");
    expectations.expect(
        client->update(targetA.target, 21U, makeUpdate(shared.foreignLease, nullptr, 320U, 240U))
            .accepted(),
        "the foreign-lease update is admitted");
    expectations.expect(waitUntil(
                            [&] {
                                const auto snapshot = client->status(targetA.target);
                                return snapshot.state == GpuPresentationTargetState::Active &&
                                       !snapshot.message.empty();
                            },
                            10'000),
                        "a foreign lease is refused while the target stays Active");
    expectations.expect(client->status(targetA.target).appliedSequence == 12U,
                        "a foreign lease is never presented");

    // Duplicate-surface negative control: a second attach over A's live surface must be refused and
    // must NOT retire A or claim A's surface is safe to destroy.
    {
        const auto duplicate = attach(*surfaceA, 320U, 240U, view.epoch.value);
        expectations.expect(duplicate.code == GpuPresentationPortCode::DuplicateSurface,
                            "a duplicate attach over a live surface is refused");
        expectations.expect(duplicate.target == 0U, "a duplicate attach yields no new target");
        expectations.expect(client->status(targetA.target).state ==
                                GpuPresentationTargetState::Active,
                            "the existing owner is untouched by the duplicate rejection");
        expectations.expect(!client->status(targetA.target).surfaceSafeToDestroy,
                            "the duplicate rejection does not claim A's surface is safe");
    }

    // Two distinct targets share one lease, each on its own real surface.
    const auto targetB = attach(*surfaceB, 320U, 240U, view.epoch.value);
    expectations.expect(targetB.code == GpuPresentationPortCode::Accepted, "target B attaches");
    expectations.expect(waitState(targetB.target, GpuPresentationTargetState::Active, 10'000),
                        "target B becomes Active");
    expectations.expect(
        client->update(targetB.target, 1U, makeUpdate(lease, nullptr, 320U, 240U)).accepted(),
        "target B admits the shared lease");
    expectations.expect(waitApplied(targetB.target, 1U, 10'000),
                        "two targets present the same lease");

    // Resize: drive the real QWindow A, process its resize, and expect the swapchain extent to
    // follow.
    surfaceA->window->resize(400, 300);
    expectations.expect(
        waitUntil(
            [&] { return surfaceA->window->width() == 400 && surfaceA->window->height() == 300; },
            5000),
        "the QWindow A reports the new size");
    expectations.expect(client->resize(targetA.target, 30U, 400U, 300U).accepted(),
                        "the resize is admitted");
    expectations.expect(waitUntil(
                            [&] {
                                const auto snapshot = client->status(targetA.target);
                                return snapshot.info.width == 400U && snapshot.info.height == 300U;
                            },
                            15'000),
                        "the target resized to match the real window extent");
    expectations.expect(
        client->update(targetA.target, 31U, makeUpdate(lease, nullptr, 400U, 300U)).accepted(),
        "an update after resize is admitted");
    expectations.expect(waitApplied(targetA.target, 31U, 10'000), "the resized target presents");

    // Bounded target cap: A, B and the attached C fill maxTargets=4 with the stale slot released.
    const auto targetC = attach(*surfaceC, 320U, 240U, view.epoch.value);
    expectations.expect(targetC.code == GpuPresentationPortCode::Accepted, "target C attaches");
    expectations.expect(waitState(targetC.target, GpuPresentationTargetState::Active, 10'000),
                        "target C becomes Active");
    auto surfaceD = createSurface(320U, 240U);
    expectations.expect(surfaceD.has_value(), "the cap-probe surface was created");
    if (surfaceD) {
        const auto targetD = attach(*surfaceD, 320U, 240U, view.epoch.value);
        expectations.expect(targetD.code == GpuPresentationPortCode::TooManyTargets,
                            "a target beyond the bounded cap is refused");
    }

    // Stale sequence and unknown target.
    expectations.expect(
        client->update(targetA.target, 31U, makeUpdate(lease, nullptr, 400U, 300U)).code ==
            GpuPresentationPortCode::StaleSequence,
        "a stale sequence is refused");
    expectations.expect(client->update(999U, 1U, makeUpdate(lease, nullptr, 320U, 240U)).code ==
                            GpuPresentationPortCode::UnknownTarget,
                        "an unknown target is refused");
    expectations.expect(client->retire(999U, 1U).code == GpuPresentationPortCode::UnknownTarget,
                        "retiring an unknown target is refused");

    // Retire every target; Retired is only published after presentation-engine proof.
    const std::array<GpuPresentationTargetId, 3> targets{targetA.target, targetB.target,
                                                         targetC.target};
    for (const auto id : targets) {
        expectations.expect(client->retire(id, 100U).code == GpuPresentationPortCode::Accepted,
                            "retire is admitted");
    }
    for (const auto id : targets) {
        expectations.expect(waitState(id, GpuPresentationTargetState::Retired, 15'000),
                            "the target retires after engine proof");
        expectations.expect(client->status(id).surfaceSafeToDestroy,
                            "a Retired target is safe for the UI to destroy");
    }

    // Reattachment after real retirement: A's surface may be attached again once its native target
    // is proven gone and destroyed.
    {
        const auto reattached = attach(*surfaceA, 320U, 240U, view.epoch.value);
        expectations.expect(reattached.code == GpuPresentationPortCode::Accepted,
                            "a retired surface may be re-attached");
        if (reattached.code == GpuPresentationPortCode::Accepted) {
            expectations.expect(
                waitState(reattached.target, GpuPresentationTargetState::Active, 10'000),
                "the re-attached target becomes Active");
            expectations.expect(client->retire(reattached.target, 200U).code ==
                                    GpuPresentationPortCode::Accepted,
                                "the re-attached target retires");
            expectations.expect(
                waitState(reattached.target, GpuPresentationTargetState::Retired, 15'000),
                "the re-attached target retires after engine proof");
        }
    }

    // Native targets are gone before the windows are destroyed; windows before the instance.
    for (UiSurface* ui : {&*surfaceA, &*surfaceB, &*surfaceC}) {
        ui->window->hide();
        delete ui->window;
        ui->window = nullptr;
    }
    if (surfaceD) {
        surfaceD->window->hide();
        delete surfaceD->window;
    }
    instance.destroy();

    {
        std::lock_guard lock(shared.mutex);
        shared.stop = true;
    }
    shared.cv.notify_all();
    owner.join();

    expectations.expect(GpuPresentationCoordinator::quarantinedGenerationCount() == 0U,
                        "the normal lifecycle left no quarantined generation");

    if (expectations.failures() == 0) {
        std::cout
            << "PASS: Wayland attach/present/overlay/coalesce/cancel/resize/duplicate-surface/"
               "retire/reattach across the presentation coordinator port\n";
    }
    return expectations.failures() == 0 ? 0 : 1;
}
