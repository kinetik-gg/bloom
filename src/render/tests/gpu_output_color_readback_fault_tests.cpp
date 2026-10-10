// Deterministic fault-injection tests for the combined output-colour readback reservation:
//  * a forced staging-allocation failure and a forced submit failure each fail closed with no
//    occupied reservation, and the object stays reusable;
//  * a forced fence timeout quarantines the exact submission (bounded, retained), refuses new
//    admission on the same owner until it retires, then recovers after genuine retirement;
//  * a foreign-thread destruction retains the one occupied reservation instead of leaking a new
//    native submission, and admission fails closed.
//
// Links the test-only fault library; production targets never link it.

#include "final_host_test_support.hpp"
#include "gpu_composite_native_support.hpp"

#include "gpu_output_color_readback_fault.hpp"

#include <bloom/render/gpu_output_color_readback.hpp>
#include <bloom/render/gpu_processing_phase_budget.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

namespace {

using namespace bloom::render::composite_proof;
using bloom::render::GpuOutputColorReadback;
using bloom::render::GpuOutputColorReadbackCode;
using bloom::render::GpuOutputColorReadbackState;
using bloom::render::output_color_readback_detail::outputColorQuarantineOccupiedForTest;
using bloom::render::output_color_readback_detail::ReadbackFault;
using bloom::render::output_color_readback_detail::retireOutputColorQuarantineForTest;
using bloom::render::output_color_readback_detail::setReadbackFaultForTest;

[[nodiscard]] std::shared_ptr<const GpuImage>
makeSolid(GpuSolid& solidOp, const std::uint32_t width, const std::uint32_t height) {
    const auto w = window(0, 0, width, height);
    auto image = solid(solidOp, {0.4, 0.5, 0.6, 1.0}, w, w, PixelAspectRatio::square());
    if (!image) {
        return nullptr;
    }
    return std::make_shared<const GpuImage>(std::move(*image));
}

[[nodiscard]] GpuOutputColorReadbackState drain(GpuOutputColorReadback& readback) {
    auto poll = readback.state();
    while (poll == GpuOutputColorReadbackState::Pending) {
        poll = readback.poll();
    }
    return poll;
}

void testAllocationFault(Expectations& expectations, GpuDevice& device) {
    auto solidOp = GpuSolid::create(device);
    if (!solidOp) {
        expectations.expect(false, "alloc-fault: solid op created");
        return;
    }
    const auto source = makeSolid(*solidOp.solid, 4, 4);
    if (source == nullptr) {
        expectations.expect(false, "alloc-fault: image built");
        return;
    }
    setReadbackFaultForTest(ReadbackFault::FailStagingAllocation);
    GpuOutputColorReadback readback;
    const bool began = readback.begin(source, nullptr, std::nullopt, kBudget);
    expectations.expect(!began && readback.diagnostic().code ==
                                      GpuOutputColorReadbackCode::ReadbackFailed,
                        "alloc-fault: a forced staging failure is a typed ReadbackFailed");
    expectations.expect(!outputColorQuarantineOccupiedForTest(),
                        "alloc-fault: no reservation is left occupied");
    setReadbackFaultForTest(ReadbackFault::None);
    expectations.expect(readback.begin(source, nullptr, std::nullopt, kBudget),
                        "alloc-fault: the object is reusable after the fault clears");
    expectations.expect(drain(readback) == GpuOutputColorReadbackState::Ready,
                        "alloc-fault: the recovered readback completes");
    static_cast<void>(readback.take());
}

void testSubmitFault(Expectations& expectations, GpuDevice& device) {
    auto solidOp = GpuSolid::create(device);
    if (!solidOp) {
        expectations.expect(false, "submit-fault: solid op created");
        return;
    }
    const auto source = makeSolid(*solidOp.solid, 4, 4);
    if (source == nullptr) {
        expectations.expect(false, "submit-fault: image built");
        return;
    }
    setReadbackFaultForTest(ReadbackFault::FailSubmit);
    GpuOutputColorReadback readback;
    const bool began = readback.begin(source, nullptr, std::nullopt, kBudget);
    expectations.expect(!began && readback.diagnostic().code ==
                                      GpuOutputColorReadbackCode::ReadbackFailed,
                        "submit-fault: a forced submit failure is a typed ReadbackFailed");
    expectations.expect(!outputColorQuarantineOccupiedForTest(),
                        "submit-fault: no reservation is left occupied");
    setReadbackFaultForTest(ReadbackFault::None);
    expectations.expect(readback.begin(source, nullptr, std::nullopt, kBudget),
                        "submit-fault: the object is reusable after the fault clears");
    expectations.expect(drain(readback) == GpuOutputColorReadbackState::Ready,
                        "submit-fault: the recovered readback completes");
    static_cast<void>(readback.take());
}

void testFenceTimeoutQuarantine(Expectations& expectations, GpuDevice& device) {
    auto solidOp = GpuSolid::create(device);
    if (!solidOp) {
        expectations.expect(false, "timeout: solid op created");
        return;
    }
    const auto source = makeSolid(*solidOp.solid, 8, 4);
    if (source == nullptr) {
        expectations.expect(false, "timeout: image built");
        return;
    }
    setReadbackFaultForTest(ReadbackFault::ForceFenceTimeout);
    GpuOutputColorReadback readback;
    expectations.expect(readback.begin(source, nullptr, std::nullopt, kBudget),
                        "timeout: begin accepted");
    expectations.expect(drain(readback) == GpuOutputColorReadbackState::Failure &&
                            readback.diagnostic().code ==
                                GpuOutputColorReadbackCode::ReadbackFailed,
                        "timeout: an unproven submission fails closed");
    expectations.expect(outputColorQuarantineOccupiedForTest(),
                        "timeout: the exact submission is retained in the bounded quarantine");

    // New admission on the same owner thread is refused while the quarantine is un-retirable.
    GpuOutputColorReadback second;
    const bool secondBegan = second.begin(source, nullptr, std::nullopt, kBudget);
    expectations.expect(!secondBegan && second.diagnostic().code ==
                                            GpuOutputColorReadbackCode::DeviceUnavailable,
                        "timeout: admission is refused while the quarantine is occupied");

    // Clearing the fault lets the genuine owner prove retirement and free the quarantine. The
    // forced-timeout poll may run before the (real, tiny) submission's fence has signalled, so poll
    // for bounded retirement rather than assuming immediate completion.
    setReadbackFaultForTest(ReadbackFault::None);
    bool retired = false;
    for (int attempt = 0; attempt < 400 && !retired; ++attempt) {
        retired = retireOutputColorQuarantineForTest();
        if (!retired) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    expectations.expect(retired, "timeout: the owner retires the quarantine after fence proof");
    expectations.expect(!outputColorQuarantineOccupiedForTest(),
                        "timeout: the quarantine is freed after retirement");
    expectations.expect(second.begin(source, nullptr, std::nullopt, kBudget),
                        "timeout: admission recovers after retirement");
    expectations.expect(drain(second) == GpuOutputColorReadbackState::Ready,
                        "timeout: the recovered readback completes");
    static_cast<void>(second.take());
}

// Recoverable foreign release hands the preowned slot to the exact native owner. No unknown
// observation is delivered to poll(), so owner drain must permit reuse without clearing a latch.
void testForeignThreadDestructionRetains(Expectations& expectations, GpuDevice& device) {
    bloom::render::test::FinalHostRetirementScope host;
    bloom::render::GpuProcessingPhaseBudget budget(1, host.owner);
    budget.setHostCapacity(1ULL << 30, true);
    budget.setDeviceCapacity(1ULL << 30, true);
    auto grant = budget.reserve({.fixedHostBytes = 1ULL << 20,
                                 .requestedExecutorBytes = 1ULL << 20,
                                 .minimumExecutorBytes = 1ULL << 20,
                                 .sourceFree = true});
    if (!grant.lease || !device.retainProcessingPhase(grant.lease)) {
        expectations.expect(false, "foreign-dtor: the exact one-slot phase is retained");
        return;
    }
    auto phase = std::move(grant.lease);
    auto solidOp = GpuSolid::create(device);
    auto source = solidOp ? makeSolid(*solidOp.solid, 4, 4) : nullptr;
    if (!source) {
        expectations.expect(false, "foreign-dtor: rooted source exists");
        return;
    }
    auto readback = std::make_unique<GpuOutputColorReadback>();
    if (!readback->begin(source, nullptr, std::nullopt, kBudget, phase)) {
        expectations.expect(false, "foreign-dtor: real submission is accepted");
        return;
    }
    const auto held = budget.snapshot();
    expectations.expect(held.ownedNativeBufferBytes > 0 && held.ownedNativeBuffers == 1,
                        "foreign-dtor: real staging backing is charged before handoff");
    // Hold only the observed fence status during handoff; no owner poll sees an unknown failure.
    setReadbackFaultForTest(ReadbackFault::ForceFenceTimeout);
    std::thread foreign([owned = std::move(readback), &device]() mutable {
        owned.reset();
        static_cast<void>(device.reclaimReleasedImages());
    });
    foreign.join();
    const auto pending = budget.snapshot();
    expectations.expect(outputColorQuarantineOccupiedForTest() &&
                            device.outputColorReadbackPending() &&
                            pending.ownedNativeBufferBytes == held.ownedNativeBufferBytes &&
                            pending.ownedNativeBuffers == 1 && pending.unprovenPhases == 0,
                        "foreign-dtor: wrong-owner tick keeps exact backing and phase charged");
    GpuOutputColorReadback recovered;
    expectations.expect(!recovered.begin(source, nullptr, std::nullopt, kBudget, phase) &&
                            recovered.diagnostic().code ==
                                GpuOutputColorReadbackCode::DeviceUnavailable,
                        "foreign-dtor: occupied submission refuses a second admission");
    setReadbackFaultForTest(ReadbackFault::None);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (device.outputColorReadbackPending() && std::chrono::steady_clock::now() < until) {
        static_cast<void>(device.reclaimReleasedImages());
        std::this_thread::yield();
    }
    expectations.expect(!device.outputColorReadbackPending() &&
                            budget.snapshot().ownedNativeBuffers == 0,
                        "foreign-dtor: actual owner fence/backing drain precedes credit");
    expectations.expect(recovered.begin(source, nullptr, std::nullopt, kBudget, phase) &&
                            drain(recovered) == GpuOutputColorReadbackState::Ready,
                        "foreign-dtor: same-generation transfer recovers without latch clearing");
    static_cast<void>(recovered.take());
    source.reset();
    solidOp.solid.reset();
    device.retireProcessingPhase(phase->id(), true);
    phase.reset();
    const auto hostUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (budget.snapshot().ownedFinalHostAllocations != 0 &&
           std::chrono::steady_clock::now() < hostUntil)
        std::this_thread::yield();
    const auto final = budget.snapshot();
    expectations.expect(final.ownedNativeBuffers == 0 && final.ownedNativeImages == 0 &&
                            final.totalHostClaimBytes == 0 && final.totalDeviceClaimBytes == 0 &&
                            final.admittedPhases == 0 && final.unprovenPhases == 0,
                        "foreign-dtor: backing destruction and one-slot phase end conserve claims");
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
        testAllocationFault(expectations, *device.device);
        testSubmitFault(expectations, *device.device);
        testFenceTimeoutQuarantine(expectations, *device.device);
        testForeignThreadDestructionRetains(expectations, *device.device);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " readback fault expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: output-colour readback faults\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
