// Real native tests for the GPU-resident frame lease + owner registry. Each leased image is an
// actual Solid -> GpuResidentDisplay result produced on a real Vulkan device; the lease layer is
// exercised only through its public, Vulkan-free API, so a bug here can only be in the lease logic.
//
// --loader pins an explicit loader and --require-device fails closed without a compatible device;
// there is no fake native skip. Every helper thread is joined, so the suite has no unjoined fixture
// and no global mutable registry.

#include <bloom/runtime/gpu_resident_frame_lease.hpp>

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image_types.hpp>

#include <atomic>
#include <cstdint>
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

using bloom::core::Color4d;
using bloom::core::PixelAspectRatio;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDisplayImage;
using bloom::render::GpuImage;
using bloom::render::GpuResidentDisplay;
using bloom::render::GpuResidentDisplayDiagnosticCode;
using bloom::render::GpuResidentDisplayPollResult;
using bloom::render::GpuSolid;
using bloom::render::GpuSolidDiagnosticCode;
using bloom::render::GpuSolidParameters;
using bloom::render::GpuSolidPollResult;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::runtime::GpuResidentFrameLease;
using bloom::runtime::GpuResidentFrameLeaseBudgets;
using bloom::runtime::GpuResidentFrameLeaseCode;
using bloom::runtime::GpuResidentFrameLeaseRegistry;
using bloom::runtime::GpuResidentFramePin;

constexpr std::uint64_t kBudget = std::uint64_t{1} << 32;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

struct Options final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool valid = true;
};

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
    Options options;
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

[[nodiscard]] std::optional<ImageWindow> makeWindow(const std::int64_t x, const std::int64_t y,
                                                    const std::uint64_t width,
                                                    const std::uint64_t height) {
    const auto created = ImageWindow::create(x, y, width, height);
    return created ? std::optional(*created.value()) : std::nullopt;
}

// One device pipeline set: the device plus the Solid and resident-display operations used to
// produce a real GPU-resident display image.
struct DisplayHost final {
    std::unique_ptr<GpuDevice> device;
    std::unique_ptr<GpuSolid> solid;
    std::unique_ptr<GpuResidentDisplay> display;
};

[[nodiscard]] std::optional<DisplayHost> makeHost(const GpuDeviceCreationOptions& options,
                                                  Expectations& expectations,
                                                  std::string* reason = nullptr) {
    auto device = GpuDevice::create(options);
    if (!device) {
        if (reason != nullptr) {
            *reason = device.diagnostic.message;
        }
        return std::nullopt;
    }
    auto solid = GpuSolid::create(*device.device);
    auto display = GpuResidentDisplay::create(*device.device);
    expectations.expect(static_cast<bool>(solid) && static_cast<bool>(display),
                        "the Solid and resident-display pipelines are created");
    if (!solid || !display) {
        return std::nullopt;
    }
    DisplayHost host;
    host.device = std::move(device.device);
    host.solid = std::move(solid.solid);
    host.display = std::move(display.display);
    return host;
}

// Small, odd/narrow display geometry. The default is the established tiny fixture; the narrow case
// exercises odd extents and a non-4:3 pixel aspect.
struct Geometry final {
    std::uint64_t width = 16;
    std::uint64_t height = 8;
    std::int64_t displayOriginX = -2;
    std::int64_t displayOriginY = 3;
    std::uint64_t aspectNumerator = 4;
    std::uint64_t aspectDenominator = 3;
};

