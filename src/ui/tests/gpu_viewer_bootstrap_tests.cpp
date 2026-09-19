// Focused, device-free regressions for the application GPU-preview wiring helpers.
//
// These prove the seams the app integration depends on, without a device, loader, window, or
// viewer:
//
//   1. budget alignment: the GPU-resident sublimits installed on the shared PreviewFrameCache and
//      the service lease-registry caps are derived together, so the cache evicts LRU resident
//      entries before the registry can ever refuse a publishable lease (the 512 MiB-registry vs
//      2 GiB-cache acceleration cliff cannot reappear). Both bytes AND entry count are aligned,
//      conservatively bounded, and derived without an overflowing intermediate;
//   2. capability fallback: Wayland presentation is requested only for a Wayland session with a
//      packaged loader, and a viewer client is handed over only for a genuinely Ready capability;
//   3. relative-media Open/SaveAs: the media context is rebuilt per request from the evaluator's
//      live assetBaseDirectory (a stale startup copy must not be reused), while the coverage and
//      prepared-upload caches stay shared across requests;
//   4. the additive cache sublimits leave the overall CPU cache budget and CPU entries untouched.

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/gpu_viewer_bootstrap.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <QCoreApplication>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>

namespace {

using bloom::render::GpuPresentationAvailability;
using bloom::runtime::CompiledCompositionPlan;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::GpuPreparedUploadCache;
using bloom::runtime::GpuPreviewDisplayServiceStatus;
using bloom::runtime::GpuSceneCoverageCache;
using bloom::runtime::PreparedPreviewFrame;
using bloom::runtime::TaskScheduler;
using bloom::runtime::TaskSchedulerConfig;
using bloom::ui::GpuResidentBudgetPlan;
using bloom::ui::GpuViewerBootstrap;
using bloom::ui::PreviewFrameCache;
using bloom::ui::PreviewFrameCacheKey;
using bloom::ui::ViewerGpuDependencies;

constexpr std::uint64_t kMib = 1024ULL * 1024ULL;
constexpr std::uint64_t kGib = 1024ULL * kMib;

int failures = 0;

void expect(const bool condition, const std::string& label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

// Genuine CPU reference display frame, device-free: the real CpuCompositionEvaluator plus
// CpuReferenceDisplayPreparer. Used only to prove the GPU-resident sublimits never touch CPU
// entries.
[[nodiscard]] std::shared_ptr<const PreparedPreviewFrame> cpuFrame(const std::uint64_t generation) {
    namespace document = bloom::document;
    namespace runtime = bloom::runtime;
    constexpr auto kProjectId = document::ProjectId::fromRaw(1);
    constexpr auto kCompositionId = document::CompositionId::fromRaw(2);
    constexpr auto kSolidNode = document::NodeId::fromRaw(10);
    constexpr auto kLayerNode = document::NodeId::fromRaw(11);
    constexpr auto kStackNode = document::NodeId::fromRaw(12);
    constexpr auto kOutputNode = document::NodeId::fromRaw(13);
    constexpr auto kLayer = document::LayerId::fromRaw(20);
    constexpr auto kSlot = document::LayerSlotId::fromRaw(30);
    const auto compositionFormat = document::CompositionFormat::create(64, 36);
    if (!compositionFormat.has_value()) {
        return nullptr;
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledSolid{
        kSolidNode,
        {document::ParameterId::fromRaw(40), bloom::core::Color4d{1.0, 0.0, 0.0, 1.0}},
        {document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1000),
         static_cast<double>(compositionFormat->width())},
        {document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1001),
         static_cast<double>(compositionFormat->height())}});
    operations.emplace_back(runtime::CompiledLayerOutput{
        kLayerNode, kLayer, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(41),
                                       document::Vec2d{2.0, 1.0}},
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(43),
                                       document::kDefaultAnchor},
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(44), document::kDefaultScale},
        runtime::CompiledScalarParameter{document::ParameterId::fromRaw(45),
                                         document::kDefaultRotationDegrees},
        runtime::CompiledScalarParameter{document::ParameterId::fromRaw(42), 1.0},
        document::ParameterId::fromRaw(46), bloom::core::kDefaultBlendMode});
    operations.emplace_back(runtime::CompiledMerge{
        kStackNode,
        {runtime::CompiledMergeInput{kSlot, kLayer, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(2)});
    auto plan =
        std::make_shared<const CompiledCompositionPlan>(runtime::CompiledCompositionPlanDefinition{
            document::Revision::fromRaw(7), kProjectId, kCompositionId, *compositionFormat,
            std::move(operations), runtime::OperationIndex::fromRaw(3)});
    const CpuCompositionEvaluator evaluator;
    const auto evaluated = evaluator.evaluate(
        plan,
        runtime::EvaluationRequest{.time = bloom::core::RationalTime::fromInteger(0),
                                   .output = plan->output(),
                                   .resolution = runtime::CompositionFormatResolution{},
                                   .quality = runtime::EvaluationQuality::Reference,
                                   .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                   .pixelStorageByteLimit = std::size_t{1} << 22},
        {});
    if (evaluated.frame() == nullptr) {
        return nullptr;
    }
    const runtime::CpuReferenceDisplayPreparer preparer;
    const auto display =
        preparer.prepare(evaluated.frame(),
                         runtime::ReferenceDisplayPreparationRequest{
                             .intent = runtime::ReferenceDisplayIntent::LinearRec709SceneToSrgb,
                             .aggregatePixelStorageByteLimit = std::size_t{1} << 22,
                             .viewAdjust = {},
                             .displayName = {},
                             .viewName = {},
                             .showLook = true},
                         {});
    if (display.frame() == nullptr) {
        return nullptr;
    }
    const auto prepared = PreparedPreviewFrame::create(generation, display.frame());
    if (!prepared.has_value()) {
        return nullptr;
    }
    return std::make_shared<const PreparedPreviewFrame>(*prepared);
}

void testBudgetPlanIsBoundedAndAligned() {
    const auto aligned = bloom::ui::gpuResidentBudgetPlanFor(2ULL * kGib);
    expect(aligned.cacheBytes >= 256ULL * kMib && aligned.cacheBytes <= 1ULL * kGib,
           "the GPU-resident cache sublimit is conservatively bounded");
    expect(aligned.cacheEntries >= 32 && aligned.cacheEntries <= 256,
           "the GPU-resident entry sublimit is bounded");
    expect(aligned.leaseBytes >= aligned.cacheBytes,
           "registry bytes are never smaller than the resident cache sublimit");
    expect(aligned.leaseEntries >= aligned.cacheEntries,
           "registry entries are never smaller than the resident cache sublimit");
    expect(aligned.leaseEntries > aligned.cacheEntries,
           "registry entries add bounded headroom for in-flight/visible leases");
    expect(aligned.leaseBytes > aligned.cacheBytes,
           "registry bytes add bounded headroom for in-flight/visible leases");
    expect(aligned.leaseBytes <= 2ULL * kGib, "registry bytes are conservatively capped");
    expect(aligned.sceneCacheBytes > 0 && aligned.sceneCacheBytes <= 512ULL * kMib,
           "the resident scene cache is bounded");

    const auto floored = bloom::ui::gpuResidentBudgetPlanFor(0);
    expect(floored.cacheBytes >= 256ULL * kMib, "a zero cache budget floors the resident subset");
    expect(floored.cacheEntries >= 32, "the floor plan still admits resident entries");
    expect(floored.leaseBytes >= floored.cacheBytes, "the floor plan keeps the alignment");
    expect(floored.leaseEntries >= floored.cacheEntries,
           "the floor plan keeps the count alignment");

    // A huge RAM cache must not overflow the derivation or inflate VRAM: every field is capped.
    const auto huge = bloom::ui::gpuResidentBudgetPlanFor(std::size_t{1} << 60U);
    expect(huge.cacheBytes == 1ULL * kGib, "an enormous cache budget caps the resident subset");
    expect(huge.cacheEntries == aligned.cacheEntries && huge.cacheEntries <= 256,
           "an enormous cache budget caps resident entries at the same bounded set");
    expect(huge.leaseBytes <= 2ULL * kGib, "an enormous cache budget caps registry bytes");
    expect(huge.leaseBytes >= huge.cacheBytes, "the huge plan keeps the byte alignment");
    expect(huge.leaseEntries >= huge.cacheEntries, "the huge plan keeps the count alignment");

    const auto small = bloom::ui::gpuResidentBudgetPlanFor(1ULL * kGib);
    expect(aligned.cacheBytes > small.cacheBytes,
           "a larger frame cache raises the resident subset");
    expect(aligned.leaseEntries > small.leaseEntries,
           "a larger frame cache raises the registry entry cap");
}

void testBudgetPlanAppliesToOptions() {
    bloom::runtime::GpuPreviewDisplayServiceOptions options;
    options.enabled = false;
    const auto plan = bloom::ui::gpuResidentBudgetPlanFor(3ULL * kGib);
    bloom::ui::applyGpuResidentBudgetPlan(options, plan);
    expect(options.residentLeaseBudgets.maxBytes == plan.leaseBytes,
           "the plan sets the lease byte budget");
    expect(options.residentLeaseBudgets.maxEntries == plan.leaseEntries,
           "the plan sets the lease entry budget");
    expect(options.residentSceneCacheBudgets.maxRetainedBytes == plan.sceneCacheBytes,
           "the plan sets the scene-cache budget");
    expect(!options.enabled, "budget alignment never enables the service");
    expect(options.presentation ==
               bloom::runtime::GpuPreviewDisplayServicePresentationMode::Disabled,
           "budget alignment never changes the presentation mode");
}

void testCacheGpuLimitsAreAdditive() {
    PreviewFrameCache cache(4ULL * kGib);
    expect(cache.gpuResidentByteLimit() == std::numeric_limits<std::size_t>::max(),
           "a standalone cache has unlimited GPU-resident byte limits by default");
    expect(cache.gpuResidentEntryLimit() == std::numeric_limits<std::size_t>::max(),
           "a standalone cache has unlimited GPU-resident entry limits by default");
    expect(cache.gpuResidentBytes() == 0 && cache.gpuResidentEntryCount() == 0,
           "GPU-resident accounting starts empty");

    const auto budgetBefore = cache.byteBudget();
    const auto plan = bloom::ui::gpuResidentBudgetPlanFor(2ULL * kGib);
    bloom::ui::applyGpuResidentCacheLimits(cache, plan);
    expect(cache.gpuResidentByteLimit() == plan.cacheBytes,
           "the GPU-resident byte sublimit is installed");
    expect(cache.gpuResidentEntryLimit() == plan.cacheEntries,
           "the GPU-resident entry sublimit is installed");
    expect(cache.byteBudget() == budgetBefore,
           "installing GPU-resident sublimits never changes the overall CPU cache budget");
    expect(cache.size() == 0 && cache.residentBytes() == 0,
           "installing GPU-resident sublimits evicts no CPU entry");
}

// A genuine CPU reference frame retained in a cache whose GPU-resident cap is ZERO must survive:
// the sublimits are scoped to the resident arm and must never evict or count CPU content.
void testCpuEntriesSurviveGpuSublimits() {
    auto frame = cpuFrame(1);
    expect(frame != nullptr, "the genuine CPU reference frame is built");
    if (frame == nullptr) {
        return;
    }
    const auto key = PreviewFrameCacheKey::forIdentity(frame->desiredIdentity());
    PreviewFrameCache cache(4ULL * kGib);
    cache.setGpuResidentLimits(0, 0);
    cache.insert(frame);
    expect(cache.contains(key), "a CPU entry survives a zero GPU-resident cap");
    expect(cache.gpuResidentBytes() == 0 && cache.gpuResidentEntryCount() == 0,
           "the CPU entry is not counted as GPU-resident");
    expect(cache.size() == 1 && cache.residentBytes() == PreviewFrameCache::frameByteCost(*frame),
           "the CPU entry is retained and charged normally");
    cache.setGpuResidentLimits(0, 0);
    expect(cache.contains(key), "re-installing GPU sublimits never evicts a CPU entry");
    expect(cache.statistics().gpuResidentEvictions == 0,
           "no GPU-resident eviction is counted for CPU content");
}

void testCapabilityFallback() {
    expect(!bloom::ui::shouldRequestWaylandPresentation(false, false),
           "no loader and no Wayland: no presentation");
    expect(!bloom::ui::shouldRequestWaylandPresentation(true, false),
           "loader without Wayland: no presentation");
    expect(!bloom::ui::shouldRequestWaylandPresentation(false, true),
           "Wayland without a packaged loader: no presentation");
    expect(bloom::ui::shouldRequestWaylandPresentation(true, true),
           "Wayland with a packaged loader requests presentation");

    expect(!bloom::ui::gpuPresentationAvailable(GpuPresentationAvailability::NotRequested),
           "NotRequested is not available");
    expect(!bloom::ui::gpuPresentationAvailable(GpuPresentationAvailability::Unavailable),
           "Unavailable is not available");
    expect(bloom::ui::gpuPresentationAvailable(GpuPresentationAvailability::Ready),
           "Ready is available");
    expect(!bloom::ui::gpuViewerClientUsable(nullptr, GpuPresentationAvailability::Ready),
           "a missing client is never usable even when the capability is Ready");
}

void testMediaContextFollowsSessionBaseDirectory() {
    CpuCompositionEvaluator evaluator;
    auto uploadCache = std::make_shared<GpuPreparedUploadCache>();
    const auto directoryA = std::filesystem::temp_directory_path() / "bloom_wiring_media_a";
    const auto directoryB = std::filesystem::temp_directory_path() / "bloom_wiring_media_b";

    evaluator.setAssetBaseDirectory(directoryA);
    const auto before = bloom::ui::gpuSceneMediaContextFor(evaluator, uploadCache);
    expect(before.assetBaseDirectory == directoryA,
           "the context copies the live session base directory");
    expect(before.preparedUploadCache.get() == uploadCache.get(),
           "the shared prepared-upload cache is installed");
    expect(before.operationCache.get() == evaluator.operationCache().get(),
           "the evaluator operation cache is shared");

    // Simulate Open/SaveAs moving the session base directory. A startup-captured context would keep
    // resolving relative media against directoryA; the per-request refresh must follow directoryB.
    evaluator.setAssetBaseDirectory(directoryB);
    const auto after = bloom::ui::gpuSceneMediaContextFor(evaluator, uploadCache);
    expect(after.assetBaseDirectory == directoryB,
           "the refreshed context follows the Open/SaveAs base directory");
    expect(before.assetBaseDirectory == directoryA,
           "the earlier context is an immutable copy, not a live view");
    expect(after.preparedUploadCache.get() == before.preparedUploadCache.get(),
           "the prepared-upload cache is shared across requests");
    expect(after.operationCache.get() == before.operationCache.get(),
           "the operation cache is shared across requests");
}

void testRefreshingStageIsConstructible() {
    bloom::runtime::NodeDefinitionRegistry nodeDefinitions;
    expect(bloom::runtime::registerBuiltInNodeDefinitions(nodeDefinitions),
           "built-in node definitions register");
    nodeDefinitions.freeze();
    bloom::runtime::SnapshotCompiler compiler(nodeDefinitions);
    bloom::runtime::QualifiedDisplayProcessorProvider provider;
    CpuCompositionEvaluator evaluator;
    auto coverage = std::make_shared<GpuSceneCoverageCache>();
    auto upload = std::make_shared<GpuPreparedUploadCache>();
    auto stage = bloom::ui::makeSessionRefreshingGpuSceneStage(compiler, evaluator, provider,
                                                               coverage, upload);
    expect(static_cast<bool>(stage), "the session-refreshing GPU scene stage is constructible");
}

void testBootstrapCachesOnlyReadyCapability() {
    TaskSchedulerConfig config = TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    TaskScheduler scheduler(config);
    GpuViewerBootstrap bootstrap(scheduler, "/nonexistent/loader.so", 2.0);

    expect(!bootstrap.clientAvailable(), "no capability is cached before a status refresh");
    expect(bootstrap.publicationCount() == 0, "nothing is published before a status refresh");

    GpuPreviewDisplayServiceStatus initial;
    bootstrap.refreshFromStatus(initial);
    expect(!bootstrap.clientAvailable(), "Initializing/NotRequested caches no client");
    expect(bootstrap.publicationCount() == 0, "a null client publishes nothing");
    const ViewerGpuDependencies initialDeps = bootstrap.dependencies();
    expect(static_cast<bool>(initialDeps.presentationClient), "the getter is always installed");
    expect(initialDeps.presentationClient() == nullptr, "the cached client is null when unusable");
    expect(initialDeps.scheduler == &scheduler, "the scheduler is carried through");
    expect(initialDeps.devicePixelRatio == 2.0, "the device pixel ratio is carried through");
    expect(initialDeps.vulkanLoaderPath == "/nonexistent/loader.so", "the loader path is carried");

    GpuPreviewDisplayServiceStatus readyButClientless;
    readyButClientless.presentationAvailability = GpuPresentationAvailability::Ready;
    bootstrap.refreshFromStatus(readyButClientless);
    expect(!bootstrap.clientAvailable(), "Ready availability without a client is still not usable");

    scheduler.beginShutdown();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    testBudgetPlanIsBoundedAndAligned();
    testBudgetPlanAppliesToOptions();
    testCacheGpuLimitsAreAdditive();
    testCpuEntriesSurviveGpuSublimits();
    testCapabilityFallback();
    testMediaContextFollowsSessionBaseDirectory();
    testRefreshingStageIsConstructible();
    testBootstrapCachesOnlyReadyCapability();

    if (failures != 0) {
        std::cerr << "FAIL: " << failures << " GPU viewer bootstrap expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: GPU viewer bootstrap helpers\n";
    return 0;
}
