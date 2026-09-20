// Bounded native-resource-retirement tests for GpuSolid / CoveredSolidV1.
//
// These vectors prove the process-global bounded resident pool: many pre-created instances beyond
// capacity, foreign-thread destruction boundedness, retention of an unproven fence, exact
// owner-thread/device-generation isolation across two real device threads, and owner-drain recovery.
// The private fault seam is Vulkan-free, so this target exists only where the Vulkan backend is
// built; a hardware-free build prints an explicit skip and --require-device fails closed.

#include "gpu_solid_fault.hpp"

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image_types.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
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
using bloom::render::GpuSolid;
using bloom::render::GpuSolidDiagnosticCode;
using bloom::render::GpuSolidParameters;
using bloom::render::GpuSolidPollResult;
using bloom::render::ImageWindow;

namespace solid_detail = bloom::render::solid_detail;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
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

[[nodiscard]] std::optional<GpuSolidParameters> makeParameters() {
    const auto window = ImageWindow::create(0, 0, 8, 4);
    if (!window) {
        return std::nullopt;
    }
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.25, 0.5, 0.75, 1.0});
    if (!pixel) {
        return std::nullopt;
    }
    return GpuSolidParameters{*pixel.value(), *window.value(), *window.value(),
                              PixelAspectRatio::square()};
}

[[nodiscard]] bool pollToCompletion(GpuSolid& solid) {
    GpuSolidPollResult poll = GpuSolidPollResult::Pending;
    while (poll == GpuSolidPollResult::Pending) {
        poll = solid.poll();
    }
    return poll == GpuSolidPollResult::Ready;
}

[[nodiscard]] bool runSolid(GpuSolid& solid, const GpuSolidParameters& parameters) {
    if (solid.begin(parameters, 1ULL << 32ULL).code != GpuSolidDiagnosticCode::None) {
        return false;
    }
    return pollToCompletion(solid);
}

// Trigger the non-blocking owner-thread drain through the public create(), which drains orphaned
// foreign-released residents before constructing the (lazy, native-free) new instance.
void drainOwnerOrphans(GpuDevice& device) { (void)GpuSolid::create(device); }

