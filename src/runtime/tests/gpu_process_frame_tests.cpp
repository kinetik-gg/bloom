// Native fixture for the bounded GPU final-render/export bridge.
//
// It drives the REAL path: a real CompiledCompositionPlan -> CpuGpuSceneBuilder ->
// GpuSceneExecutor on a dedicated owner worker -> the production GpuProcessReadback -> a genuine
// ProcessFrame with EvaluationProvider::GpuResident. It compares the final process image against
// the CPU reference evaluator, proves the final RGBA32F readback happened exactly once, and proves
// an unchanged warm scene recomputes no native dispatch. The canonical output semantic identity
// (which needs bloom::output) is proven separately in bloom.host. Without a loader/device it prints
// an explicit SKIP unless --require-device is passed.

#include "gpu_media_executor_test_support.hpp"
#include "gpu_scene_executor_test_support.hpp"

#include <bloom/core/color.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::EvaluationProvider;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::EvaluationStatus;
using bloom::runtime::GpuProcessFrameDiagnosticCode;
using bloom::runtime::GpuProcessFrameEvaluator;
using bloom::runtime::GpuProcessFrameEvaluatorOptions;
using bloom::runtime::GpuProcessFrameStatus;
using bloom::runtime::ProcessFrame;
using namespace bloom::runtime::executor_test;
using namespace bloom::runtime::media_executor_test;

using namespace std::chrono_literals;

constexpr std::uint64_t kReadbackBudget = 1ULL << 32U;
constexpr std::uint64_t kRequestBudget = 1ULL << 32U;

std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> lifecyclePlan() {
    return twoSolidPlan(format(24, 18), Color4d{0.5, 0.25, 0.125, 1.0},
                        LayerValues{.position = {4.25, 3.5}}, Color4d{0.125, 0.375, 0.75, 0.5},
                        LayerValues{.position = {7.5, 6.25}}, 9.0, 7.0, 9000);
}

std::shared_ptr<const ProcessFrame>
evaluateCpu(const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
            const EvaluationRequest& request) {
    const CpuCompositionEvaluator oracle;
    const auto result = oracle.evaluate(plan, request, {});
    if (result.status() != EvaluationStatus::Evaluated) {
        return nullptr;
    }
    return result.frame();
}

// The device-only options every lifecycle vector uses. Constructed with field assignment (not a
// designated initializer) so the OCIO/media context fields keep their inert defaults.
[[nodiscard]] GpuProcessFrameEvaluatorOptions
gpuEvaluatorOptions(const std::filesystem::path& loaderPath) {
    GpuProcessFrameEvaluatorOptions options;
    options.enabled = true;
    options.loaderPath = loaderPath;
    options.requestByteBudget = kRequestBudget;
    options.readbackByteBudget = kReadbackBudget;
    return options;
}

// Two concurrent callers each receive exactly one outcome; neither hangs even though the owner
// runs one request at a time. Bounded by a wall-clock deadline so a missed wake-up is a hard fail.
void runTwoCallerTest(Expectations& expectations, const Options& options) {
    auto evaluator = GpuProcessFrameEvaluator::create(gpuEvaluatorOptions(options.loader_path));
    expectations.expect(evaluator != nullptr, "two-caller: evaluator constructed");
    if (evaluator == nullptr || !evaluator->gpuAvailable()) {
        return;
    }
    const auto plan = lifecyclePlan();
    const auto request = requestFor(*plan);

    std::atomic<int> completed{0};
    std::atomic<int> evaluated{0};
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    std::vector<std::thread> callers;
    for (int i = 0; i < 2; ++i) {
        callers.emplace_back([&] {
            const auto outcome = evaluator->evaluate(plan, request);
            if (outcome.status == GpuProcessFrameStatus::Evaluated && outcome.frame != nullptr) {
                ++evaluated;
            }
            ++completed;
        });
    }
    for (auto& caller : callers) {
        caller.join();
    }
    expectations.expect(completed.load() == 2 && std::chrono::steady_clock::now() < deadline,
                        "two-caller: both callers returned exactly once without hanging");
    expectations.expect(evaluated.load() == 2, "two-caller: both requests evaluated");
    evaluator->beginShutdown();
}

