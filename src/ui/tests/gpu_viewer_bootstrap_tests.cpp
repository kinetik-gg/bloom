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
    // Explicit 60/20/20 host partition of the artist's ceiling; no fixed device cap anymore.
    expect(aligned.residentPoolBytes == 2ULL * kGib, "the pool is the configured ceiling");
    expect(aligned.leaseBytes == (2ULL * kGib / 5ULL) * 3ULL, "the lease share is 3/5 of the pool");
    expect(aligned.sceneCacheBytes == 2ULL * kGib / 5ULL, "the scene share is 1/5 of the pool");
    expect(aligned.requestBytes == 2ULL * kGib - aligned.leaseBytes - aligned.sceneCacheBytes,
           "the request share is the remaining 1/5 of the pool");
    expect(aligned.leaseBytes > aligned.cacheBytes,
           "registry bytes add headroom for in-flight/visible leases");
    expect(aligned.leaseBytes >= aligned.cacheBytes,
           "registry bytes are never smaller than the resident cache sublimit");
    expect(aligned.leaseEntries > aligned.cacheEntries,
           "registry entries add bounded headroom for in-flight/visible leases");
    expect(aligned.leaseEntries >= aligned.cacheEntries,
           "registry entries are never smaller than the resident cache sublimit");
    expect(aligned.cacheBytes > 0 && aligned.sceneCacheBytes > 0,
           "a configured plan produces positive resident sublimits");

    // An explicit zero ceiling stays zero: no floor forcing a resident route the artist did not ask
    // for, and no blind over-allocation.
    const auto floored = bloom::ui::gpuResidentBudgetPlanFor(0);
    expect(floored.residentPoolBytes == 0 && floored.cacheBytes == 0 && floored.leaseBytes == 0 &&
               floored.sceneCacheBytes == 0 && floored.requestBytes == 0,
           "a zero budget yields an all-zero plan");

    // A huge RAM cache must not overflow the derivation; entries stay bounded by the metadata cap.
    const auto huge = bloom::ui::gpuResidentBudgetPlanFor(std::size_t{1} << 60U);
    expect(huge.cacheBytes > 0 && huge.cacheBytes > aligned.cacheBytes,
           "an enormous cache budget raises the resident subset");
    expect(huge.cacheEntries == 4096, "an enormous cache budget caps resident entries");
    expect(huge.leaseEntries >= huge.cacheEntries, "the huge plan keeps the count alignment");

    const auto small = bloom::ui::gpuResidentBudgetPlanFor(1ULL * kGib);
    expect(aligned.cacheBytes > small.cacheBytes,
           "a larger frame cache raises the resident subset");
    expect(aligned.leaseEntries > small.leaseEntries,
           "a larger frame cache raises the registry entry cap");
}

