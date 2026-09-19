// Qt surface fixture for the presentation target. This is the ONLY file in the package that may
// include Qt; it creates an actual Wayland surface from a QVulkanInstance that adopts Bloom's
// borrowed instance, so the owner thread can validate and present to a real surface. It never
// fabricates a VkSurfaceKHR.
//
// One binary drives two lifecycle paths:
//
//   * The default path is the normal owner-thread lifecycle. Before the first successful target is
//     created it injects every SwapchainResources construction fault (stages 0..4) to prove a
//     mid-construction failure publishes no target. It then presents, recreates, retires, and
//     destroys the target on the owner, and only after the target is gone destroys the QWindow and
//     finally the QVulkanInstance.
//
//   * --quarantine-fixture drives the foreign-thread teardown of an already-retired target. The
//     foreign destructor must never call Vulkan: it retains the whole native generation and latches
//     the process quarantine fuse, so any later target creation fails closed. That retained
//     generation still references the Wayland surface, so the process exits with std::_Exit before
//     Qt or the QVulkanInstance can be destroyed. Destroying either while the native graph is
//     retained is exactly the invalid Vulkan lifetime this package forbids.
//
// The fixture never pretends retirement: a target is only treated as safe to destroy after the
// presentation engine is proven done, and a foreign-thread teardown of even a retired target must
// report the quarantine fuse instead of silently destroying Vulkan state off the owner thread.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_presentation_target.hpp>

#include <QGuiApplication>
#include <QTimer>
#include <QVulkanInstance>
#include <QWindow>

// Test-only access to the private construction-fault seam. The Qt include above already defines
// VK_NO_PROTOTYPES before pulling in vulkan.h, so the render module's private header is consistent
// here. The definition is supplied by CMake only when bloom_render was built with its Vulkan
// backend, so a stub build still compiles and simply skips the injected-failure cases.
#if defined(BLOOM_UI_GPU_PRESENTATION_TARGET_PRIVATE_SEAM)
#include "gpu_presentation_target_private.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace {

struct TestOptions final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool quarantine_fixture = false;
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
        } else if (argument == "--quarantine-fixture") {
            options.quarantine_fixture = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

void pump(const int milliseconds) {
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
        pump(8);
    }
    return predicate();
}

[[nodiscard]] std::uint64_t surfaceBits(const VkSurfaceKHR surface) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(surface));
}

// A requested device/surface that cannot be produced is a skip for a host without a GPU, but a hard
// failure under --require-device, which is how the gate proves the native path really ran.
[[nodiscard]] int skipOrFail(const TestOptions& options) { return options.require_device ? 1 : 0; }

#if defined(BLOOM_UI_GPU_PRESENTATION_TARGET_PRIVATE_SEAM)

// Injects each construction fault before the first successful target. Every stage must fail with no
// published target and a non-Ok code, and the seam must be cleared so the clean creation afterwards
// still succeeds. Stage 0..4 correspond to the constructor's five early-return points in
// gpu_presentation_target_resources.cpp.
[[nodiscard]] bool
exerciseConstructionFaults(bloom::render::GpuDevice& device,
                           const bloom::render::GpuPresentationTargetDescription& description) {
    using bloom::render::GpuPresentationTarget;
    using bloom::render::GpuPresentationTargetCode;
    using bloom::render::presentation_detail::clearSwapchainConstructionFaultForTesting;
    using bloom::render::presentation_detail::setSwapchainConstructionFaultForTesting;

    for (std::uint32_t stage = 0U; stage <= 4U; ++stage) {
        setSwapchainConstructionFaultForTesting(stage);
        const auto faulted = GpuPresentationTarget::create(device, description);
        clearSwapchainConstructionFaultForTesting();
        if (faulted.target != nullptr) {
            std::cerr << "FAIL: construction fault stage " << stage
                      << " published a presentation target\n";
            return false;
        }
        if (faulted.code != GpuPresentationTargetCode::DriverUnavailable) {
            std::cerr << "FAIL: construction fault stage " << stage
                      << " returned an unexpected code\n";
            return false;
        }
    }
    return true;
}