// Many pre-created instances beyond capacity; foreign destruction preserves the bounded pool; the
// owner drain retires every orphan and admission recovers.
void testBoundedPool(Expectations& expectations, GpuDevice& device) {
    const auto parameters = makeParameters();
    expectations.expect(parameters.has_value(), "the pool fixture parameters build");
    if (!parameters) {
        return;
    }
    const std::size_t capacity = solid_detail::solidResidentCapacity();
    expectations.expect(capacity > 0, "the solid resident pool has a positive bounded capacity");
    const std::size_t inUseBefore = solid_detail::solidResidentInUse();
    const std::size_t orphanedBefore = solid_detail::solidResidentOrphaned();
    const std::uint64_t retiredBefore = solid_detail::solidResidentRetired();
    expectations.expect(inUseBefore <= capacity, "the solid pool invariant holds before the test");
    const std::size_t available = capacity - inUseBefore;
    expectations.expect(available > 0, "the solid pool has at least one free slot");
    if (available == 0) {
        return;
    }

    // 32 pre-created idle instances allocate no native resources and hold no slot.
    std::vector<std::unique_ptr<GpuSolid>> producers;
    for (int index = 0; index < 32; ++index) {
        auto created = GpuSolid::create(device);
        if (!created) {
            break;
        }
        producers.push_back(std::move(created.solid));
    }
    expectations.expect(producers.size() == 32, "32 idle solids are created");
    expectations.expect(solid_detail::solidResidentInUse() == inUseBefore,
                        "idle solids allocate no resident slot");

    // Sequential Ready fills exactly the available slots; the next begin is refused before any
    // native allocation.
    std::size_t ready = 0;
    for (auto& producer : producers) {
        if (!runSolid(*producer, *parameters)) {
            break;
        }
        ++ready;
    }
    expectations.expect(ready == available, "exactly the available solid slots become Ready");
    expectations.expect(solid_detail::solidResidentInUse() == capacity, "the solid pool is full");

    const std::uint64_t refusalsBefore = solid_detail::solidResidentRefusals();
    auto extra = GpuSolid::create(device);
    expectations.expect(extra.hasValue(), "an idle solid is still creatable when the pool is full");
    if (extra) {
        const auto refused = extra.solid->begin(*parameters, 1ULL << 32ULL);
        expectations.expect(refused.code == GpuSolidDiagnosticCode::DeviceUnavailable,
                            "a full solid pool refuses a begin cleanly");
        expectations.expect(solid_detail::solidResidentRefusals() == refusalsBefore + 1,
                            "the solid refusal is counted");
        expectations.expect(solid_detail::solidResidentInUse() == capacity,
                            "a refused solid begin allocates no resident slot");
    }

    // Foreign-destroy all 32: the resident holders are orphaned in their own slots, the idle ones
    // own no native state.
    std::thread worker([&producers]() { producers.clear(); });
    worker.join();
    expectations.expect(solid_detail::solidResidentInUse() == capacity,
                        "foreign destruction preserves the solid pool bound");
    expectations.expect(solid_detail::solidResidentOrphaned() == orphanedBefore + available,
                        "every solid resident holder is orphaned in its own slot");

    // Owner drain retires the orphaned residents without allocating and returns their slots.
    drainOwnerOrphans(device);
    expectations.expect(solid_detail::solidResidentOrphaned() == orphanedBefore,
                        "the solid owner drain empties the orphaned set");
    expectations.expect(solid_detail::solidResidentInUse() == inUseBefore,
                        "the solid drain returns every orphaned slot");
    expectations.expect(solid_detail::solidResidentRetired() == retiredBefore + available,
                        "every orphaned solid resident was retired on the owner thread");

    // Admission recovers.
    auto recovered = GpuSolid::create(device);
    expectations.expect(recovered.hasValue(), "a solid is creatable after recovery");
    if (recovered) {
        expectations.expect(runSolid(*recovered.solid, *parameters),
                            "a solid begin works after recovery");
    }
}

// An unproven fence is retained intact until the rightful owner proves retirement.
void testUnprovenRetention(Expectations& expectations, GpuDevice& device) {
    const auto parameters = makeParameters();
    expectations.expect(parameters.has_value(), "the retention fixture parameters build");
    if (!parameters) {
        return;
    }
    const std::size_t orphanedBefore = solid_detail::solidResidentOrphaned();
    const std::size_t inUseBefore = solid_detail::solidResidentInUse();
    const std::uint64_t retiredBefore = solid_detail::solidResidentRetired();

    auto created = GpuSolid::create(device);
    expectations.expect(created.hasValue(), "the retention solid is created");
    if (!created) {
        return;
    }
    auto solid = std::move(created.solid);
    expectations.expect(solid->begin(*parameters, 1ULL << 32ULL).code ==
                            GpuSolidDiagnosticCode::None,
                        "the retention begin is accepted");
    expectations.expect(solid->hasUnretiredSubmission(), "the retention submission is unretired");

    // Foreign-destroy while the submission is genuinely pending: the slot is orphaned, never
    // destroyed off-thread.
    std::thread worker([&solid]() { solid.reset(); });
    worker.join();
    expectations.expect(solid_detail::solidResidentOrphaned() == orphanedBefore + 1,
                        "the pending solid is orphaned");
    expectations.expect(solid_detail::solidResidentInUse() == inUseBefore + 1,
                        "the pending solid still holds its slot");

    // With the fence treated as unproven the drain must retain the exact slot.
    solid_detail::setSolidRetirementFaultForTest(
        solid_detail::SolidRetirementFault::ForceFenceTimeout);
    drainOwnerOrphans(device);
    expectations.expect(solid_detail::solidResidentOrphaned() == orphanedBefore + 1,
                        "an unproven solid fence retains its orphan");
    expectations.expect(solid_detail::solidResidentInUse() == inUseBefore + 1,
                        "an unproven solid fence retains its slot");
    expectations.expect(solid_detail::solidResidentRetired() == retiredBefore,
                        "nothing is retired while the solid fence is unproven");

    // Once the fence is proven the same owner drain frees it and returns the slot.
    solid_detail::setSolidRetirementFaultForTest(solid_detail::SolidRetirementFault::None);
    bool retired = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!retired && std::chrono::steady_clock::now() < deadline) {
        drainOwnerOrphans(device);
        retired = solid_detail::solidResidentOrphaned() == orphanedBefore;
        if (!retired) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    expectations.expect(retired, "the proven solid fence retires the orphan");
    expectations.expect(solid_detail::solidResidentInUse() == inUseBefore,
                        "the proven solid drain returns the slot");
    expectations.expect(solid_detail::solidResidentRetired() == retiredBefore + 1,
                        "the proven solid orphan was retired exactly once");
}