// Pure capacity policy: the owner clamps the configured route to one shared device pool. The
// 24x6000x4000 RGBA8 retention case must be admitted when the real device budget allows it, and a
// smaller device must scale every ledger down so nothing overcommits.
void testCapacityPlanClampsToDevice() {
    using bloom::runtime::GpuResidentCapacity;
    using bloom::runtime::GpuResidentCapacitySource;
    using bloom::runtime::GpuResidentConfiguredBudgets;
    const std::uint64_t frame6k = 6000ULL * 4000ULL * 4ULL; // 2.3e9 bytes per frame
    const std::uint64_t retention24 = 24ULL * frame6k;

    // A 16 GiB device with a 12 GiB configured ceiling: the pool is capped at half the device, and
    // the resident cache still admits 24 6K RGBA8 frames.
    GpuResidentConfiguredBudgets configured;
    configured.leaseBytes = 12ULL * kGib * 3ULL / 5ULL;
    configured.sceneCacheBytes = 12ULL * kGib / 5ULL;
    configured.requestBytes = 12ULL * kGib - configured.leaseBytes - configured.sceneCacheBytes;
    GpuResidentCapacity device;
    device.source = GpuResidentCapacitySource::MemoryBudget;
    device.availableBytes = 16ULL * kGib;
    device.deviceLocalBytes = 16ULL * kGib;

    const auto plan = bloom::runtime::gpuResidentCapacityPlanFor(configured, device);
    expect(plan.poolBytes == 8ULL * kGib, "the resident pool is half the resolved device budget");
    expect(plan.cacheBytes >= retention24,
           "24 6K RGBA8 frames are retained when the device budget allows it");
    expect(plan.leaseBytes + plan.sceneCacheBytes + plan.requestBytes <= plan.poolBytes,
           "the lease/scene/request shares never exceed the pool");
    expect(plan.leaseBytes >= plan.cacheBytes,
           "the lease ledger includes the cache sublimit with headroom");

    // A smaller 8 GiB device halves the pool again; every ledger scales down and nothing
    // overcommits.
    GpuResidentCapacity smaller;
    smaller.source = GpuResidentCapacitySource::MemoryBudget;
    smaller.availableBytes = 8ULL * kGib;
    const auto clamped = bloom::runtime::gpuResidentCapacityPlanFor(configured, smaller);
    expect(clamped.poolBytes == 4ULL * kGib, "the smaller device caps the resident pool");
    expect(clamped.leaseBytes < plan.leaseBytes && clamped.requestBytes < plan.requestBytes,
           "capacity pressure scales the shared ledgers down");
    expect(clamped.leaseBytes + clamped.sceneCacheBytes + clamped.requestBytes <= clamped.poolBytes,
           "the clamped plan never exceeds the pool");

    // Unknown capacity uses the small safe fallback; it never assumes host RAM is VRAM.
    const auto unknown =
        bloom::runtime::gpuResidentCapacityPlanFor(configured, GpuResidentCapacity{});
    expect(unknown.poolBytes == 256ULL * kMib, "an unknown device falls back to a safe pool");
    expect(unknown.cacheBytes > 0 && unknown.cacheBytes < retention24,
           "an unknown device never admits a large resident subset");

    // Saturated configured values must not overflow the scaling arithmetic.
    GpuResidentConfiguredBudgets saturated;
    saturated.leaseBytes = std::numeric_limits<std::uint64_t>::max();
    saturated.sceneCacheBytes = std::numeric_limits<std::uint64_t>::max();
    saturated.requestBytes = std::numeric_limits<std::uint64_t>::max();
    const auto overflow = bloom::runtime::gpuResidentCapacityPlanFor(saturated, device);
    expect(overflow.poolBytes <= 8ULL * kGib && overflow.cacheBytes <= overflow.leaseBytes,
           "saturated configured budgets scale without overflowing");
}

void testBudgetPlanAppliesToOptions() {
    bloom::runtime::GpuPreviewDisplayServiceOptions options;
    options.enabled = false;
    options.previewByteAllowance = 4ULL * kGib;
    const auto plan = bloom::ui::gpuResidentBudgetPlanFor(3ULL * kGib);
    bloom::ui::applyGpuResidentBudgetPlan(options, plan);
    expect(options.residentLeaseBudgets.maxBytes == plan.leaseBytes,
           "the plan sets the lease byte budget");
    expect(options.residentLeaseBudgets.maxEntries == plan.leaseEntries,
           "the plan sets the lease entry budget");
    expect(options.residentSceneCacheBudgets.maxRetainedBytes == plan.sceneCacheBytes,
           "the plan sets the scene-cache budget");
    expect(options.previewByteAllowance == plan.requestBytes,
           "the plan lowers the request allowance to the shared share");
    expect(!options.enabled, "budget alignment never enables the service");
    expect(options.presentation ==
               bloom::runtime::GpuPreviewDisplayServicePresentationMode::Disabled,
           "budget alignment never changes the presentation mode");
}

