// #364 real native proof for Cryptomatte.
//
// A real multi-rank Cryptomatte OpenEXR (two CryptoObject rank layers, with and without the
// manifest) is resolved through the production media pipeline. The production CpuGpuSceneBuilder
// gathers the rank pairs into one stacked upload and emits a CryptomatteCoverageV1 command (plus a
// PointResampleV1 under a proxy); the production GpuSceneExecutor runs it on a real device and the
// result is compared BIT FOR BIT with the unchanged CpuCompositionEvaluator oracle.
//
// It also proves the cache identity (an edited selection keeps the rank upload and re-dispatches
// only the coverage pass; an unchanged selection is a warm hit), cancellation and the pixel budget.
//
// Without a device it SKIPs (exit 77) unless --require-device is passed.

#include "cryptomatte_fixture_support.hpp"
#include "gpu_media_cache_test_support.hpp"
#include "gpu_media_executor_test_support.hpp"
#include "gpu_media_scene_preparation_test_support.hpp"

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/cryptomatte_matte_list.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/task_types.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::readbackResidentImage;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneCryptomatteCommand;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuScenePointResampleCommand;
using bloom::runtime::GpuSceneUploadCommand;
using bloom::runtime::PreparedGpuScene;
using bloom::runtime::PreparedGpuSceneDiagnosticCode;
using bloom::runtime::media_executor_test::kSceneBudget;
using bloom::runtime::media_executor_test::Options;
using bloom::runtime::media_executor_test::parseOptions;
using bloom::runtime::media_executor_test::pixelsClose;
using bloom::runtime::media_executor_test::pixelsExact;
using bloom::runtime::media_executor_test::runScene;

// A genuinely cancelled token, taken from a scheduler task that is cancelled while it runs.
[[nodiscard]] bloom::runtime::CancellationToken makeCancelledToken() {
    using namespace bloom::runtime;
    TaskSchedulerConfig config = TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    CancellationToken token;
    auto submission = scheduler.submit<void>(
        TaskRequest("gpu cryptomatte cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(98)}),
        [&](TaskContext& context) {
            {
                std::lock_guard lock(mutex);
                token = context.cancellation();
                entered = true;
            }
            condition.notify_all();
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] { return release; });
            return TaskResult<void>::succeeded();
        });
    if (!submission.accepted())
        throw std::logic_error("cancel token task was refused");
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    submission.handle.cancel();
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!submission.handle.tryTakeResult().has_value() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return token;
}

template <typename Command> [[nodiscard]] const Command* first(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands())
        if (const auto* found = std::get_if<Command>(&command))
            return found;
    return nullptr;
}

// Source -> Cryptomatte -> Layer Output at an integral placement, so the composited frame carries
// the matte's exact bits. With `imageOutput` the layer reads beauty x matte (MultiplyAlpha).
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
cryptomattePlan(const bloom::document::AssetRecord& asset, std::vector<std::string> mattes,
                const bool imageOutput, const std::uint64_t idBase) {
    using bloom::document::NodeId;
    using bloom::document::ParameterId;
    const LayerIds ids{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                       ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                       ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    bloom::runtime::CompiledCryptomatte cryptomatte;
    cryptomatte.sourceNodeId = NodeId::fromRaw(idBase + 20);
    cryptomatte.channels.sources.push_back(bloom::runtime::CompiledChannelSource{
        NodeId::fromRaw(idBase + 10), asset, 0, 0, 0, {}, {}});
    bloom::runtime::CompiledChannelStep source;
    source.kind = bloom::runtime::CompiledChannelStepKind::Source;
    source.nodeId = NodeId::fromRaw(idBase + 10);
    cryptomatte.channels.steps.push_back(source);
    cryptomatte.layer = "CryptoObject";
    cryptomatte.mattes = std::move(mattes);
    std::vector<CompiledOperation> operations;
    operations.emplace_back(std::move(cryptomatte));
    auto layerInput = OperationIndex::fromRaw(0);
    if (imageOutput) {
        operations.emplace_back(bloom::runtime::CompiledImageSource{
            NodeId::fromRaw(idBase + 10), asset, 0, 0, 0, std::string{}, false});
        operations.emplace_back(bloom::runtime::CompiledImageEffect{
            NodeId::fromRaw(idBase + 20), OperationIndex::fromRaw(1),
            bloom::runtime::ImageCombineKernel{
                bloom::runtime::ImageCombineMode::MultiplyAlpha,
                bloom::runtime::CompiledScalarParameter{ParameterId{}, 1.0},
                bloom::render::ImageCoverageChannel::Alpha, std::nullopt, false},
            false, false, OperationIndex::fromRaw(0)});
        layerInput = OperationIndex::fromRaw(2);
    }
    const auto layerIndex = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 11),
                                        bloom::document::LayerId::fromRaw(idBase + 12), layerInput,
                                        ids, LayerValues{.position = {0.0, 0.0}}));
    const auto mergeIndex = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{
        NodeId::fromRaw(idBase + 13),
        std::vector<CompiledMergeInput>{
            CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(idBase + 14),
                               bloom::document::LayerId::fromRaw(idBase + 12), layerIndex}}});
    const auto outputIndex = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 15), mergeIndex});
    return publish(CompiledCompositionPlanDefinition{bloom::document::Revision::fromRaw(7),
                                                     kProjectId, kCompositionId, format(8, 8),
                                                     std::move(operations), outputIndex});
}