// Two real device owner threads. B's owner drain must skip A's orphan (wrong owner thread) and
// leave it untouched; A's owner drain then frees it. This is the exact-owner guard that prevents
// one device thread from querying a fence or tearing down another device's Vulkan resources.
struct OwnerThreadContext final {
    explicit OwnerThreadContext(GpuSolidParameters parametersIn) noexcept
        : parameters(std::move(parametersIn)) {}

    std::filesystem::path loaderPath;
    GpuSolidParameters parameters;
    std::unique_ptr<GpuDevice> device;
    std::unique_ptr<GpuSolid> solid;
    std::atomic<bool> ready{false};
    std::atomic<bool> drainNow{false};
    std::atomic<bool> drained{false};
    std::size_t orphanedAfter = 0;
    std::size_t inUseAfter = 0;
    std::uint64_t retiredAfter = 0;
    bool created = false;
    std::string error;
};

void runOwnerThread(OwnerThreadContext& context) {
    GpuDeviceCreationOptions options;
    options.loader_path = context.loaderPath;
    auto device = GpuDevice::create(options);
    if (!device) {
        context.error = "device: " + device.diagnostic.message;
        context.ready.store(true);
        return;
    }
    context.device = std::move(device.device);
    auto solid = GpuSolid::create(*context.device);
    if (!solid) {
        context.error = "solid: " + solid.diagnostic.message;
        context.ready.store(true);
        return;
    }
    if (!runSolid(*solid.solid, context.parameters)) {
        context.error = "solid did not reach Ready";
        context.ready.store(true);
        return;
    }
    context.solid = std::move(solid.solid);
    context.created = true;
    context.ready.store(true);
    while (!context.drainNow.load()) {
        std::this_thread::yield();
    }
    (void)GpuSolid::create(*context.device);
    context.orphanedAfter = solid_detail::solidResidentOrphaned();
    context.inUseAfter = solid_detail::solidResidentInUse();
    context.retiredAfter = solid_detail::solidResidentRetired();
    context.drained.store(true);
    // context.device is destroyed here, on its owner thread.
}