// Production binding: the UI derives the configured plan from the artist's ceiling, applies it to
// the service options, the owner resolves a device budget, and a request's HOST decode ceiling is
// clamped to the effective GPU request ceiling -- a large host ceiling must never refuse the GPU,
// and the configured ceiling must still be respected.
void testAppConfiguredPlanFlowSeparatesHostAndGpu() {
    // An 8 GiB configured ceiling fits inside half of a 16 GiB device (the owner pool), so the
    // configured split is preserved while the host decode ceiling is still far larger.
    const std::size_t hostCeiling = 8ULL * kGib;
    const auto configured = bloom::ui::gpuResidentBudgetPlanFor(hostCeiling);
    bloom::runtime::GpuPreviewDisplayServiceOptions options;
    options.enabled = true;
    options.previewByteAllowance =
        64ULL * kGib; // host decode/CPU ceiling, larger than any GPU bound
    bloom::ui::applyGpuResidentBudgetPlan(options, configured);
    expect(options.residentLeaseBudgets.maxBytes == configured.leaseBytes,
           "the configured lease ceiling is applied");
    expect(options.residentSceneCacheBudgets.maxRetainedBytes == configured.sceneCacheBytes,
           "the configured scene ceiling is applied");
    expect(options.previewByteAllowance == configured.requestBytes,
           "the configured request ceiling is the shared GPU share, not the host decode ceiling");

    bloom::runtime::GpuResidentConfiguredBudgets ownerConfigured;
    ownerConfigured.leaseBytes = options.residentLeaseBudgets.maxBytes;
    ownerConfigured.sceneCacheBytes = options.residentSceneCacheBudgets.maxRetainedBytes;
    ownerConfigured.requestBytes = options.previewByteAllowance;
    bloom::runtime::GpuResidentCapacity device;
    device.source = bloom::runtime::GpuResidentCapacitySource::MemoryBudget;
    device.availableBytes = 16ULL * kGib;
    device.deviceLocalBytes = 16ULL * kGib;
    const auto plan = bloom::runtime::gpuResidentCapacityPlanFor(ownerConfigured, device);
    expect(plan.resolved && plan.requestBytes == ownerConfigured.requestBytes,
           "a device with room preserves the configured request ceiling");

    // The controller passes a many-GiB host decode limit. The device stage budget is the min of the
    // host ceiling and the effective GPU ceiling; it is clamped, never a refusal.
    const std::uint64_t gpuBudget =
        bloom::runtime::gpuResidentRequestBudget(64ULL * kGib, plan.requestBytes);
    expect(gpuBudget == plan.requestBytes && gpuBudget > 0,
           "a large host ceiling is clamped to the GPU request ceiling, not refused");
    expect(bloom::runtime::gpuResidentRequestBudget(64ULL * kGib, 0) == 0,
           "a zero GPU request ceiling is honest zero device admission");
    expect(bloom::runtime::gpuResidentRequestBudget(0, plan.requestBytes) == 0,
           "a zero host ceiling is zero device admission");

    // Tight device: the owner scales the configured ledger down but still admits a positive GPU
    // request; the configured ceiling stays an upper bound, never exceeded.
    bloom::runtime::GpuResidentCapacity tight;
    tight.source = bloom::runtime::GpuResidentCapacitySource::MemoryBudget;
    tight.availableBytes = 4ULL * kGib;
    tight.deviceLocalBytes = 4ULL * kGib;
    const auto tightPlan = bloom::runtime::gpuResidentCapacityPlanFor(ownerConfigured, tight);
    expect(tightPlan.requestBytes < ownerConfigured.requestBytes && tightPlan.requestBytes > 0,
           "a tight device scales the configured request ceiling down but still admits the GPU");
    expect(bloom::runtime::gpuResidentRequestBudget(64ULL * kGib, tightPlan.requestBytes) ==
               tightPlan.requestBytes,
           "the device stage budget stays the clamped GPU ceiling under pressure");
}