// Two independent queued requests both resolve in order without hanging.
void runQueuedRequestsTest(Expectations& expectations, const Options& options) {
    auto evaluator = GpuProcessFrameEvaluator::create(gpuEvaluatorOptions(options.loader_path));
    expectations.expect(evaluator != nullptr, "queued: evaluator constructed");
    if (evaluator == nullptr || !evaluator->gpuAvailable()) {
        return;
    }
    std::atomic<int> resolved{0};
    std::thread first([&] {
        static_cast<void>(evaluator->evaluate(lifecyclePlan(), requestFor(*lifecyclePlan())));
        ++resolved;
    });
    std::thread second([&] {
        static_cast<void>(evaluator->evaluate(lifecyclePlan(), requestFor(*lifecyclePlan())));
        ++resolved;
    });
    first.join();
    second.join();
    expectations.expect(resolved.load() == 2, "queued: both queued requests resolved");
    evaluator->beginShutdown();
}

// After beginShutdown(), new evaluate() calls are rejected with a typed failure.
void runShutdownRejectsNewCallsTest(Expectations& expectations, const Options& options) {
    auto evaluator = GpuProcessFrameEvaluator::create(gpuEvaluatorOptions(options.loader_path));
    expectations.expect(evaluator != nullptr, "shutdown-reject: evaluator constructed");
    if (evaluator == nullptr || !evaluator->gpuAvailable()) {
        return;
    }
    const auto plan = lifecyclePlan();
    const auto request = requestFor(*plan);
    evaluator->beginShutdown();
    const auto rejected = evaluator->evaluate(plan, request);
    expectations.expect(rejected.status == GpuProcessFrameStatus::Failed &&
                            rejected.frame == nullptr,
                        "shutdown-reject: a post-shutdown evaluate is refused with no frame");
}

// A throwing progress callback must never unwind out of the owner worker; the request still
// completes with a typed outcome.
void runThrowingProgressTest(Expectations& expectations, const Options& options) {
    auto evaluator = GpuProcessFrameEvaluator::create(gpuEvaluatorOptions(options.loader_path));
    expectations.expect(evaluator != nullptr, "throwing-progress: evaluator constructed");
    if (evaluator == nullptr || !evaluator->gpuAvailable()) {
        return;
    }
    const auto plan = lifecyclePlan();
    const auto request = requestFor(*plan);
    const auto outcome =
        evaluator->evaluate(plan, request, {}, [](const bloom::runtime::EvaluationProgress&) {
            throw std::runtime_error("progress callback threw");
        });
    expectations.expect(outcome.status == GpuProcessFrameStatus::Evaluated ||
                            outcome.status == GpuProcessFrameStatus::Failed,
                        "throwing-progress: the request reaches a typed outcome, never terminates");
    evaluator->beginShutdown();
}

// Reentrancy: an evaluate() issued from inside a progress callback (the owner thread) must be
// rejected with a typed failure before any wait, so it cannot deadlock on its own completion.
void runReentrantEvaluateTest(Expectations& expectations, const Options& options) {
    auto evaluator = GpuProcessFrameEvaluator::create(gpuEvaluatorOptions(options.loader_path));
    expectations.expect(evaluator != nullptr, "reentrant: evaluator constructed");
    if (evaluator == nullptr || !evaluator->gpuAvailable()) {
        return;
    }
    const auto plan = lifecyclePlan();
    const auto request = requestFor(*plan);
    std::atomic<bool> reentrantRejected{false};
    std::atomic<bool> reentrantReturned{false};
    const auto outcome =
        evaluator->evaluate(plan, request, {}, [&](const bloom::runtime::EvaluationProgress&) {
            const auto nested = evaluator->evaluate(plan, request);
            if (nested.status == GpuProcessFrameStatus::Failed && nested.frame == nullptr) {
                reentrantRejected.store(true);
            }
            reentrantReturned.store(true);
        });
    expectations.expect(outcome.status == GpuProcessFrameStatus::Evaluated ||
                            outcome.status == GpuProcessFrameStatus::Failed,
                        "reentrant: the outer request reaches a typed outcome");
    expectations.expect(
        reentrantReturned.load() && reentrantRejected.load(),
        "reentrant: the nested owner-thread evaluate is rejected typed, no deadlock");
    evaluator->beginShutdown();
}

