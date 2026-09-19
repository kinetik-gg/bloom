// Real-device retirement contracts for the media upload step of the GPU scene executor.
//
// Cancellation before and during a real submitted upload, and the deadline / unknown-fence faults,
// must fail closed with no published image and retain every pin until the fence is PROVEN retired
// (only VK_SUCCESS may clear a submission). These vectors use the proof-only fault macro and are
// inert in a production build.

#include "gpu_media_scene_preparation_test_support.hpp"

#include "gpu_media_executor_test_support.hpp"

#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
#include "gpu_scene_executor_fault_injection.hpp"
#endif

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <thread>
#include <utility>

namespace {

using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorBudgets;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneExecutorPollResult;
using bloom::runtime::GpuSceneMediaContext;

using bloom::runtime::media_executor_test::drainToTerminal;
using bloom::runtime::media_executor_test::kCacheBudget;
using bloom::runtime::media_executor_test::kSceneBudget;
using bloom::runtime::media_executor_test::Options;
using bloom::runtime::media_executor_test::parseOptions;
using bloom::runtime::media_executor_test::runScene;

struct MediaFixture final {
    std::filesystem::path directory;
    std::filesystem::path path;
    bloom::document::AssetRecord asset;
};

[[nodiscard]] MediaFixture makeFixture() {
    MediaFixture fixture;
    fixture.directory =
        std::filesystem::temp_directory_path() / "bloom_gpu_media_executor_retirement_test";
    std::filesystem::remove_all(fixture.directory);
    std::filesystem::create_directories(fixture.directory);
    fixture.path = fixture.directory / "signed_hdr.exr";
    writeExrRgba(fixture.path, 3, 2, signedHdrPixels());
    fixture.asset = imageAsset(fixture.path, "signed_hdr", 900);
    return fixture;
}

// Cancellation before a native job and during a stalled real upload job fails closed, retains until
// the fence is proven retired, and leaves the executor reusable.
void testCancellation(Expectations& expectations, GpuDevice& device,
                      const CpuCompositionEvaluator& evaluator, const MediaFixture& fixture) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(cache.hasValue() && executor.hasValue(), "cancel: cache+executor");
    if (!cache || !executor) {
        return;
    }
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan =
        mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {4.3, 3.1}}, 2200);

    // Cancel immediately after begin, before the first dispatch.
    const auto preparedA = builder.build(plan, requestFor(*plan));
    expectations.expect(preparedA.hasValue(), "cancel: the first scene prepares");
    if (!preparedA) {
        return;
    }
    expectations.expect(executor.executor->begin(preparedA.scene, kSceneBudget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        "cancel: begin accepted");
    executor.executor->cancel();
    const auto cancelled = drainToTerminal(*executor.executor);
    expectations.expect(cancelled == GpuSceneExecutorPollResult::Failure &&
                            executor.executor->diagnostic().code ==
                                GpuSceneExecutorDiagnosticCode::Cancelled,
                        "cancel: a pre-dispatch cancel fails closed as Cancelled");
    expectations.expect(executor.executor->image() == nullptr &&
                            executor.executor->takeImage() == nullptr,
                        "cancel: no image is published after a cancel");
    expectations.expect(!executor.executor->ownerDrainRequired(),
                        "cancel: a pre-dispatch cancel needs no owner drain");

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    // Cancel during a real submitted upload whose poll is stalled: the fence is only released after
    // the stall is cleared and the real fence proves retirement.
    const auto preparedB = builder.build(plan, requestFor(*plan));
    expectations.expect(preparedB.hasValue(), "cancel: the stalled scene prepares");
    if (!preparedB) {
        return;
    }
    expectations.expect(executor.executor->begin(preparedB.scene, kSceneBudget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        "cancel: stalled begin accepted");
    bloom::render::gpu_scene_executor_fault::clear();
    // First poll dispatches the upload; then stall its next poll.
    expectations.expect(executor.executor->poll() == GpuSceneExecutorPollResult::Pending,
                        "cancel: the upload dispatch started");
    bloom::render::gpu_scene_executor_fault::set(
        bloom::render::gpu_scene_executor_fault::PollFault::StallPending);
    expectations.expect(executor.executor->poll() == GpuSceneExecutorPollResult::Pending,
                        "cancel: the stalled upload still polls Pending");
    executor.executor->cancel();
    expectations.expect(executor.executor->poll() == GpuSceneExecutorPollResult::Pending,
                        "cancel: the stalled job is retained across the cancel request");
    bloom::render::gpu_scene_executor_fault::clear();
    const auto cancelledMid = drainToTerminal(*executor.executor);
    expectations.expect(cancelledMid == GpuSceneExecutorPollResult::Failure &&
                            executor.executor->diagnostic().code ==
                                GpuSceneExecutorDiagnosticCode::Cancelled,
                        "cancel: a mid-job cancel fails closed after a proven retirement");
    expectations.expect(!executor.executor->ownerDrainRequired(),
                        "cancel: a proven retirement needs no owner drain");
#else
    std::cout << "NOTE: upload fault-injection cancel vector skipped (macro not defined)\n";
#endif
}

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
// The deadline and unknown-fence faults prove retention + owner drain for a real submitted upload.
void testUploadRetirement(Expectations& expectations, GpuDevice& device,
                          const CpuCompositionEvaluator& evaluator, const MediaFixture& fixture) {
    using Fault = bloom::render::gpu_scene_executor_fault::PollFault;
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "retire: cache created");
    if (!cache) {
        return;
    }
    GpuSceneExecutorBudgets budgets;
    budgets.jobDeadlineMilliseconds = 5;
    auto executor = GpuSceneExecutor::create(device, *cache.cache, budgets);
    expectations.expect(executor.hasValue(), "retire: executor created");
    if (!executor) {
        return;
    }
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan =
        mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {4.3, 3.1}}, 2300);
    const auto prepared = builder.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "retire: scene prepares");
    if (!prepared) {
        return;
    }

    // Stall the upload poll past the deadline: NativeTimeout, owner drain required, no image.
    bloom::render::gpu_scene_executor_fault::set(Fault::StallPending);
    expectations.expect(executor.executor->begin(prepared.scene, kSceneBudget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        "retire: begin accepted");
    expectations.expect(executor.executor->poll() == GpuSceneExecutorPollResult::Pending,
                        "retire: the upload dispatch started");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    GpuSceneExecutorPollResult timeout = GpuSceneExecutorPollResult::Pending;
    while (std::chrono::steady_clock::now() < deadline) {
        timeout = executor.executor->poll();
        if (timeout == GpuSceneExecutorPollResult::Failure) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    expectations.expect(timeout == GpuSceneExecutorPollResult::Failure &&
                            executor.executor->diagnostic().code ==
                                GpuSceneExecutorDiagnosticCode::NativeTimeout,
                        "retire: the stalled upload fails as NativeTimeout");
    expectations.expect(executor.executor->ownerDrainRequired(),
                        "retire: the unproven upload requires owner drain");
    expectations.expect(executor.executor->image() == nullptr &&
                            executor.executor->takeImage() == nullptr,
                        "retire: no image is published on an unproven failure");
    expectations.expect(executor.executor->begin(prepared.scene, kSceneBudget).code ==
                            GpuSceneExecutorDiagnosticCode::OwnerDrainRequired,
                        "retire: reuse is refused while drain is required");
    bloom::render::gpu_scene_executor_fault::clear();
    bool drained = false;
    const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < drainDeadline) {
        static_cast<void>(executor.executor->poll());
        if (!executor.executor->ownerDrainRequired()) {
            drained = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectations.expect(drained, "retire: the real fence retires and the drain completes");
    const auto reused = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(reused.ready, "retire: the executor is reusable after the drain");

    // Unknown fence: the submission is not proven retired, so the request fails NativeUnproven and
    // retains every pin until a later owner poll proves retirement.
    const auto secondPlan =
        mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {2.7, 2.4}}, 2400);
    const auto second = builder.build(secondPlan, requestFor(*secondPlan));
    expectations.expect(second.hasValue(), "retire: the second scene prepares");
    if (!second) {
        return;
    }
    bloom::render::gpu_scene_executor_fault::set(Fault::UnknownFence);
    expectations.expect(executor.executor->begin(second.scene, kSceneBudget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        "retire: second begin accepted");
    const auto unproven = drainToTerminal(*executor.executor);
    expectations.expect(unproven == GpuSceneExecutorPollResult::Failure &&
                            executor.executor->diagnostic().code ==
                                GpuSceneExecutorDiagnosticCode::NativeUnproven,
                        "retire: an unknown upload fence fails NativeUnproven");
    expectations.expect(executor.executor->ownerDrainRequired(),
                        "retire: the unproven upload retains its pins for owner drain");
    bloom::render::gpu_scene_executor_fault::clear();
    drained = false;
    const auto secondDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < secondDeadline) {
        static_cast<void>(executor.executor->poll());
        if (!executor.executor->ownerDrainRequired()) {
            drained = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectations.expect(drained, "retire: the unknown-fence submission drains on the owner thread");
    const auto reusedAgain = runScene(*executor.executor, second.scene, kSceneBudget);
    expectations.expect(reusedAgain.ready, "retire: the executor is reusable after the drain");
}
#endif

} // namespace

int main(const int argc, char** argv) {
    const Options options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    Expectations expectations;
    const CpuCompositionEvaluator evaluator;
    const auto fixture = makeFixture();
    evaluator.setAssetBaseDirectory(fixture.directory);

    GpuDeviceCreationOptions createOptions;
    createOptions.loader_path = options.loader_path;
    auto device = GpuDevice::create(createOptions);
    if (!device) {
        if (options.require_device) {
            std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device available: " << device.diagnostic.message
                  << '\n';
        return expectations.ok() ? 0 : 1;
    }

    testCancellation(expectations, *device.device, evaluator, fixture);
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    testUploadRetirement(expectations, *device.device, evaluator, fixture);
#endif

    if (!expectations.ok()) {
        std::cerr << "FAIL: GPU media executor retirement expectations failed\n";
        return 1;
    }
    std::cout << "PASS: GPU media executor retirement\n";
    return 0;
}
