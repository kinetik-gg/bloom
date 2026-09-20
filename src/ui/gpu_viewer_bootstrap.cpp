#include <bloom/ui/gpu_viewer_bootstrap.hpp>

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace bloom::ui {

namespace {

constexpr std::uint64_t kMebibyte = 1024ULL * 1024ULL;

// The GPU-resident subset of the UI cache is bounded conservatively and independently of how large
// the RAM cache is: a big RAM budget is not evidence of usable VRAM.
constexpr std::uint64_t kResidentCacheMinBytes = 256ULL * kMebibyte;
constexpr std::uint64_t kResidentCacheMaxBytes = 1024ULL * kMebibyte;
constexpr std::size_t kResidentCacheMinEntries = 32;
constexpr std::size_t kResidentCacheMaxEntries = 256;
// A conservative nominal resident frame for turning a byte sublimit into an entry sublimit. A 1080p
// packed RGBA8 frame is about 8 MiB. This is a bound, not a claim about any composition extent.
constexpr std::uint64_t kNominalResidentFrameBytes = 8ULL * kMebibyte;
// The registry cap is the cache sublimit plus bounded headroom for leases that are in flight or
// currently presented but not yet cached. The absolute ceiling is conservative: it never scales
// with the RAM cache budget.
constexpr std::uint64_t kLeaseByteHeadroomCap = 256ULL * kMebibyte;
constexpr std::uint64_t kLeaseBudgetMaxBytes = 2ULL * 1024ULL * kMebibyte;
constexpr std::size_t kLeaseEntryHeadroomMin = 8;
constexpr std::size_t kLeaseEntryHeadroomMax = 64;
constexpr std::size_t kLeaseEntriesMax = 4096;
constexpr std::uint64_t kSceneCacheMaxBytes = 512ULL * kMebibyte;

} // namespace

GpuResidentBudgetPlan
gpuResidentBudgetPlanFor(const std::size_t previewFrameCacheByteBudget) noexcept {
    const std::uint64_t budget = static_cast<std::uint64_t>(previewFrameCacheByteBudget);
    // Halve the RAM cache for the resident subset and clamp conservatively. No intermediate product
    // can overflow: every division happens before any addition, and the added headroom is bounded.
    const std::uint64_t cacheBytes =
        std::clamp(budget / std::uint64_t{2}, kResidentCacheMinBytes, kResidentCacheMaxBytes);
    const std::uint64_t rawEntries = cacheBytes / kNominalResidentFrameBytes;
    const std::size_t cacheEntries = static_cast<std::size_t>(
        std::clamp(rawEntries, static_cast<std::uint64_t>(kResidentCacheMinEntries),
                   static_cast<std::uint64_t>(kResidentCacheMaxEntries)));
    const std::uint64_t byteHeadroom =
        std::min(cacheBytes / std::uint64_t{4}, kLeaseByteHeadroomCap);
    const std::uint64_t leaseBytes = std::min(cacheBytes + byteHeadroom, kLeaseBudgetMaxBytes);
    const std::size_t entryHeadroom =
        std::clamp(cacheEntries / 8, kLeaseEntryHeadroomMin, kLeaseEntryHeadroomMax);
    const std::size_t leaseEntries = std::min(cacheEntries + entryHeadroom, kLeaseEntriesMax);
    const std::uint64_t sceneCacheBytes =
        std::min(cacheBytes / std::uint64_t{2}, kSceneCacheMaxBytes);
    return GpuResidentBudgetPlan{.cacheBytes = cacheBytes,
                                 .cacheEntries = cacheEntries,
                                 .leaseBytes = leaseBytes,
                                 .leaseEntries = leaseEntries,
                                 .sceneCacheBytes = sceneCacheBytes};
}

void applyGpuResidentBudgetPlan(runtime::GpuPreviewDisplayServiceOptions& options,
                                const GpuResidentBudgetPlan& plan) noexcept {
    options.residentLeaseBudgets.maxBytes = plan.leaseBytes;
    options.residentLeaseBudgets.maxEntries = plan.leaseEntries;
    options.residentSceneCacheBudgets.maxRetainedBytes = plan.sceneCacheBytes;
}

void applyGpuResidentCacheLimits(PreviewFrameCache& cache,
                                 const GpuResidentBudgetPlan& plan) noexcept {
    cache.setGpuResidentLimits(static_cast<std::size_t>(plan.cacheBytes), plan.cacheEntries);
}

bool shouldRequestWaylandPresentation(const bool bundledNativeLoader,
                                      const bool waylandSession) noexcept {
    return bundledNativeLoader && waylandSession;
}

bool gpuPresentationAvailable(const render::GpuPresentationAvailability availability) noexcept {
    return availability == render::GpuPresentationAvailability::Ready;
}

bool gpuViewerClientUsable(const std::shared_ptr<runtime::GpuPresentationClient>& client,
                           const render::GpuPresentationAvailability availability) noexcept {
    return client != nullptr && gpuPresentationAvailable(availability);
}

runtime::GpuSceneMediaContext
gpuSceneMediaContextFor(const runtime::CpuCompositionEvaluator& evaluator,
                        const std::shared_ptr<runtime::GpuPreparedUploadCache>& uploadCache) {
    auto context = runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
    if (uploadCache != nullptr) {
        context.preparedUploadCache = uploadCache;
    }
    return context;
}

runtime::PreviewGpuSceneStageFunction makeSessionRefreshingGpuSceneStage(
    const runtime::SnapshotCompiler& compiler, const runtime::CpuCompositionEvaluator& evaluator,
    const runtime::QualifiedDisplayProcessorProvider& qualifiedProcessorProvider,
    std::shared_ptr<runtime::GpuSceneCoverageCache> coverageCache,
    std::shared_ptr<runtime::GpuPreparedUploadCache> uploadCache,
    CompiledPlanCacheHandle planCache,
    std::shared_ptr<const runtime::GpuDisplayProgramService> displayProgramService) {
    return [&compiler, &evaluator, &qualifiedProcessorProvider,
            coverageCache = std::move(coverageCache), uploadCache = std::move(uploadCache),
            planCache = std::move(planCache),
            displayProgramService = std::move(displayProgramService)](
               const document::Snapshot& snapshot,
               const runtime::PreviewRequestIdentity& desiredIdentity,
               const std::size_t pixelStorageByteLimit,
               const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
               runtime::TaskContext& context) {
        // The local builder owns the copied context for exactly this invocation. The evaluator's
        // getters are internally locked, so a concurrent Open/SaveAs base-directory update is
        // observed as a whole, never as a torn path.
        auto mediaContext = gpuSceneMediaContextFor(evaluator, uploadCache);
        const runtime::CpuGpuSceneBuilder builder(coverageCache, std::move(mediaContext));
        auto stage = makeCompositionPreviewGpuSceneStage(compiler, builder,
                                                         qualifiedProcessorProvider, planCache,
                                                         displayProgramService);
        return stage(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                     context);
    };
}

std::shared_ptr<const runtime::GpuDisplayProgramService>
makeGpuDisplayProgramService(runtime::GpuDisplayProgramService::CompileOptionsProvider
                                 optionsProvider) {
    // The provider is stored, never invoked: no resolution/hash/compile happens here. The service
    // performs its first I/O on the GPU-scene CPU worker.
    return std::make_shared<const runtime::GpuDisplayProgramService>(std::move(optionsProvider));
}

GpuViewerBootstrap::GpuViewerBootstrap(runtime::TaskScheduler& scheduler,
                                       std::string vulkanLoaderPath, const double devicePixelRatio)
    : scheduler_(&scheduler), loaderPath_(std::move(vulkanLoaderPath)),
      devicePixelRatio_(devicePixelRatio) {}

void GpuViewerBootstrap::refreshFromStatus(const runtime::GpuPreviewDisplayServiceStatus& status) {
    std::shared_ptr<runtime::GpuPresentationClient> next;
    if (gpuViewerClientUsable(status.presentationClient, status.presentationAvailability)) {
        next = status.presentationClient;
    }
    std::function<void(const ViewerGpuDependencies&)> sink;
    bool changed = false;
    {
        const std::lock_guard lock(mutex_);
        changed = next != client_;
        client_ = std::move(next);
        active_ = client_ != nullptr;
        if (changed) {
            ++publicationCount_;
        }
        sink = sink_;
    }
    if (changed && sink) {
        sink(dependencies());
    }
}

ViewerGpuDependencies GpuViewerBootstrap::dependencies() const {
    return ViewerGpuDependencies{
        .presentationClient =
            [this] {
                const std::lock_guard lock(mutex_);
                return client_;
            },
        .scheduler = scheduler_,
        .vulkanLoaderPath = loaderPath_,
        .devicePixelRatio = devicePixelRatio_,
    };
}

bool GpuViewerBootstrap::clientAvailable() const noexcept {
    const std::lock_guard lock(mutex_);
    return active_;
}

std::uint64_t GpuViewerBootstrap::publicationCount() const noexcept {
    const std::lock_guard lock(mutex_);
    return publicationCount_;
}

void GpuViewerBootstrap::setPublicationSink(
    std::function<void(const ViewerGpuDependencies&)> sink) {
    const std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
}

} // namespace bloom::ui