// Runs the actual Solid -> GpuResidentDisplay chain and returns strong ownership of the resident
// RGBA8 display image.
//
// UPSTREAM NOTE: the resident-display production path used here is the current live revision, whose
// status-buffer initialization and Neutral dispatch dimensionality are being corrected in the
// separate gpu-present-image-prep slice. The lease correctness below depends only on the operation
// producing a valid, device-bound resident image with the requested window/PAR and a non-zero
// actual allocation; it does not depend on display parity. When integrating, use the patched
// production revision rather than copying this proof's native sources.
[[nodiscard]] std::shared_ptr<const GpuDisplayImage>
produceDisplay(DisplayHost& host, Expectations& expectations, const Geometry& geometry = {}) {
    const auto dataWindow = makeWindow(0, 0, geometry.width, geometry.height);
    const auto displayWindow = makeWindow(geometry.displayOriginX, geometry.displayOriginY,
                                          geometry.width, geometry.height);
    const auto aspect =
        PixelAspectRatio::create(geometry.aspectNumerator, geometry.aspectDenominator);
    expectations.expect(static_cast<bool>(dataWindow) && static_cast<bool>(displayWindow) &&
                            static_cast<bool>(aspect),
                        "the geometry and pixel aspect build");
    if (!dataWindow || !displayWindow || !aspect) {
        return nullptr;
    }
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.25, 0.5, 0.75, 1.0});
    if (!pixel) {
        expectations.expect(false, "the solid primitive builds");
        return nullptr;
    }
    const auto solidBegin = host.solid->begin(
        GpuSolidParameters{*pixel.value(), *dataWindow, *displayWindow, *aspect}, kBudget);
    expectations.expect(solidBegin.code == GpuSolidDiagnosticCode::None,
                        "the solid begin is accepted: " + solidBegin.message);
    if (solidBegin.code != GpuSolidDiagnosticCode::None) {
        return nullptr;
    }
    GpuSolidPollResult solidPoll = GpuSolidPollResult::Pending;
    while (solidPoll == GpuSolidPollResult::Pending) {
        solidPoll = host.solid->poll();
    }
    expectations.expect(solidPoll == GpuSolidPollResult::Ready, "the solid job completes");
    if (solidPoll != GpuSolidPollResult::Ready) {
        return nullptr;
    }
    auto input = std::make_shared<const GpuImage>(host.solid->takeImage());
    const auto displayBegin = host.display->begin(input, kBudget);
    expectations.expect(displayBegin.code == GpuResidentDisplayDiagnosticCode::None,
                        "the resident display begin is accepted: " + displayBegin.message);
    if (displayBegin.code != GpuResidentDisplayDiagnosticCode::None) {
        return nullptr;
    }
    GpuResidentDisplayPollResult displayPoll = GpuResidentDisplayPollResult::Pending;
    while (displayPoll == GpuResidentDisplayPollResult::Pending) {
        displayPoll = host.display->poll();
    }
    expectations.expect(displayPoll == GpuResidentDisplayPollResult::Ready,
                        "the resident display job completes");
    if (displayPoll != GpuResidentDisplayPollResult::Ready) {
        return nullptr;
    }
    auto image = std::make_shared<const GpuDisplayImage>(host.display->takeImage());
    expectations.expect(image->isValid(), "the resident display image is valid");
    return image->isValid() ? image : nullptr;
}