#endif // BLOOM_UI_GPU_PRESENTATION_TARGET_PRIVATE_SEAM

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

    // QT_VULKAN_LIB must be set before the first Vulkan use so Qt loads the pinned loader by
    // absolute path rather than an ambient libvulkan.
    qputenv("QT_VULKAN_LIB", options.loader_path.string().c_str());

    bloom::render::GpuDeviceCreationOptions createOptions;
    createOptions.loader_path = options.loader_path;
    createOptions.request_presentation = true;
    createOptions.presentation_platform = bloom::render::GpuPresentationPlatform::Wayland;
    auto created = bloom::render::GpuDevice::create(createOptions);
    if (!created) {
        std::cout << "SKIP: no presentable Vulkan device: " << created.diagnostic.message << '\n';
        return skipOrFail(options);
    }
    bloom::render::GpuDevice& device = *created.device;
    if (device.presentationStatus().availability !=
        bloom::render::GpuPresentationAvailability::Ready) {
        std::cout << "SKIP: presentation not ready: " << device.presentationStatus().detail << '\n';
        return skipOrFail(options);
    }
    const bloom::render::GpuBorrowedInstanceView view = device.borrowedInstanceView();
    if (!view.valid) {
        std::cerr << "FAIL: presentation is Ready but no borrowed instance view is available\n";
        return 1;
    }

    QVulkanInstance instance;
    instance.setVkInstance(
        reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(view.instance_bits)));
    if (!instance.create() || !instance.isValid()) {
        std::cout << "SKIP: QVulkanInstance could not adopt the borrowed instance\n";
        return skipOrFail(options);
    }

    // Allocated on the heap so a cleanup path can withhold the destructor when a native surface
    // could still be referenced.
    auto* window = new QWindow();
    window->setSurfaceType(QSurface::VulkanSurface);
    window->setVulkanInstance(&instance);
    window->resize(320, 240);
    window->show();

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!waitUntil(
            [&surface, window] {
                surface = QVulkanInstance::surfaceForWindow(window);
                return surface != VK_NULL_HANDLE;
            },
            10'000)) {
        window->hide();
        delete window;
        instance.destroy();
        std::cerr << "FAIL: the QWindow did not produce a Wayland VkSurfaceKHR\n";
        return 1;
    }

    bloom::render::GpuPresentationTargetDescription description;
    description.surface.surface_bits = surfaceBits(surface);
    description.surface.epoch = view.epoch;
    description.width = 320;
    description.height = 240;

#if defined(BLOOM_UI_GPU_PRESENTATION_TARGET_PRIVATE_SEAM)
    // Inject every constructor fault point on the live Wayland surface before the first real
    // target, then clear the seam so the clean creation below is the proof the failure paths left
    // no state.
    if (!exerciseConstructionFaults(device, description)) {
        window->hide();
        delete window;
        instance.destroy();
        return 1;
    }
#else
    std::cout << "NOTE: the private construction-fault seam is unavailable in this build; "
                 "skipping the injected constructor-failure cases\n";