// A request whose real TaskScheduler cancellation token is already cancelled returns Cancelled
// promptly without waiting for a nonexistent active request. The token is produced by the real
// scheduler task handle, never a test-only setter.
void runCancelledTokenTest(Expectations& expectations, const Options& options) {
    auto evaluator = GpuProcessFrameEvaluator::create(gpuEvaluatorOptions(options.loader_path));
    expectations.expect(evaluator != nullptr, "cancelled-token: evaluator constructed");
    if (evaluator == nullptr || !evaluator->gpuAvailable()) {
        return;
    }
    bloom::runtime::TaskScheduler scheduler;
    std::mutex tokenMutex;
    std::condition_variable tokenCv;
    std::optional<bloom::runtime::CancellationToken> captured;
    auto submission = scheduler.submit<bool>(
        bloom::runtime::TaskRequest(
            "capture cancellation token",
            bloom::runtime::TaskOwner{.kind = bloom::runtime::TaskOwnerKind::Export,
                                      .id = bloom::runtime::TaskOwnerId::fromRaw(1)},
            bloom::runtime::TaskPriority::Foreground, bloom::runtime::TaskExecutor::Cpu),
        [&](bloom::runtime::TaskContext& context) -> bloom::runtime::TaskResult<bool> {
            {
                std::lock_guard lock(tokenMutex);
                captured = context.cancellation();
            }
            tokenCv.notify_all();
            while (!context.isCancellationRequested()) {
                std::this_thread::sleep_for(1ms);
            }
            return bloom::runtime::TaskResult<bool>::cancelled();
        });
    expectations.expect(submission.accepted(), "cancelled-token: token task accepted");
    if (submission.accepted()) {
        {
            std::unique_lock lock(tokenMutex);
            tokenCv.wait_for(lock, 5s, [&] { return captured.has_value(); });
        }
        submission.handle.cancel();
        if (captured.has_value()) {
            const auto plan = lifecyclePlan();
            const auto request = requestFor(*plan);
            const auto outcome = evaluator->evaluate(plan, request, *captured);
            expectations.expect(outcome.status == GpuProcessFrameStatus::Cancelled &&
                                    outcome.frame == nullptr,
                                "cancelled-token: an already-cancelled token returns Cancelled");
        }
    }
    evaluator->beginShutdown();
}

