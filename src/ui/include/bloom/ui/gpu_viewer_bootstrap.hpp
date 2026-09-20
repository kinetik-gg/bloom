#pragma once

// Application-side GPU preview bootstrap helpers for the resident display route.
//
// This header is deliberately small and typed: it carries NO native/Vulkan/Qt handle, starts no
// thread, creates no device, and never duplicates the viewer's own CPU fallback. It only:
//
//   * derives the bounded resident lease/scene-cache budgets from the artist's UI frame-cache
//     budget so the resident registry cannot be permanently smaller than the cache that references
//     its leases (the 512 MiB-registry vs 2 GiB-cache acceleration cliff);
//   * decides whether the Wayland-only presentation capability may be requested at all;
//   * rebuilds the evaluator-owned GpuSceneMediaContext per GPU-scene request so a relative media
//     path follows the live session base directory (Open/SaveAs) instead of a stale startup copy,
//     while sharing the coverage cache and the prepared-upload cache across requests;
//   * caches the service's presentation client/availability and hands every current/future
//     ViewerEditor one typed ViewerGpuDependencies context.
//
// The existing ViewerGpuResidentController already owns the CPU fallback for a resident frame that
// cannot be presented; nothing here re-implements it.

#include <bloom/render/gpu_presentation_types.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_display_arm.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/preview_gpu_scene_stage.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_plan_cache.hpp>
#include <bloom/ui/editor_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace bloom::runtime {
class GpuPresentationClient;
} // namespace bloom::runtime

namespace bloom::ui {

class PreviewFrameCache;

// Bounded resident-route budgets derived from the UI frame-cache byte budget. The plan carries BOTH
// the additive GPU-resident sublimits to install on the shared PreviewFrameCache and the service
// lease-registry caps, so the cache evicts LRU resident entries before the registry can ever refuse
// a publishable lease. Every field is finite: no live lease/pin is invalidated, over-budget
// publications are refused by the service, and the same request takes the CPU fallback.
struct GpuResidentBudgetPlan final {
    // GPU-resident sublimits inside the overall (unchanged) UI cache budget.
    std::uint64_t cacheBytes = 0;
    std::size_t cacheEntries = 0;
    // Service lease-registry caps: always >= the cache sublimits plus bounded headroom for leases
    // that are in flight or currently presented but not yet cached.
    std::uint64_t leaseBytes = 0;
    std::size_t leaseEntries = 0;
    std::uint64_t sceneCacheBytes = 0;