// The status poll installs the owner-resolved capacity plan on the bound cache exactly when it
// changes, and never queries a device from the UI thread.
void testBootstrapAppliesResolvedCapacity() {
    TaskSchedulerConfig config = TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    TaskScheduler scheduler(config);
    GpuViewerBootstrap bootstrap(scheduler, "/nonexistent/loader.so", 1.0);

    PreviewFrameCache cache(4ULL * kGib);
    bootstrap.bindResidentCache(cache);
    const auto configured = bloom::ui::gpuResidentBudgetPlanFor(4ULL * kGib);
    bloom::ui::applyGpuResidentCacheLimits(cache, configured);
    expect(cache.gpuResidentByteLimit() == configured.cacheBytes,
           "the configured sublimit is installed before any device resolves");

    // (1) An uninitialized default status (resolved == false) changes nothing.
    const bloom::runtime::GpuPreviewDisplayServiceStatus uninitialized;
    bootstrap.refreshFromStatus(uninitialized);
    expect(cache.gpuResidentByteLimit() == configured.cacheBytes,
           "an uninitialized status does not change the installed sublimit");

    // (2) A configured host-only plan is NOT resolved and must be ignored even with a large value.
    bloom::runtime::GpuPreviewDisplayServiceStatus configuredStatus;
    configuredStatus.residentCapacityPlan.cacheBytes = 8ULL * kGib;
    configuredStatus.residentCapacityPlan.cacheEntries = 512;
    bootstrap.refreshFromStatus(configuredStatus);
    expect(cache.gpuResidentByteLimit() == configured.cacheBytes,
           "a configured/unresolved plan is ignored by the poll");

    // (3) A resolved Unknown-device plan is the valid conservative fallback: it MUST be applied so
    // the cache and lease limits stay aligned, and it must not be mistaken for "not resolved yet".
    bloom::runtime::GpuPreviewDisplayServiceStatus fallbackStatus;
    fallbackStatus.residentCapacityPlan.resolved = true;
    fallbackStatus.residentCapacityPlan.capacity.source =
        bloom::runtime::GpuResidentCapacitySource::Unknown;
    fallbackStatus.residentCapacityPlan.poolBytes = 256ULL * kMib;
    fallbackStatus.residentCapacityPlan.cacheBytes = 123ULL * kMib;
    fallbackStatus.residentCapacityPlan.cacheEntries = 16;
    bootstrap.refreshFromStatus(fallbackStatus);
    expect(cache.gpuResidentByteLimit() == 123ULL * kMib && cache.gpuResidentEntryLimit() == 16,
           "a resolved unknown-device fallback is applied by the poll");

    // (4) A resolved known-zero device clears the stale limit (explicit zero is a real resolution).
    bloom::runtime::GpuPreviewDisplayServiceStatus zeroStatus;
    zeroStatus.residentCapacityPlan.resolved = true;
    zeroStatus.residentCapacityPlan.capacity.source =
        bloom::runtime::GpuResidentCapacitySource::MemoryBudget;
    zeroStatus.residentCapacityPlan.cacheBytes = 0;
    zeroStatus.residentCapacityPlan.cacheEntries = 0;
    bootstrap.refreshFromStatus(zeroStatus);
    expect(cache.gpuResidentByteLimit() == 0 && cache.gpuResidentEntryLimit() == 0,
           "a resolved known-zero device clears the resident cache sublimit");

    // A resolved plan that does not change is not re-installed.
    bootstrap.refreshFromStatus(fallbackStatus);
    bootstrap.refreshFromStatus(fallbackStatus);
    expect(cache.gpuResidentByteLimit() == 123ULL * kMib,
           "an unchanged resolved plan is applied once and stays installed");

    scheduler.beginShutdown();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
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
    testCapacityPlanClampsToDevice();
    testBudgetPlanAppliesToOptions();
    testAppConfiguredPlanFlowSeparatesHostAndGpu();
    testBootstrapAppliesResolvedCapacity();
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
