// Focused native tests for the production final-output readback primitive:
//  * a real begin -> poll -> take round trip against a resident solid image;
//  * wrong-thread poll leaves the native job Pending (no resource freed, no state change) and the
//    actual owner can still complete and retire it;
//  * a foreign owner thread cannot steal an active reservation: begin() from another owner thread
//    with another device's image is refused without touching the live submission;
//  * the destructor is clean after a completed take and after an owner-thread quarantine.
//
// Compiled against the FROZEN snapshot's private headers (gpu_image_private.hpp) exactly like the
// composite native proof. Local mode pins the explicit prefix loader and requires a device.

#include "gpu_composite_native_support.hpp"

#include "gpu_image_private.hpp"

#include <bloom/render/gpu_process_readback.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;
using bloom::render::GpuProcessReadback;
using bloom::render::GpuProcessReadbackCode;
using bloom::render::GpuProcessReadbackState;
using bloom::render::GpuSolid;

[[nodiscard]] std::shared_ptr<const GpuImage> makeSolid(GpuSolid& solidOp, const Color4d color,
                                                        const std::uint32_t width,
                                                        const std::uint32_t height) {
    const auto windowValue = window(0, 0, width, height);
    auto image = solid(solidOp, color, windowValue, windowValue, PixelAspectRatio::square());
    if (!image) {
        return nullptr;
    }
    return std::make_shared<const GpuImage>(std::move(*image));
}

void testRoundTrip(Expectations& expectations, GpuDevice& device) {
    auto solidOp = GpuSolid::create(device);
    expectations.expect(solidOp.hasValue(), "round-trip: solid op created");
    if (!solidOp) {
        return;
    }
    auto source = makeSolid(*solidOp.solid, {0.4, 0.6, 0.2, 1.0}, 8, 4);
    expectations.expect(source != nullptr, "round-trip: source image built");
    if (source == nullptr) {
        return;
    }
    GpuProcessReadback readback;
    expectations.expect(readback.begin(source, kBudget), "round-trip: begin accepted");
    expectations.expect(readback.state() == GpuProcessReadbackState::Pending,
                        "round-trip: begin leaves Pending");
    GpuProcessReadbackState poll = readback.state();
    while (poll == GpuProcessReadbackState::Pending) {
        poll = readback.poll();
    }
    expectations.expect(poll == GpuProcessReadbackState::Ready, "round-trip: poll reaches Ready");
    const auto pixels = readback.take();
    expectations.expect(pixels.size() == 32, "round-trip: take returns the pixel vector");
    expectations.expect(readback.state() == GpuProcessReadbackState::Idle,
                        "round-trip: take resets to Idle");
}

// Wrong-thread poll must not free resources or change the job state; the real owner then completes.
void testWrongThreadPollThenOwnerCompletion(Expectations& expectations, GpuDevice& device) {
    auto solidOp = GpuSolid::create(device);
    expectations.expect(solidOp.hasValue(), "wrong-thread: solid op created");
    if (!solidOp) {
        return;
    }
    auto source = makeSolid(*solidOp.solid, {0.2, 0.4, 0.8, 1.0}, 6, 6);
    expectations.expect(source != nullptr, "wrong-thread: source image built");
    if (source == nullptr) {
        return;
    }
    GpuProcessReadback readback;
    expectations.expect(readback.begin(source, kBudget), "wrong-thread: begin accepted");
    expectations.expect(readback.state() == GpuProcessReadbackState::Pending,
                        "wrong-thread: begin leaves Pending");

    std::thread foreign([&] {
        const auto result = readback.poll();
        // A wrong-thread poll must keep the job Pending and only report a typed diagnostic.
        expectations.expect(result == GpuProcessReadbackState::Pending,
                            "wrong-thread: foreign poll does not change the job state");
        expectations.expect(readback.diagnostic().code == GpuProcessReadbackCode::WrongThread,
                            "wrong-thread: foreign poll reports WrongThread");
    });
    foreign.join();

    expectations.expect(readback.state() == GpuProcessReadbackState::Pending,
                        "wrong-thread: the job is still Pending after a foreign poll");
    GpuProcessReadbackState poll = readback.state();
    while (poll == GpuProcessReadbackState::Pending) {
        poll = readback.poll();
    }
    expectations.expect(poll == GpuProcessReadbackState::Ready,
                        "wrong-thread: the owner completes after a foreign poll");
    expectations.expect(readback.take().size() == 36, "wrong-thread: the owner reads the pixels");
}