[[nodiscard]] bloom::runtime::EvaluationRequest request(const CompiledCompositionPlan& plan,
                                                        const bool proxy) {
    auto value = requestFor(plan);
    if (proxy) {
        const auto extent = bloom::render::ImageExtent::create(4, 4);
        value.resolution = bloom::runtime::ProxyResolution{*extent.value()};
    }
    return value;
}

struct Harness final {
    GpuDevice& device;
    CpuCompositionEvaluator& evaluator;
    std::filesystem::path directory;
};

// One plan prepared by the production builder, run natively cold then warm, and compared with the
// CPU oracle: bit for bit for the Mask output, 2e-6 for the Image output's MultiplyAlpha combine.
void testParity(Expectations& expectations, Harness& harness,
                const bloom::document::AssetRecord& asset, std::vector<std::string> mattes,
                const bool imageOutput, const bool proxy, const std::uint64_t idBase,
                const std::string& label) {
    auto context = GpuSceneMediaContext::fromEvaluator(harness.evaluator);
    context.assetBaseDirectory = harness.directory;
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = cryptomattePlan(asset, std::move(mattes), imageOutput, idBase);
    const auto evaluation = request(*plan, proxy);
    const auto prepared = builder.build(plan, evaluation);
    expectations.expect(prepared.hasValue(), label + ": prepares");
    if (!prepared) {
        std::cerr << label << " prepare diagnostic: " << prepared.diagnostic.message << "\n";
        return;
    }
    const auto* upload = first<GpuSceneUploadCommand>(*prepared.scene);
    const auto* coverage = first<GpuSceneCryptomatteCommand>(*prepared.scene);
    expectations.expect(upload != nullptr && coverage != nullptr && coverage->rankCount == 4 &&
                            upload->descriptor.dataWindow().extent().height() ==
                                coverage->outputWindow.extent().height() * coverage->rankCount,
                        label + ": emits the stacked four-rank upload and a CryptomatteCoverageV1");
    expectations.expect((first<GpuScenePointResampleCommand>(*prepared.scene) != nullptr) == proxy,
                        label + ": a proxy adds the PointResampleV1 after the coverage pass");

    auto oracleRequest = evaluation;
    oracleRequest.bypassOperationCache = true;
    const auto frame = harness.evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, label + ": CPU oracle evaluates");
    if (!frame.frame())
        return;
    auto cache = GpuSceneCache::create(harness.device, GpuSceneCacheBudgets{});
    auto executor = cache ? GpuSceneExecutor::create(harness.device, *cache.cache)
                          : decltype(GpuSceneExecutor::create(harness.device, *cache.cache)){};
    expectations.expect(cache.hasValue() && executor.hasValue(), label + ": executor creates");
    if (!cache || !executor)
        return;
    const auto cold = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(cold.ready, label + ": cold run completes");
    if (!cold.ready)
        return;
    expectations.expect(cold.countersAtReady.cryptomatteDispatches == 1,
                        label + ": exactly one native CryptomatteCoverageV1 dispatch ran");
    expectations.expect(cold.countersAtReady.readbacks == 0,
                        label + ": the executor performs no full-frame readback");
    const auto readback = readbackResidentImage(*cold.image, kSceneBudget);
    const auto oracle = frame.frame()->processImage().pixels();
    if (imageOutput) {
        expectations.expect(readback.hasValue() && pixelsClose(readback.pixels, oracle),
                            label + ": native Image output matches the CPU oracle (2e-6)");
    } else {
        expectations.expect(readback.hasValue() && pixelsExact(readback.pixels, oracle),
                            label + ": native matte is bit-identical to the CPU oracle");
    }
    const auto before = executor.executor->counters();
    const auto warm = runScene(*executor.executor, prepared.scene, kSceneBudget);
    const auto after = executor.executor->counters();
    expectations.expect(warm.ready && after.dispatches == before.dispatches,
                        label + ": the warm rerun dispatches nothing");
}