void testOwnerIsolation(Expectations& expectations, const std::filesystem::path& loaderPath) {
    const auto parameters = makeParameters();
    expectations.expect(parameters.has_value(), "the owner-isolation parameters build");
    if (!parameters) {
        return;
    }
    const std::size_t inUseBefore = solid_detail::solidResidentInUse();
    const std::size_t orphanedBefore = solid_detail::solidResidentOrphaned();
    const std::uint64_t retiredBefore = solid_detail::solidResidentRetired();

    OwnerThreadContext a(*parameters);
    a.loaderPath = loaderPath;
    OwnerThreadContext b(*parameters);
    b.loaderPath = loaderPath;
    std::thread threadA([&a] { runOwnerThread(a); });
    std::thread threadB([&b] { runOwnerThread(b); });
    while (!a.ready.load() || !b.ready.load()) {
        std::this_thread::yield();
    }
    expectations.expect(a.created && b.created, "both owner-thread solids become Ready");
    if (!a.created || !b.created) {
        std::cerr << "owner-thread A: " << a.error << " ; B: " << b.error << '\n';
    }
    if (a.created && b.created) {
        // Orphan both solids from this foreign thread.
        a.solid.reset();
        b.solid.reset();
        expectations.expect(solid_detail::solidResidentOrphaned() == orphanedBefore + 2,
                            "both owner-thread solids are orphaned");
        expectations.expect(solid_detail::solidResidentInUse() == inUseBefore + 2,
                            "both owner-thread solids hold slots");

        // B drains first: it owns B's orphan but not A's, so A's must be left untouched.
        b.drainNow.store(true);
        while (!b.drained.load()) {
            std::this_thread::yield();
        }
        expectations.expect(b.orphanedAfter == orphanedBefore + 1,
                            "B's drain leaves A's orphan untouched");
        expectations.expect(solid_detail::solidResidentOrphaned() == orphanedBefore + 1,
                            "a wrong-owner drain does not touch another owner's orphan");
        expectations.expect(solid_detail::solidResidentRetired() == retiredBefore + 1,
                            "B retires exactly its own solid orphan");

        // A drains its own orphan.
        a.drainNow.store(true);
        while (!a.drained.load()) {
            std::this_thread::yield();
        }
        expectations.expect(solid_detail::solidResidentOrphaned() == orphanedBefore,
                            "A's drain frees A's orphan");
        expectations.expect(solid_detail::solidResidentInUse() == inUseBefore,
                            "all owner-thread solid slots are returned");
        expectations.expect(solid_detail::solidResidentRetired() == retiredBefore + 2,
                            "both owners retire their own solid orphans");
    } else {
        a.drainNow.store(true);
        b.drainNow.store(true);
    }
    threadA.join();
    threadB.join();
}

// Cancellation is honest: it publishes nothing, retires the submission, and leaves the instance
// reusable on the owner thread.
void testCancelHonest(Expectations& expectations, GpuDevice& device) {
    const auto parameters = makeParameters();
    expectations.expect(parameters.has_value(), "the cancel fixture parameters build");
    if (!parameters) {
        return;
    }
    auto created = GpuSolid::create(device);
    expectations.expect(created.hasValue(), "the cancellable solid is created");
    if (!created) {
        return;
    }
    GpuSolid& solid = *created.solid;
    expectations.expect(solid.begin(*parameters, 1ULL << 32ULL).code ==
                            GpuSolidDiagnosticCode::None,
                        "the cancellable solid begin is accepted");
    solid.cancel();
    expectations.expect(pollToCompletion(solid) == false &&
                            solid.diagnostic().code == GpuSolidDiagnosticCode::Cancelled,
                        "a cancelled solid fails closed as Cancelled");
    expectations.expect(solid.image() == nullptr, "a cancelled solid publishes no image");
    expectations.expect(!solid.hasUnretiredSubmission(),
                        "a cancelled solid retires its submission honestly");
    expectations.expect(runSolid(solid, *parameters), "the solid is reusable after cancellation");
}

} // namespace

int main(const int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;

        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return expectations.ok() ? 0 : 1;
        }

        testBoundedPool(expectations, *device.device);
        testUnprovenRetention(expectations, *device.device);
        testOwnerIsolation(expectations, options.loader_path);
        testCancelHonest(expectations, *device.device);

        if (!expectations.ok()) {
            std::cerr << "FAIL: solid retirement expectations failed\n";
            return 1;
        }
        std::cout << "PASS: solid bounded retirement\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