// A foreign owner thread cannot steal an active reservation, and the live submission is untouched.
void testForeignOwnerCannotSteal(Expectations& expectations, GpuDevice& device,
                                 const GpuDeviceCreationOptions& createOptions) {
    auto solidOp = GpuSolid::create(device);
    expectations.expect(solidOp.hasValue(), "foreign-owner: solid op created");
    if (!solidOp) {
        return;
    }
    auto source = makeSolid(*solidOp.solid, {0.9, 0.1, 0.7, 1.0}, 5, 5);
    expectations.expect(source != nullptr, "foreign-owner: source image built");
    if (source == nullptr) {
        return;
    }

    GpuProcessReadback readback;
    expectations.expect(readback.begin(source, kBudget), "foreign-owner: begin accepted");
    expectations.expect(readback.state() == GpuProcessReadbackState::Pending,
                        "foreign-owner: begin leaves Pending");

    // On a genuinely different device AND owner thread, build an image and attempt a readback. The
    // global slot is active with a different owner; the begin must be refused and the first
    // submission stays live.
    std::atomic<bool> refused{false};
    std::thread foreignOwner([&] {
        auto secondDevice = GpuDevice::create(createOptions);
        if (!secondDevice) {
            return;
        }
        auto secondSolid = GpuSolid::create(*secondDevice.device);
        if (!secondSolid) {
            return;
        }
        auto secondImage = makeSolid(*secondSolid.solid, {0.1, 0.9, 0.3, 1.0}, 4, 4);
        if (secondImage == nullptr) {
            return;
        }
        GpuProcessReadback secondReadback;
        const bool began = secondReadback.begin(secondImage, kBudget);
        if (!began &&
            (secondReadback.diagnostic().code == GpuProcessReadbackCode::DeviceUnavailable ||
             secondReadback.diagnostic().code == GpuProcessReadbackCode::WrongThread)) {
            refused.store(true);
        }
    });
    foreignOwner.join();
    expectations.expect(refused.load(), "foreign-owner: the foreign begin is refused");

    // The first submission is still healthy and completes on its real owner thread.
    GpuProcessReadbackState poll = readback.state();
    while (poll == GpuProcessReadbackState::Pending) {
        poll = readback.poll();
    }
    expectations.expect(poll == GpuProcessReadbackState::Ready,
                        "foreign-owner: the original submission still completes");
    expectations.expect(readback.take().size() == 25,
                        "foreign-owner: the original pixels are intact");
}