    friend bool operator==(const GpuResidentBudgetPlan&, const GpuResidentBudgetPlan&) = default;
};

// The cache sublimits and the registry caps are derived together from
// `previewFrameCacheByteBudget`. A tiny/zero value yields a bounded positive plan; a huge value is
// capped conservatively (a large RAM cache is NOT treated as usable VRAM). No intermediate product
// can overflow.
[[nodiscard]] GpuResidentBudgetPlan
gpuResidentBudgetPlanFor(std::size_t previewFrameCacheByteBudget) noexcept;

// Applies the plan to the existing service options. It never enables the service and never changes
// the per-request allowance, native budgets, or presentation mode.
void applyGpuResidentBudgetPlan(runtime::GpuPreviewDisplayServiceOptions& options,
                                const GpuResidentBudgetPlan& plan) noexcept;

// Installs the plan's additive GPU-resident sublimits on the shared frame cache. The overall CPU
// cache budget is untouched; only the bounded resident subset is capped, and it evicts LRU resident
// entries (never invalidating a live lease) when it is exceeded.
void applyGpuResidentCacheLimits(PreviewFrameCache& cache,
                                 const GpuResidentBudgetPlan& plan) noexcept;

// The Wayland presentation bootstrap is requested only when the native loader was actually packaged
// AND the live platform is Wayland. Every other platform/session keeps the compute-only or
// CPU-only path with no blank activation claim.
[[nodiscard]] bool shouldRequestWaylandPresentation(bool bundledNativeLoader,
                                                    bool waylandSession) noexcept;

// A viewer may be handed a presentation client only when the owner published a genuinely Ready
// generation AND the client exists. Otherwise the viewer keeps its unchanged CPU paint path.
[[nodiscard]] bool
gpuPresentationAvailable(render::GpuPresentationAvailability availability) noexcept;
[[nodiscard]] bool
gpuViewerClientUsable(const std::shared_ptr<runtime::GpuPresentationClient>& client,
                      render::GpuPresentationAvailability availability) noexcept;

// A fresh, thread-safe copy of the evaluator's media context with the shared prepared-upload cache
// installed. Re-call it per GPU-scene request: fromEvaluator() copies assetBaseDirectory, so a
// cached startup copy would keep resolving relative media against the pre-Open/SaveAs directory.
[[nodiscard]] runtime::GpuSceneMediaContext
gpuSceneMediaContextFor(const runtime::CpuCompositionEvaluator& evaluator,
                        const std::shared_ptr<runtime::GpuPreparedUploadCache>& uploadCache);

// The GPU-scene stage factory. Unlike makeCompositionPreviewGpuSceneStage, it does NOT capture one
// builder for the process lifetime: each invocation constructs a local CpuGpuSceneBuilder over a
// freshly copied media context (sharing the coverage + prepared-upload caches) and immediately
// invokes the existing stage function while that builder is alive. This is the
// media-follows-session seam; the evaluator's canonical accessors are internally locked.
//
// `ocioContextResolver` is the ONE shared, inert runtime::GpuOcioContextResolver. It is resolved on
// this CPU worker per request (idempotent after the first success) and supplies BOTH the builder's
// GpuSceneOcioContext (its single shared GpuOcioProgramPreparer and the validated executable-
// relative tool paths, enabling effects/media/arbitrary ACES working space) and the SAME preparer
// to the general display program service. A null resolver or a failed resolve keeps the builder's
// default fail-closed context and every affected request takes the CPU fallback with a reason.
// Nothing here resolves tools, hashes, or compiles on the UI thread.
[[nodiscard]] runtime::PreviewGpuSceneStageFunction makeSessionRefreshingGpuSceneStage(
    const runtime::SnapshotCompiler& compiler, const runtime::CpuCompositionEvaluator& evaluator,
    const runtime::QualifiedDisplayProcessorProvider& qualifiedDisplayProcessorProvider,
    std::shared_ptr<runtime::GpuSceneCoverageCache> coverageCache,
    std::shared_ptr<runtime::GpuPreparedUploadCache> uploadCache,
    CompiledPlanCacheHandle planCache = nullptr,
    std::shared_ptr<runtime::GpuOcioContextResolver> ocioContextResolver = nullptr);

// Owns the cached presentation capability for the whole application. It is not a QObject, holds no
// native handle, and never blocks. The application refreshes it from the EXISTING TaskUiBridge poll
// (which runs until shutdown), so a capability that is later lost is observed and published as a
// null client to future viewers (through the getter) and to live viewers (through the sink). A
// cached client is INERT until the viewer's own presenter genuinely presents; this class makes no
// presentation claim.
class GpuViewerBootstrap final {
  public:
    GpuViewerBootstrap(runtime::TaskScheduler& scheduler, std::string vulkanLoaderPath,
                       double devicePixelRatio);

    // Cheap, UI-thread-safe: reads only the already-published status. Updates the cached client and
    // invokes the publication sink exactly when the usable client changed -- including a change to
    // null when the capability is lost.
    void refreshFromStatus(const runtime::GpuPreviewDisplayServiceStatus& status);

    [[nodiscard]] ViewerGpuDependencies dependencies() const;
    // True once a genuinely Ready capability has been cached. This is a dependency-availability
    // fact only; it is not a presentation/activation claim.
    [[nodiscard]] bool clientAvailable() const noexcept;
    [[nodiscard]] std::uint64_t publicationCount() const noexcept;
    void setPublicationSink(std::function<void(const ViewerGpuDependencies&)> sink);

  private:
    runtime::TaskScheduler* scheduler_ = nullptr;
    std::string loaderPath_;
    double devicePixelRatio_ = 1.0;
    mutable std::mutex mutex_;
    std::shared_ptr<runtime::GpuPresentationClient> client_;
    bool active_ = false;
    std::uint64_t publicationCount_ = 0;
    std::function<void(const ViewerGpuDependencies&)> sink_;
};

} // namespace bloom::ui