// Copy/drop of the last token on a foreign thread must not touch Vulkan, and the owner's
// collectExpired() must then release the native image.
void testTokenReleaseAndCollect(Expectations& expectations, DisplayHost& host) {
    auto registry = GpuResidentFrameLeaseRegistry::create(
        *host.device, GpuResidentFrameLeaseBudgets{std::uint64_t{1} << 30, 8});
    expectations.expect(registry != nullptr, "the registry is created");
    if (registry == nullptr) {
        return;
    }
    auto image = produceDisplay(host, expectations);
    if (image == nullptr) {
        return;
    }
    const std::uint64_t bytes = image->allocationBytes();
    expectations.expect(bytes > 0, "the resident image reports actual allocation bytes");
    std::weak_ptr<const GpuDisplayImage> weakImage = image;

    auto published = registry->publish(image);
    expectations.expect(published.hasValue(),
                        "publish accepts a bound resident image: " + published.diagnostic.message);
    if (!published.hasValue()) {
        return;
    }
    expectations.expect(published.lease.width() == 16 && published.lease.height() == 8,
                        "the token carries the image extent");
    const auto expectedAspect = PixelAspectRatio::create(4, 3);
    expectations.expect(expectedAspect.has_value(), "the 4:3 test pixel aspect is valid");
    if (!expectedAspect.has_value()) {
        return;
    }
    expectations.expect(published.lease.pixelAspect() == *expectedAspect,
                        "the token carries the pixel aspect");
    expectations.expect(published.lease.displayWindow().has_value() &&
                            published.lease.displayWindow()->originX() == -2 &&
                            published.lease.displayWindow()->originY() == 3,
                        "the token carries the display window");
    expectations.expect(published.lease.allocationBytes() == bytes,
                        "the token carries the actual allocation bytes");
    expectations.expect(registry->chargedBytes() == bytes && registry->entryCount() == 1,
                        "publishing charges the actual bytes once");
    // Drop the test's own strong reference: after this, only the registry owns the native image.
    image.reset();

    // The UI thread receives the only token and drops it there. Dropping a token never touches
    // Vulkan or frees native memory; the registry still owns the image.
    GpuResidentFrameLease moved = std::move(published.lease);
    bool uiValid = false;
    std::thread ui([token = std::move(moved), &uiValid]() mutable { uiValid = token.isValid(); });
    ui.join();
    expectations.expect(uiValid, "the token is valid on the UI thread before it is dropped");
    expectations.expect(!weakImage.expired(),
                        "the registry retains the native image while it holds the lease");
    expectations.expect(registry->chargedBytes() == bytes,
                        "the token drop on the UI thread did not free native bytes");

    registry->collectExpired();
    expectations.expect(weakImage.expired(), "owner collection releases the unreferenced image");
    expectations.expect(registry->chargedBytes() == 0 && registry->entryCount() == 0,
                        "owner collection clears the byte charge and metadata");
}

// An invalidated lease that is still pinned remains charged and native-alive until the pin is
// released.
void testPinnedTombstone(Expectations& expectations, DisplayHost& host) {
    auto registry = GpuResidentFrameLeaseRegistry::create(
        *host.device, GpuResidentFrameLeaseBudgets{std::uint64_t{1} << 30, 8});
    if (registry == nullptr) {
        expectations.expect(false, "the registry is created");
        return;
    }
    auto image = produceDisplay(host, expectations);
    if (image == nullptr) {
        return;
    }
    const std::uint64_t bytes = image->allocationBytes();
    std::weak_ptr<const GpuDisplayImage> weakImage = image;
    auto published = registry->publish(image);
    if (!published.hasValue()) {
        expectations.expect(false, "publish accepts the pinned image");
        return;
    }
    auto pin = std::move(registry->pin(published.lease).pin);
    expectations.expect(pin.isValid(), "the owner lookup returns a native pin");
    expectations.expect(pin.leaseId() == published.lease.id(), "the pin reports its lease id");
    // After this, the registry entry and the pin are the only strong owners.
    image.reset();

    registry->invalidateAll();
    expectations.expect(!published.lease.isValid(), "invalidateAll invalidates the token");
    expectations.expect(pin.isValid(), "an existing pin survives invalidationAll");
    expectations.expect(registry->chargedBytes() == bytes,
                        "the pinned tombstone remains charged after invalidation");

    published.lease = {};
    registry->collectExpired();
    expectations.expect(registry->chargedBytes() == bytes && registry->entryCount() == 1,
                        "a pinned tombstone stays charged and metadata-retained");
    expectations.expect(!weakImage.expired(), "the pinned tombstone still owns the native image");

    pin = GpuResidentFramePin{};
    registry->collectExpired();
    expectations.expect(registry->chargedBytes() == 0 && registry->entryCount() == 0,
                        "releasing the last pin frees the tombstone");
    expectations.expect(weakImage.expired(), "the native image is released after the pin");
}