// A selection edit over one prepared-upload cache and one executor cache keeps the rank upload
// (same identity, served from the upload cache with the manifest-resolved IDs kept beside it) and
// re-dispatches the coverage pass; the IDs from that cache hit still give the exact matte.
void testSelectionEdit(Expectations& expectations, Harness& harness,
                       const bloom::document::AssetRecord& asset) {
    auto context = legacyPreparedMediaContext(harness.evaluator);
    context.assetBaseDirectory = harness.directory;
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto cube = cryptomattePlan(asset, {"cube"}, false, 96000);
    const auto floor = cryptomattePlan(asset, {"floor", "sky"}, false, 96000);
    const auto first_ = builder.build(cube, request(*cube, false));
    const auto second = builder.build(floor, request(*floor, false));
    const auto again = builder.build(cube, request(*cube, false));
    expectations.expect(first_.hasValue() && second.hasValue() && again.hasValue(),
                        "selection edit: every build prepares");
    if (!first_ || !second || !again)
        return;
    const auto* firstUpload = first<GpuSceneUploadCommand>(*first_.scene);
    const auto* secondUpload = first<GpuSceneUploadCommand>(*second.scene);
    const auto* firstCoverage = first<GpuSceneCryptomatteCommand>(*first_.scene);
    const auto* secondCoverage = first<GpuSceneCryptomatteCommand>(*second.scene);
    const auto* againCoverage = first<GpuSceneCryptomatteCommand>(*again.scene);
    expectations.expect(firstUpload && secondUpload &&
                            firstUpload->semanticKey == secondUpload->semanticKey,
                        "selection edit: the rank upload identity ignores the selection");
    expectations.expect(firstCoverage && secondCoverage && againCoverage &&
                            firstCoverage->semanticKey != secondCoverage->semanticKey &&
                            firstCoverage->semanticKey == againCoverage->semanticKey,
                        "selection edit: the coverage identity carries the selected IDs");
    expectations.expect(second.scene->mediaStatistics().uploadCacheMisses == 1 &&
                            again.scene->mediaStatistics().uploadCacheHits == 1,
                        "selection edit: a new selection resolves its IDs once; a repeat is a hit");

    auto cache = GpuSceneCache::create(harness.device, GpuSceneCacheBudgets{});
    auto executor = cache ? GpuSceneExecutor::create(harness.device, *cache.cache)
                          : decltype(GpuSceneExecutor::create(harness.device, *cache.cache)){};
    expectations.expect(cache.hasValue() && executor.hasValue(), "selection edit: executor");
    if (!cache || !executor)
        return;
    const auto cubeRun = runScene(*executor.executor, first_.scene, kSceneBudget);
    const auto afterCube = executor.executor->counters();
    const auto floorRun = runScene(*executor.executor, second.scene, kSceneBudget);
    const auto afterFloor = executor.executor->counters();
    expectations.expect(cubeRun.ready && floorRun.ready, "selection edit: both runs complete");
    expectations.expect(afterFloor.uploads == afterCube.uploads &&
                            afterFloor.cryptomatteDispatches == afterCube.cryptomatteDispatches + 1,
                        "selection edit: the resident upload is reused; only coverage re-runs");
    auto oracleRequest = request(*floor, false);
    oracleRequest.bypassOperationCache = true;
    const auto frame = harness.evaluator.evaluate(floor, oracleRequest, {});
    const auto readback =
        floorRun.ready ? readbackResidentImage(*floorRun.image, kSceneBudget) : decltype(readbackResidentImage(*floorRun.image, kSceneBudget)){};
    expectations.expect(frame.frame() != nullptr && readback.hasValue() &&
                            pixelsExact(readback.pixels, frame.frame()->processImage().pixels()),
                        "selection edit: the edited matte is bit-identical to the CPU oracle");
}