#endif

    // A stale epoch must be rejected before any driver call.
    {
        auto stale = description;
        stale.surface.epoch.value += 1U;
        auto staleResult = bloom::render::GpuPresentationTarget::create(device, stale);
        if (staleResult.code != bloom::render::GpuPresentationTargetCode::WrongEpoch) {
            window->hide();
            delete window;
            instance.destroy();
            std::cerr << "FAIL: a stale epoch was not rejected\n";
            return 1;
        }
    }

    auto targetResult = bloom::render::GpuPresentationTarget::create(device, description);
    if (!targetResult) {
        window->hide();
        delete window;
        instance.destroy();
        std::cerr << "FAIL: presentation target creation failed: " << targetResult.message << '\n';
        return 1;
    }
    bloom::render::GpuPresentationTarget& target = *targetResult.target;

    const auto presentFrame = [&]() -> bool {
        for (int attempt = 0; attempt < 400; ++attempt) {
            const auto acquire = target.acquire();
            if (acquire == bloom::render::GpuPresentationTargetCode::Ok ||
                acquire == bloom::render::GpuPresentationTargetCode::Suboptimal) {
                const auto present = target.present(
                    bloom::render::GpuClearColor{.red = 0.1, .green = 0.2, .blue = 0.3});
                if (present == bloom::render::GpuPresentationTargetCode::Ok ||
                    present == bloom::render::GpuPresentationTargetCode::Suboptimal) {
                    return true;
                }
            }
            pump(8);
        }
        return false;
    };
    const auto drain = [&]() {
        for (int attempt = 0; attempt < 2000; ++attempt) {
            if (target.pollRetirement() !=
                bloom::render::GpuPresentationTargetCode::RetirePending) {
                break;
            }
            pump(8);
        }
    };

    if (!presentFrame()) {
        window->hide();
        delete window;
        instance.destroy();
        std::cerr << "FAIL: no frame could be presented\n";
        return 1;
    }

    // A foreign-thread acquire fails closed without touching the driver.
    {
        std::atomic<int> foreign{0};
        std::thread other([&] { foreign.store(static_cast<int>(target.acquire())); });
        other.join();
        if (foreign.load() !=
            static_cast<int>(bloom::render::GpuPresentationTargetCode::WrongThread)) {
            window->hide();
            delete window;
            instance.destroy();
            std::cerr << "FAIL: a foreign-thread acquire was not rejected\n";
            return 1;
        }
    }

    // Repeated resize/recreate at a positive extent, then present again. The old generation's
    // present must be proven retired first, so poll and retry recreate while it reports NotReady.
    bloom::render::GpuPresentationTargetDescription resized = description;
    resized.width = 400;
    resized.height = 300;
    auto recreated = bloom::render::GpuPresentationTargetCode::NotReady;
    for (int attempt = 0; attempt < 2000; ++attempt) {
        static_cast<void>(target.pollRetirement());
        recreated = target.recreate(resized);
        if (recreated != bloom::render::GpuPresentationTargetCode::NotReady) {
            break;
        }
        pump(8);
    }
    if (recreated != bloom::render::GpuPresentationTargetCode::Ok &&
        recreated != bloom::render::GpuPresentationTargetCode::Suboptimal) {
        window->hide();
        delete window;
        instance.destroy();
        std::cerr << "FAIL: recreate failed: " << target.lastMessage() << '\n';
        return 1;
    }
    if (!presentFrame()) {
        window->hide();
        delete window;
        instance.destroy();
        std::cerr << "FAIL: no frame could be presented after recreate\n";
        return 1;
    }
    drain();

    // Acquired-but-unpresented close: beginRetire must legally discard the acquired image and only
    // report Retired once that discard present is proven retired.
    auto acquired = bloom::render::GpuPresentationTargetCode::NotReady;
    for (int attempt = 0; attempt < 2000; ++attempt) {
        static_cast<void>(target.pollRetirement());
        acquired = target.acquire();
        if (acquired != bloom::render::GpuPresentationTargetCode::NotReady) {
            break;
        }
        pump(8);
    }
    if (acquired != bloom::render::GpuPresentationTargetCode::Ok &&
        acquired != bloom::render::GpuPresentationTargetCode::Suboptimal) {
        window->hide();
        delete window;
        instance.destroy();
        std::cerr << "FAIL: the acquired-unpresented fixture could not acquire: "
                  << target.lastMessage() << '\n';
        return 1;
    }
    static_cast<void>(target.beginRetire());
    drain();
    if (target.retireState() != bloom::render::GpuPresentationTargetCode::Retired) {
        // Withhold the retire acknowledgement and bypass all Qt/native cleanup: keep both the
        // QWindow and the adopted QVulkanInstance alive, because either being destroyed could free
        // a surface the presentation engine may still reference.
        std::cerr << "FAIL: acquired-unpresented retirement was not proven ("
                  << target.lastMessage() << "); retaining the QWindow and QVulkanInstance\n";
        std::_Exit(1);
    }

    if (options.quarantine_fixture) {
        // Foreign-thread teardown of the already-retired target. releaseImpl must never call Vulkan
        // from the foreign thread: it retains the whole native generation, latches the quarantine
        // fuse, and create() then fails closed. The retained generation keeps the device allocator
        // state the borrowed instance belongs to alive until process end, so the process exits
        // before Qt or the QVulkanInstance is destroyed and the retained native graph is never torn
        // down out from under them.
        std::unique_ptr<bloom::render::GpuPresentationTarget> owned =
            std::move(targetResult.target);
        std::thread foreign([&owned] { owned.reset(); });
        foreign.join();
        if (!bloom::render::GpuPresentationTarget::teardownDrainIncomplete()) {
            std::_Exit(1);
        }
        const auto blocked = bloom::render::GpuPresentationTarget::create(device, description);
        if (blocked.code != bloom::render::GpuPresentationTargetCode::PresentationUnavailable ||
            blocked.target != nullptr) {
            std::_Exit(1);
        }
        std::cout
            << "PASS: a foreign-thread teardown of a retired presentation target retained the "
               "native graph, latched the quarantine fuse, and blocked further target creation\n"
            << std::flush;
        std::_Exit(0);
    }

    // Normal owner-thread lifecycle: the target is already retired, so destroying it on the owner
    // releases its Vulkan resources before Qt destroys the surface, and the instance goes last.
    targetResult.target.reset();
    window->hide();
    delete window;
    instance.destroy();
    std::cout
        << "PASS: Wayland surface acquired, presented, recreated/resized, acquired-unpresented "
           "close, and safely retired on the owner thread\n";
    return 0;
}