// A publication that would exceed the budget is refused; active UI leases are untouched.
void testActiveBudgetRefusal(Expectations& expectations, DisplayHost& host) {
    auto probe = produceDisplay(host, expectations);
    if (probe == nullptr) {
        return;
    }
    const std::uint64_t bytes = probe->allocationBytes();
    auto registry = GpuResidentFrameLeaseRegistry::create(
        *host.device, GpuResidentFrameLeaseBudgets{bytes * 2, 8});
    if (registry == nullptr) {
        expectations.expect(false, "the registry is created");
        return;
    }
    auto first = registry->publish(probe);
    auto second = registry->publish(probe);
    expectations.expect(first.hasValue() && second.hasValue(),
                        "two publications within the budget are accepted");
    auto third = registry->publish(probe);
    expectations.expect(!third.hasValue() &&
                            third.diagnostic.code == GpuResidentFrameLeaseCode::OverBudget,
                        "the over-budget publication is refused");
    expectations.expect(first.lease.isValid() && second.lease.isValid(),
                        "active leases survive a refused publication");
    expectations.expect(registry->pin(first.lease).hasValue(),
                        "an active lease still pins after a refused publication");
}

// A resident image from a different actual device is rejected before it is retained.
void testForeignDeviceImage(Expectations& expectations, DisplayHost& host,
                            const GpuDeviceCreationOptions& options) {
    auto second = makeHost(options, expectations);
    if (!second) {
        std::cerr << "NOTE: a second device is unavailable; foreign-device rejection not "
                     "exercised\n";
        return;
    }
    auto registry = GpuResidentFrameLeaseRegistry::create(
        *host.device, GpuResidentFrameLeaseBudgets{std::uint64_t{1} << 30, 8});
    if (registry == nullptr) {
        expectations.expect(false, "the registry is created");
        return;
    }
    expectations.expect(registry->isBoundTo(*host.device), "the registry is bound to its device");
    expectations.expect(!registry->isBoundTo(*second->device),
                        "the registry is not bound to the second device");
    auto foreignImage = produceDisplay(*second, expectations);
    if (foreignImage == nullptr) {
        return;
    }
    auto rejected = registry->publish(foreignImage);
    expectations.expect(!rejected.hasValue() &&
                            rejected.diagnostic.code == GpuResidentFrameLeaseCode::ForeignDevice,
                        "a foreign-device image is refused");
    expectations.expect(registry->chargedBytes() == 0,
                        "a foreign-device rejection retains nothing");
}

// Tokens from another registry and invalidated (stale) tokens are rejected by owner lookup.
void testForeignRegistryAndStale(Expectations& expectations, DisplayHost& host) {
    auto first = GpuResidentFrameLeaseRegistry::create(
        *host.device, GpuResidentFrameLeaseBudgets{std::uint64_t{1} << 30, 8});
    auto second = GpuResidentFrameLeaseRegistry::create(
        *host.device, GpuResidentFrameLeaseBudgets{std::uint64_t{1} << 30, 8});
    expectations.expect(first != nullptr && second != nullptr, "two registries are created");
    if (first == nullptr || second == nullptr) {
        return;
    }
    expectations.expect(first->epoch() != second->epoch(), "the registries have distinct epochs");
    auto image = produceDisplay(host, expectations);
    if (image == nullptr) {
        return;
    }
    auto published = first->publish(image);
    if (!published.hasValue()) {
        expectations.expect(false, "publish accepts the image");
        return;
    }
    auto foreign = second->pin(published.lease);
    expectations.expect(!foreign.hasValue() &&
                            foreign.diagnostic.code == GpuResidentFrameLeaseCode::ForeignRegistry,
                        "a token from another registry is rejected");
    expectations.expect(first->pin(published.lease).hasValue(),
                        "the owning registry still pins the token");

    first->invalidateAll();
    auto stale = first->pin(published.lease);
    expectations.expect(!stale.hasValue() &&
                            stale.diagnostic.code == GpuResidentFrameLeaseCode::StaleLease,
                        "an invalidated token is stale");
}