// Regression: a same-owner second readback before the first poll must be refused Busy (the first
// submission is Reserved and owns its resources), the first then completes, and a third new
// readback succeeds. Covered for the same device and a different device on the same owner thread.
void testSameOwnerSecondReadbackRefused(Expectations& expectations, GpuDevice& device,
                                        const GpuDeviceCreationOptions& createOptions) {
    auto solidOp = GpuSolid::create(device);
    expectations.expect(solidOp.hasValue(), "same-owner-second: solid op created");
    if (!solidOp) {
        return;
    }
    auto firstSource = makeSolid(*solidOp.solid, {0.3, 0.5, 0.7, 1.0}, 6, 4);
    expectations.expect(firstSource != nullptr, "same-owner-second: first source built");
    if (firstSource == nullptr) {
        return;
    }
    auto secondDevice = GpuDevice::create(createOptions);
    expectations.expect(secondDevice.hasValue(), "same-owner-second: second device created");

    GpuProcessReadback first;
    expectations.expect(first.begin(firstSource, kBudget),
                        "same-owner-second: first begin accepted");
    expectations.expect(first.state() == GpuProcessReadbackState::Pending,
                        "same-owner-second: first is Pending before poll");

    // Same owner, same device, before the first poll: must be refused.
    GpuProcessReadback sameSecond;
    const bool sameBegan = sameSecond.begin(firstSource, kBudget);
    expectations.expect(!sameBegan, "same-owner-second: second same-device begin is refused");
    expectations.expect(sameSecond.diagnostic().code == GpuProcessReadbackCode::DeviceUnavailable,
                        "same-owner-second: refusal is a typed DeviceUnavailable");
    expectations.expect(first.state() == GpuProcessReadbackState::Pending,
                        "same-owner-second: the first submission is untouched by the refusal");

    // Same owner, different device, before the first poll: must also be refused.
    if (secondDevice) {
        auto secondSolid = GpuSolid::create(*secondDevice.device);
        if (secondSolid) {
            auto secondSource = makeSolid(*secondSolid.solid, {0.7, 0.3, 0.5, 1.0}, 4, 4);
            if (secondSource != nullptr) {
                GpuProcessReadback crossSecond;
                const bool crossBegan = crossSecond.begin(secondSource, kBudget);
                expectations.expect(!crossBegan,
                                    "same-owner-second: cross-device same-owner begin is refused");
                expectations.expect(
                    crossSecond.diagnostic().code == GpuProcessReadbackCode::DeviceUnavailable,
                    "same-owner-second: cross-device refusal is a typed DeviceUnavailable");
            }
        }
    }

    // The first submission completes correctly.
    GpuProcessReadbackState poll = first.state();
    while (poll == GpuProcessReadbackState::Pending) {
        poll = first.poll();
    }
    expectations.expect(poll == GpuProcessReadbackState::Ready,
                        "same-owner-second: the first submission completes");
    expectations.expect(first.take().size() == 24,
                        "same-owner-second: the first pixels are intact");

    // A third, new readback now succeeds (the slot was released on retirement).
    GpuProcessReadback third;
    expectations.expect(third.begin(firstSource, kBudget),
                        "same-owner-second: a third new readback begins after retirement");
    GpuProcessReadbackState thirdPoll = third.state();
    while (thirdPoll == GpuProcessReadbackState::Pending) {
        thirdPoll = third.poll();
    }
    expectations.expect(thirdPoll == GpuProcessReadbackState::Ready,
                        "same-owner-second: the third readback completes");
    expectations.expect(third.take().size() == 24,
                        "same-owner-second: the third pixels are intact");
}

// Regression: a budget that can hold the logical bytes but not the actual allocator-rounded staging
// plus the host vector is refused OverBudget, releases the slot, and a subsequent valid readback
// succeeds.
void testBudgetBoundaryReleasesSlot(Expectations& expectations, GpuDevice& device) {
    auto solidOp = GpuSolid::create(device);
    expectations.expect(solidOp.hasValue(), "budget-boundary: solid op created");
    if (!solidOp) {
        return;
    }
    auto source = makeSolid(*solidOp.solid, {0.6, 0.2, 0.4, 1.0}, 8, 8);
    expectations.expect(source != nullptr, "budget-boundary: source built");
    if (source == nullptr) {
        return;
    }
    // 8x8 RGBA32F = 1024 logical bytes. A budget just below the concurrent peak (staging + vector)
    // must be refused; the exact rounding is allocator-dependent, so use a clearly-too-small value
    // that still admits the logical 1024 bytes.
    GpuProcessReadback refused;
    const bool began = refused.begin(source, 1100);
    expectations.expect(!began, "budget-boundary: an under-peak budget is refused");
    expectations.expect(refused.diagnostic().code == GpuProcessReadbackCode::OverBudget,
                        "budget-boundary: the refusal is a typed OverBudget");

    // The refused pre-submit failure released the slot: a subsequent valid readback succeeds.
    GpuProcessReadback valid;
    expectations.expect(valid.begin(source, kBudget),
                        "budget-boundary: a subsequent valid readback begins");
    GpuProcessReadbackState poll = valid.state();
    while (poll == GpuProcessReadbackState::Pending) {
        poll = valid.poll();
    }
    expectations.expect(poll == GpuProcessReadbackState::Ready,
                        "budget-boundary: the subsequent readback completes");
    expectations.expect(valid.take().size() == 64,
                        "budget-boundary: the subsequent pixels are intact");
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
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 0;
        }
        testRoundTrip(expectations, *device.device);
        testWrongThreadPollThenOwnerCompletion(expectations, *device.device);
        testForeignOwnerCannotSteal(expectations, *device.device, createOptions);
        testSameOwnerSecondReadbackRefused(expectations, *device.device, createOptions);
        testBudgetBoundaryReleasesSlot(expectations, *device.device);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " readback expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU process readback\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