void testCancellationAndBudget(Expectations& expectations, Harness& harness,
                               const bloom::document::AssetRecord& asset) {
    auto context = GpuSceneMediaContext::fromEvaluator(harness.evaluator);
    context.assetBaseDirectory = harness.directory;
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = cryptomattePlan(asset, {"cube"}, false, 97000);
    const auto cancelled = builder.build(plan, request(*plan, false), makeCancelledToken());
    expectations.expect(!cancelled.hasValue() &&
                            cancelled.diagnostic.code == PreparedGpuSceneDiagnosticCode::Cancelled,
                        "cancellation: a cancelled preparation reports Cancelled");
    auto tiny = request(*plan, false);
    tiny.pixelStorageByteLimit = 64;
    const auto refused = builder.build(plan, tiny);
    expectations.expect(!refused.hasValue(), "budget: a request below the rank stack is refused");

    // A native job cancelled right after begin publishes no image, retires, and the same executor
    // then completes the scene with the exact matte.
    const auto prepared = builder.build(plan, request(*plan, false));
    auto cache = GpuSceneCache::create(harness.device, GpuSceneCacheBudgets{});
    auto executor = cache ? GpuSceneExecutor::create(harness.device, *cache.cache)
                          : decltype(GpuSceneExecutor::create(harness.device, *cache.cache)){};
    expectations.expect(prepared.hasValue() && cache.hasValue() && executor.hasValue(),
                        "native cancel: prepares and creates an executor");
    if (!prepared || !cache || !executor)
        return;
    auto& native = *executor.executor;
    const auto begun = native.begin(prepared.scene, kSceneBudget);
    expectations.expect(begun.code == bloom::runtime::GpuSceneExecutorDiagnosticCode::None,
                        "native cancel: begin accepts the scene");
    native.cancel();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (native.state() == bloom::runtime::GpuSceneExecutorJobState::Pending &&
           std::chrono::steady_clock::now() < deadline) {
        (void)native.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectations.expect(native.state() != bloom::runtime::GpuSceneExecutorJobState::Ready &&
                            native.image() == nullptr,
                        "native cancel: no image is published after cancel");
    while (native.hasUnretiredSubmission() && std::chrono::steady_clock::now() < deadline) {
        (void)native.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto rerun = runScene(native, prepared.scene, kSceneBudget);
    auto oracleRequest = request(*plan, false);
    oracleRequest.bypassOperationCache = true;
    const auto frame = harness.evaluator.evaluate(plan, oracleRequest, {});
    const auto readback = rerun.ready ? readbackResidentImage(*rerun.image, kSceneBudget)
                                      : decltype(readbackResidentImage(*rerun.image,
                                                                       kSceneBudget)){};
    expectations.expect(rerun.ready && frame.frame() != nullptr && readback.hasValue() &&
                            pixelsExact(readback.pixels, frame.frame()->processImage().pixels()),
                        "native cancel: the executor retires and then renders the exact matte");
}

} // namespace

int main(const int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid)
            return 2;
        Expectations expectations;
        auto directory = std::filesystem::temp_directory_path() / "bloom_gpu_cryptomatte_test";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        bloom::cryptomatte_fixture::writeCryptomatteExr(directory / "manifest.exr", true);
        bloom::cryptomatte_fixture::writeCryptomatteExr(directory / "plain.exr", false);
        const auto manifest = imageAsset(directory / "manifest.exr", "manifest", 710);
        const auto plain = imageAsset(directory / "plain.exr", "plain", 711);

        CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(directory);
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
            return 77;
        }
        Harness harness{*device.device, evaluator, directory};
        const auto sky = bloom::runtime::formatCryptomatteRawId(
            bloom::runtime::cryptomatteNameId("sky"));
        testParity(expectations, harness, manifest, {"cube", "floor"}, false, false, 93000,
                   "manifest names");
        testParity(expectations, harness, plain, {"cube", "floor", sky}, false, false, 93100,
                   "hash names and a raw ID without a manifest");
        testParity(expectations, harness, manifest, {"sphere", "cube", "floor", "sky"}, false,
                   true, 93200, "every object under a proxy");
        testParity(expectations, harness, manifest, {}, false, false, 93300, "empty selection");
        testParity(expectations, harness, manifest, {"sphere"}, true, false, 93400,
                   "Image output");
        testSelectionEdit(expectations, harness, manifest);
        testCancellationAndBudget(expectations, harness, manifest);
        if (!expectations.ok()) {
            std::cerr << "FAIL: native Cryptomatte expectations failed\n";
            return 1;
        }
        std::cout << "PASS: native GPU Cryptomatte vs CPU oracle\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