// An explicit lease loss (cancel/device-loss path) invalidates every token and drops unpinned
// native ownership at once.
void testInvalidateAll(Expectations& expectations, DisplayHost& host) {
    auto registry = GpuResidentFrameLeaseRegistry::create(
        *host.device, GpuResidentFrameLeaseBudgets{std::uint64_t{1} << 30, 8});
    if (registry == nullptr) {
        expectations.expect(false, "the registry is created");
        return;
    }
    auto firstImage = produceDisplay(host, expectations);
    auto secondImage = produceDisplay(host, expectations);
    if (firstImage == nullptr || secondImage == nullptr) {
        return;
    }
    std::weak_ptr<const GpuDisplayImage> firstWeak = firstImage;
    std::weak_ptr<const GpuDisplayImage> secondWeak = secondImage;
    auto first = registry->publish(firstImage);
    auto second = registry->publish(secondImage);
    expectations.expect(first.hasValue() && second.hasValue(), "two distinct images publish");
    firstImage.reset();
    secondImage.reset();
    registry->invalidateAll();
    expectations.expect(!first.lease.isValid() && !second.lease.isValid(),
                        "lease loss invalidates every token");
    expectations.expect(registry->chargedBytes() == 0,
                        "lease loss drops every unpinned byte charge");
    expectations.expect(firstWeak.expired() && secondWeak.expired(),
                        "lease loss releases every unpinned native image");
}

// A token that outlives its registry safely reports invalid.
void testRegistryShutdown(Expectations& expectations, DisplayHost& host) {
    auto image = produceDisplay(host, expectations);
    if (image == nullptr) {
        return;
    }
    GpuResidentFrameLease survivor;
    {
        auto registry = GpuResidentFrameLeaseRegistry::create(
            *host.device, GpuResidentFrameLeaseBudgets{std::uint64_t{1} << 30, 8});
        if (registry == nullptr) {
            expectations.expect(false, "the registry is created");
            return;
        }
        auto published = registry->publish(image);
        if (!published.hasValue()) {
            expectations.expect(false, "publish accepts the image");
            return;
        }
        survivor = published.lease;
        expectations.expect(survivor.isValid(), "the token is valid before shutdown");
    }
    expectations.expect(!survivor.isValid(), "a surviving token reports invalid after shutdown");
    expectations.expect(survivor.width() == 16 && survivor.allocationBytes() > 0,
                        "metadata reads remain safe after shutdown");
    GpuResidentFrameLease copy = survivor;
    copy = {};
    expectations.expect(!survivor.isValid(), "copying/dropping a dead token is safe");
}

// Wrong-thread publish and owner lookup fail closed without mutating state.
void testWrongThread(Expectations& expectations, DisplayHost& host) {
    auto registry = GpuResidentFrameLeaseRegistry::create(
        *host.device, GpuResidentFrameLeaseBudgets{std::uint64_t{1} << 30, 8});
    if (registry == nullptr) {
        expectations.expect(false, "the registry is created");
        return;
    }
    auto image = produceDisplay(host, expectations);
    if (image == nullptr) {
        return;
    }
    auto published = registry->publish(image);
    if (!published.hasValue()) {
        expectations.expect(false, "publish accepts the image");
        return;
    }
    auto observedPublish = GpuResidentFrameLeaseCode::None;
    auto observedPin = GpuResidentFrameLeaseCode::None;
    std::thread worker([&]() {
        observedPublish = registry->publish(image).diagnostic.code;
        observedPin = registry->pin(published.lease).diagnostic.code;
    });
    worker.join();
    expectations.expect(observedPublish == GpuResidentFrameLeaseCode::WrongThread,
                        "publish from a foreign thread is WrongThread");
    expectations.expect(observedPin == GpuResidentFrameLeaseCode::WrongThread,
                        "owner lookup from a foreign thread is WrongThread");
    expectations.expect(published.lease.isValid(), "a wrong-thread request leaves the lease valid");
}

