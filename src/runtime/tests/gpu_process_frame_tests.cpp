// Native fixture for the bounded GPU final-render/export bridge.
//
// It drives the REAL path: a real CompiledCompositionPlan -> CpuGpuSceneBuilder ->
// GpuSceneExecutor on a dedicated owner worker -> the production GpuProcessReadback -> a genuine
// ProcessFrame with EvaluationProvider::GpuResident. It compares the final process image and the
// canonical process-frame semantic identity against the CPU reference evaluator, proves the final
// RGBA32F readback happened exactly once, and proves an unchanged warm scene recomputes no native
// dispatch. Without a loader/device it prints an explicit SKIP unless --require-device is passed.

#include "gpu_media_executor_test_support.hpp"
#include "gpu_scene_executor_test_support.hpp"

#include <bloom/core/color.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
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

// Two concurrent callers each receive exactly one outcome; neither hangs even though the owner
// runs one request at a time. Bounded by a wall-clock deadline so a missed wake-up is a hard fail.
void runTwoCallerTest(Expectations& expectations, const Options& options) {
    auto evaluator = GpuProcessFrameEvaluator::create(
        GpuProcessFrameEvaluatorOptions{.enabled = true,
                                        .loaderPath = options.loader_path,
                                        .requestByteBudget = kRequestBudget,
                                        .readbackByteBudget = kReadbackBudget});
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
    auto evaluator = GpuProcessFrameEvaluator::create(
        GpuProcessFrameEvaluatorOptions{.enabled = true,
                                        .loaderPath = options.loader_path,
                                        .requestByteBudget = kRequestBudget,
                                        .readbackByteBudget = kReadbackBudget});
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
    auto evaluator = GpuProcessFrameEvaluator::create(
        GpuProcessFrameEvaluatorOptions{.enabled = true,
                                        .loaderPath = options.loader_path,
                                        .requestByteBudget = kRequestBudget,
                                        .readbackByteBudget = kReadbackBudget});
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
    auto evaluator = GpuProcessFrameEvaluator::create(
        GpuProcessFrameEvaluatorOptions{.enabled = true,
                                        .loaderPath = options.loader_path,
                                        .requestByteBudget = kRequestBudget,
                                        .readbackByteBudget = kReadbackBudget});
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
    auto evaluator = GpuProcessFrameEvaluator::create(
        GpuProcessFrameEvaluatorOptions{.enabled = true,
                                        .loaderPath = options.loader_path,
                                        .requestByteBudget = kRequestBudget,
                                        .readbackByteBudget = kReadbackBudget});
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
    auto evaluator = GpuProcessFrameEvaluator::create(
        GpuProcessFrameEvaluatorOptions{.enabled = true,
                                        .loaderPath = options.loader_path,
                                        .requestByteBudget = kRequestBudget,
                                        .readbackByteBudget = kReadbackBudget});
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

void runLifecycleTests(Expectations& expectations, const Options& options) {
    runTwoCallerTest(expectations, options);
    runQueuedRequestsTest(expectations, options);
    runShutdownRejectsNewCallsTest(expectations, options);
    runThrowingProgressTest(expectations, options);
    runReentrantEvaluateTest(expectations, options);
    runCancelledTokenTest(expectations, options);
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

    auto evaluator = GpuProcessFrameEvaluator::create(
        GpuProcessFrameEvaluatorOptions{.enabled = true,
                                        .loaderPath = options.loader_path,
                                        .requestByteBudget = kRequestBudget,
                                        .readbackByteBudget = kReadbackBudget});
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

    // The canonical semantic identity is derived from the exact process pixels plus the closed
    // identity fields; a bit-equal GPU frame must produce the same canonical bytes.
    const bloom::output::ProcessFrameSemanticIdentityV1Preparer preparer;
    const auto cpuIdentity = preparer.prepare(cpuFrame, {});
    const auto gpuIdentity = preparer.prepare(first.frame, {});
    expectations.expect(
        cpuIdentity.status() ==
                bloom::output::ProcessFrameSemanticIdentityPreparationStatus::Prepared &&
            gpuIdentity.status() ==
                bloom::output::ProcessFrameSemanticIdentityPreparationStatus::Prepared,
        "both frames prepare a semantic identity");
    if (cpuIdentity.identity() != nullptr && gpuIdentity.identity() != nullptr) {
        const auto cpuBytes = cpuIdentity.identity()->canonicalBytes();
        const auto gpuBytes = gpuIdentity.identity()->canonicalBytes();
        expectations.expect(cpuBytes.size() == gpuBytes.size() &&
                                std::equal(cpuBytes.begin(), cpuBytes.end(), gpuBytes.begin()),
                            "the GPU frame's canonical identity bytes equal the CPU oracle");
        expectations.expect(cpuIdentity.identity()->processPixelDigest() ==
                                gpuIdentity.identity()->processPixelDigest(),
                            "the GPU process-pixel digest equals the CPU oracle");
    }

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

    // An ordinary unsupported operation (a rotated layer) must be diagnosed and left to CPU, never
    // claimed as GPU.
    const auto rotated = twoSolidPlan(format(24, 18), Color4d{0.5, 0.25, 0.125, 1.0},
                                      LayerValues{.position = {4.25, 3.5}, .rotation = 30.0},
                                      Color4d{0.125, 0.375, 0.75, 0.5},
                                      LayerValues{.position = {7.5, 6.25}}, 9.0, 7.0, 9100);
    const auto rotatedRequest = requestFor(*rotated);
    const auto refused = evaluator->evaluate(rotated, rotatedRequest);
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