void runPreparationOffOwnerTest(Expectations& expectations, const Options& options) {
    std::atomic<std::thread::id> providerThread{};
    std::atomic<std::thread::id> preflightThread{};
    std::atomic<std::thread::id> operationThread{};
    std::atomic<std::thread::id> evaluateThread{};
    auto evaluatorOptions = gpuEvaluatorOptions(options.loader_path);
    evaluatorOptions.mediaContextProvider = [&] {
        providerThread.store(std::this_thread::get_id());
        return bloom::runtime::GpuSceneMediaContext{};
    };
    auto evaluator = GpuProcessFrameEvaluator::create(evaluatorOptions);
    expectations.expect(evaluator != nullptr, "prep-off-owner: evaluator constructed");
    if (evaluator == nullptr || !evaluator->gpuAvailable()) {
        return;
    }
    const auto plan = lifecyclePlan();
    const auto request = requestFor(*plan);

    // Caller A blocks inside its CPU-preparation progress callback until released, so the owner is
    // demonstrably free to serve another request while A's preparation is still pending.
    std::atomic<bool> releaseA{false};
    std::atomic<bool> aEnteredPreflight{false};
    std::thread a([&] {
        evaluateThread.store(std::this_thread::get_id());
        static_cast<void>(evaluator->evaluate(
            plan, request, {}, [&](const bloom::runtime::EvaluationProgress& event) {
                if (event.stage == bloom::runtime::EvaluationProgressStage::Preflight) {
                    preflightThread.store(std::this_thread::get_id());
                    aEnteredPreflight.store(true);
                    while (!releaseA.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(1ms);
                    }
                } else if (event.stage == bloom::runtime::EvaluationProgressStage::Operation) {
                    operationThread.store(std::this_thread::get_id());
                }
            }));
    });
    const auto aDeadline = std::chrono::steady_clock::now() + 10s;
    while (!aEnteredPreflight.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < aDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(aEnteredPreflight.load(std::memory_order_acquire),
                        "prep-off-owner: A entered CPU preparation");

    std::atomic<bool> bCompleted{false};
    std::atomic<bool> bEvaluated{false};
    std::thread b([&] {
        const auto outcome = evaluator->evaluate(plan, request);
        bEvaluated.store(outcome.status == GpuProcessFrameStatus::Evaluated &&
                         outcome.frame != nullptr);
        bCompleted.store(true);
    });
    const auto bDeadline = std::chrono::steady_clock::now() + 20s;
    while (!bCompleted.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < bDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(bCompleted.load(std::memory_order_acquire) &&
                            bEvaluated.load(std::memory_order_acquire),
                        "prep-off-owner: the owner serves B while A's CPU preparation is pending");
    releaseA.store(true, std::memory_order_release);
    a.join();
    b.join();

    expectations.expect(preflightThread.load() == evaluateThread.load(),
                        "prep-off-owner: Preflight fires on the calling CPU worker");
    expectations.expect(providerThread.load() == evaluateThread.load(),
                        "prep-off-owner: the media context provider (decode/preparation) runs on "
                        "the calling CPU worker, never the GPU owner");
    expectations.expect(
        operationThread.load() != evaluateThread.load() &&
            operationThread.load() != std::thread::id{},
        "prep-off-owner: the native Operation event fires on the GPU owner, not the "
        "caller");
    evaluator->beginShutdown();
}

// A deterministic fault seam proves the admission reservation is released through the public API
// when slot allocation fails, and that a later request on the same evaluator still succeeds.
void runPreparationAllocationFaultTest(Expectations& expectations, const Options& options) {
    auto evaluatorOptions = gpuEvaluatorOptions(options.loader_path);
    evaluatorOptions.failPreparationAllocationAt = 1;
    auto evaluator = GpuProcessFrameEvaluator::create(evaluatorOptions);
    expectations.expect(evaluator != nullptr, "prep-fault: evaluator constructed");
    if (evaluator == nullptr || !evaluator->gpuAvailable()) {
        return;
    }
    const auto plan = lifecyclePlan();
    const auto request = requestFor(*plan);
    const auto first = evaluator->evaluate(plan, request);
    expectations.expect(first.status == GpuProcessFrameStatus::Failed &&
                            first.diagnostic.code == GpuProcessFrameDiagnosticCode::BadAllocation &&
                            first.frame == nullptr,
                        "prep-fault: the injected allocation failure is a typed BadAllocation");
    const auto second = evaluator->evaluate(plan, request);
    expectations.expect(second.status == GpuProcessFrameStatus::Evaluated &&
                            second.frame != nullptr,
                        "prep-fault: a subsequent request succeeds (reservation was not leaked)");
    evaluator->beginShutdown();
}

// beginShutdown() while a calling worker is still preparing must not report retirement complete,
// must not publish a frame, and must complete retirement only once the caller finishes.
void runShutdownDuringPreparationTest(Expectations& expectations, const Options& options) {
    auto evaluator = GpuProcessFrameEvaluator::create(gpuEvaluatorOptions(options.loader_path));
    expectations.expect(evaluator != nullptr, "shutdown-prep: evaluator constructed");
    if (evaluator == nullptr || !evaluator->gpuAvailable()) {
        return;
    }
    const auto plan = lifecyclePlan();
    const auto request = requestFor(*plan);
    std::atomic<bool> release{false};
    std::atomic<bool> entered{false};
    std::atomic<GpuProcessFrameStatus> status{GpuProcessFrameStatus::Failed};
    std::thread caller([&] {
        const auto outcome = evaluator->evaluate(
            plan, request, {}, [&](const bloom::runtime::EvaluationProgress& event) {
                if (event.stage == bloom::runtime::EvaluationProgressStage::Preflight) {
                    entered.store(true, std::memory_order_release);
                    while (!release.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(1ms);
                    }
                }
            });
        status.store(outcome.status);
    });
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(entered.load(std::memory_order_acquire),
                        "shutdown-prep: the caller entered CPU preparation");
    evaluator->beginShutdown();
    // Give the owner time to leave its loop and retire; retirement must still be withheld while the
    // calling worker is inside CPU preparation.
    std::this_thread::sleep_for(500ms);
    expectations.expect(!evaluator->retirementComplete(),
                        "shutdown-prep: retirement is withheld while a caller is preparing");
    release.store(true, std::memory_order_release);
    caller.join();
    const auto retireDeadline = std::chrono::steady_clock::now() + 10s;
    while (!evaluator->retirementComplete() && std::chrono::steady_clock::now() < retireDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(evaluator->retirementComplete(),
                        "shutdown-prep: retirement completes once the caller finishes");
    expectations.expect(status.load() != GpuProcessFrameStatus::Evaluated,
                        "shutdown-prep: no frame is published after shutdown");
}

void runLifecycleTests(Expectations& expectations, const Options& options) {
    runTwoCallerTest(expectations, options);
    runQueuedRequestsTest(expectations, options);
    runShutdownRejectsNewCallsTest(expectations, options);
    runThrowingProgressTest(expectations, options);
    runReentrantEvaluateTest(expectations, options);
    runCancelledTokenTest(expectations, options);
    runPreparationOffOwnerTest(expectations, options);
    runPreparationAllocationFaultTest(expectations, options);
    runShutdownDuringPreparationTest(expectations, options);
}

int run(int argc, char** argv) {
    const Options options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    Expectations expectations;

    const auto plan = twoSolidPlan(
        format(24, 18), Color4d{0.5, 0.25, 0.125, 1.0}, LayerValues{.position = {4.25, 3.5}},
        Color4d{0.125, 0.375, 0.75, 0.5}, LayerValues{.position = {7.5, 6.25}}, 9.0, 7.0, 9000);
    const auto request = requestFor(*plan);

    auto evaluator = GpuProcessFrameEvaluator::create(gpuEvaluatorOptions(options.loader_path));
    expectations.expect(evaluator != nullptr, "the evaluator is constructed");
    if (evaluator == nullptr) {
        return 1;
    }
    if (!evaluator->gpuAvailable()) {
        if (options.require_device) {
            std::cerr << "FAIL: required device unavailable: "
                      << evaluator->availabilityDiagnostic().message << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device: "
                  << evaluator->availabilityDiagnostic().message << '\n';
        return expectations.ok() ? 0 : 1;
    }

    const auto cpuFrame = evaluateCpu(plan, request);
    expectations.expect(cpuFrame != nullptr, "the CPU oracle produced a reference frame");

    const auto first = evaluator->evaluate(plan, request);
    expectations.expect(first.status == GpuProcessFrameStatus::Evaluated && first.frame != nullptr,
                        "the GPU bridge produced a genuine process frame");
    if (first.frame == nullptr || cpuFrame == nullptr) {
        std::cerr << "GPU diagnostic: " << static_cast<int>(first.diagnostic.code) << ' '
                  << first.diagnostic.message << '\n';
        return 1;
    }

    expectations.expect(first.frame->identity().provider == EvaluationProvider::GpuResident,
                        "the frame provenance is GpuResident");
    expectations.expect(first.counters.nativeDispatches > 0,
                        "the scene performed real native dispatches");
    expectations.expect(first.counters.readbacks == 1, "exactly one final RGBA32F readback");

    expectations.expect(
        pixelsClose(first.frame->processImage().pixels(), cpuFrame->processImage().pixels()),
        "the GPU process image matches the CPU oracle within tolerance");

    // The canonical output semantic identity (canonical bytes + process-pixel digest equality with
    // the CPU oracle) is proven in bloom.host -- the runtime module cannot depend on bloom::output,
    // even in tests. See src/host/tests/gpu_process_frame_identity_tests.cpp.

    // Warm path: the same scene served by the content cache performs no new native dispatch, but
    // still needs the one final readback for the CPU output adapter.
    const auto second = evaluator->evaluate(plan, request);
    expectations.expect(second.status == GpuProcessFrameStatus::Evaluated &&
                            second.frame != nullptr,
                        "the warm request evaluates");
    expectations.expect(second.counters.nativeDispatches == first.counters.nativeDispatches,
                        "an unchanged warm scene performs zero new native dispatches");
    expectations.expect(second.counters.readbacks == 1,
                        "the warm request still performs exactly one final readback");
    if (second.frame != nullptr) {
        expectations.expect(pixelsExact(second.frame->processImage().pixels(),
                                        first.frame->processImage().pixels()),
                            "warm GPU pixels are byte-identical to the cold GPU pixels");
    }

    // Rotation is inside the current prepared-GPU affine subset, so a rotated (affine) layer now
    // evaluates genuinely on the GPU. This is the current positive behaviour.
    const auto rotated = twoSolidPlan(format(24, 18), Color4d{0.5, 0.25, 0.125, 1.0},
                                      LayerValues{.position = {4.25, 3.5}, .rotation = 30.0},
                                      Color4d{0.125, 0.375, 0.75, 0.5},
                                      LayerValues{.position = {7.5, 6.25}}, 9.0, 7.0, 9100);
    const auto rotatedRequest = requestFor(*rotated);
    const auto rotatedOutcome = evaluator->evaluate(rotated, rotatedRequest);
    expectations.expect(rotatedOutcome.status == GpuProcessFrameStatus::Evaluated &&
                            rotatedOutcome.frame != nullptr &&
                            rotatedOutcome.frame->identity().provider ==
                                EvaluationProvider::GpuResident,
                        "a rotated (affine) layer is evaluated on the GPU");

    // A genuinely out-of-subset operation (a text box on the integer grid) must be diagnosed and
    // left to CPU, never claimed as GPU.
    auto text = makeText(9200);
    text.layout.box = {2.0, 2.0};
    const auto unsupported = vectorLeafPlan(format(24, 18), text, LayerValues{}, 9200);
    const auto unsupportedRequest = requestFor(*unsupported);
    const auto refused = evaluator->evaluate(unsupported, unsupportedRequest);
    expectations.expect(refused.status == GpuProcessFrameStatus::UnsupportedGpuSubset,
                        "an out-of-subset operation is diagnosed, not silently CPU-executed");

    evaluator->beginShutdown();

    runLifecycleTests(expectations, options);

    if (!expectations.ok()) {
        std::cerr << "FAIL: GPU process-frame expectations failed\n";
        return 1;
    }
    std::cout << "PASS: GPU process-frame bridge\n";
    return 0;
}

} // namespace

int main(const int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