// create() binds only to the caller's own Ready device generation.
void testCreateGate(Expectations& expectations, DisplayHost& host) {
    const GpuResidentFrameLeaseBudgets defaults;
    expectations.expect(defaults.maxEntries == 4096, "the default metadata cap is the finite 4096");

    // A foreign thread cannot create a registry, and the owner create still succeeds afterward.
    GpuResidentFrameLeaseRegistry* foreignCreated = nullptr;
    std::unique_ptr<GpuResidentFrameLeaseRegistry> foreignOwner;
    bool foreignRan = false;
    std::thread worker([&]() {
        foreignRan = true;
        foreignOwner = GpuResidentFrameLeaseRegistry::create(
            *host.device, GpuResidentFrameLeaseBudgets{std::uint64_t{1} << 30, 8});
        foreignCreated = foreignOwner.get();
    });
    worker.join();
    expectations.expect(foreignRan, "the foreign create ran");
    expectations.expect(foreignOwner == nullptr && foreignCreated == nullptr,
                        "a foreign-thread create returns nullptr");
    auto ownerRegistry = GpuResidentFrameLeaseRegistry::create(
        *host.device, GpuResidentFrameLeaseBudgets{std::uint64_t{1} << 30, 8});
    expectations.expect(ownerRegistry != nullptr,
                        "the owner create succeeds after a foreign-thread create");

    // An invalid/moved-from device generation fails create.
    GpuDevice displaced = std::move(*host.device);
    expectations.expect(GpuResidentFrameLeaseRegistry::create(*host.device, {}) == nullptr,
                        "a moved-from device fails create");
    *host.device = std::move(displaced);
    expectations.expect(GpuResidentFrameLeaseRegistry::create(*host.device, {}) != nullptr,
                        "the restored owner device creates again");
}

// Narrow/odd resident geometry flows through the token metadata and the real byte budget.
void testNarrowGeometry(Expectations& expectations, DisplayHost& host) {
    auto registry = GpuResidentFrameLeaseRegistry::create(
        *host.device, GpuResidentFrameLeaseBudgets{std::uint64_t{1} << 30, 8});
    if (registry == nullptr) {
        expectations.expect(false, "the registry is created");
        return;
    }
    auto image = produceDisplay(host, expectations, Geometry{7, 3, 5, -4, 16, 9});
    if (image == nullptr) {
        return;
    }
    const std::uint64_t bytes = image->allocationBytes();
    std::weak_ptr<const GpuDisplayImage> weakImage = image;
    auto published = registry->publish(image);
    expectations.expect(published.hasValue(), "publish accepts the narrow image");
    if (!published.hasValue()) {
        return;
    }
    expectations.expect(published.lease.width() == 7 && published.lease.height() == 3,
                        "the token carries the odd extent");
    const auto aspect = PixelAspectRatio::create(16, 9);
    expectations.expect(aspect && published.lease.pixelAspect() == *aspect,
                        "the token carries the non-4:3 aspect");
    expectations.expect(published.lease.displayWindow().has_value() &&
                            published.lease.displayWindow()->originX() == 5 &&
                            published.lease.displayWindow()->originY() == -4 &&
                            published.lease.displayWindow()->extent().width() == 7,
                        "the token carries the odd display window");
    expectations.expect(published.lease.allocationBytes() == bytes && bytes > 0,
                        "the token charges the actual allocation bytes");
    image.reset();
    published.lease = {};
    registry->collectExpired();
    expectations.expect(weakImage.expired() && registry->chargedBytes() == 0,
                        "the narrow lease is reclaimed once unreferenced");
}

// Stress the owner collect against a concurrent UI drop of the last tokens. The old
// expired()-then-lock() pair could dereference a null lock if the UI dropped between the two; the
// single-snapshot collect must survive, keep a pinned tombstone charged, and fully reclaim after
// the pin is released.
void testConcurrentReleaseStress(Expectations& expectations, DisplayHost& host) {
    auto registry = GpuResidentFrameLeaseRegistry::create(
        *host.device, GpuResidentFrameLeaseBudgets{std::uint64_t{1} << 30, 64});
    if (registry == nullptr) {
        expectations.expect(false, "the registry is created");
        return;
    }
    auto image = produceDisplay(host, expectations);
    if (image == nullptr) {
        return;
    }
    const std::uint64_t bytes = image->allocationBytes();
    constexpr int kRounds = 1000;
    constexpr int kTokensPerRound = 8;
    for (int round = 0; round < kRounds; ++round) {
        std::vector<GpuResidentFrameLease> tokens;
        tokens.reserve(static_cast<std::size_t>(kTokensPerRound));
        for (int index = 0; index < kTokensPerRound; ++index) {
            auto published = registry->publish(image);
            if (!published.hasValue()) {
                expectations.expect(false, "the stress publish is accepted");
                return;
            }
            tokens.push_back(std::move(published.lease));
        }
        auto pin = std::move(registry->pin(tokens.front()).pin);
        if (!pin.isValid()) {
            expectations.expect(false, "the stress pin is valid");
            return;
        }
        std::atomic<bool> uiStarted{false};
        std::thread ui([&tokens, &uiStarted]() {
            uiStarted.store(true, std::memory_order_release);
            tokens.clear();
        });
        while (!uiStarted.load(std::memory_order_acquire)) {
            registry->collectExpired();
        }
        for (int spin = 0; spin < 64; ++spin) {
            registry->collectExpired();
        }
        ui.join();
        registry->collectExpired();
        expectations.expect(registry->chargedBytes() == bytes,
                            "the pinned tombstone stays charged through the stress round");
        pin = GpuResidentFramePin{};
        registry->collectExpired();
        expectations.expect(registry->chargedBytes() == 0 && registry->entryCount() == 0,
                            "the stress round fully reclaims after the pin is released");
    }
    image.reset();
}

// The optional wake hook fires on a state change without reentering the registry.
void testWakeCallback(Expectations& expectations, DisplayHost& host) {
    auto registry = GpuResidentFrameLeaseRegistry::create(
        *host.device, GpuResidentFrameLeaseBudgets{std::uint64_t{1} << 30, 8});
    if (registry == nullptr) {
        expectations.expect(false, "the registry is created");
        return;
    }
    std::atomic<int> wakeCount{0};
    registry->setWakeCallback([&wakeCount]() { wakeCount.fetch_add(1); });
    auto image = produceDisplay(host, expectations);
    if (image == nullptr) {
        return;
    }
    auto published = registry->publish(image);
    expectations.expect(published.hasValue(), "publish accepts the image");
    expectations.expect(wakeCount.load() > 0, "the wake hook fires after publish");
    const int beforeInvalidate = wakeCount.load();
    registry->invalidateAll();
    expectations.expect(wakeCount.load() > beforeInvalidate, "the wake hook fires on invalidation");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;

        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;

        std::string reason;
        auto host = makeHost(createOptions, expectations, &reason);
        if (!host) {
            if (options.require_device) {
                std::cerr << "FAIL: required Vulkan device unavailable: " << reason << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: " << reason << '\n';
            return expectations.failures() == 0 ? 0 : 1;
        }

        testTokenReleaseAndCollect(expectations, *host);
        testPinnedTombstone(expectations, *host);
        testActiveBudgetRefusal(expectations, *host);
        testForeignDeviceImage(expectations, *host, createOptions);
        testForeignRegistryAndStale(expectations, *host);
        testInvalidateAll(expectations, *host);
        testRegistryShutdown(expectations, *host);
        testCreateGate(expectations, *host);
        testNarrowGeometry(expectations, *host);
        testWrongThread(expectations, *host);
        testConcurrentReleaseStress(expectations, *host);
        testWakeCallback(expectations, *host);

        if (expectations.failures() != 0) {
            std::cerr << "FAIL: " << expectations.failures() << " lease assertion(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU-resident frame lease owner registry\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
