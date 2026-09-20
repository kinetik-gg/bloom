#pragma once

// Owner-thread, non-blocking executor for a PreparedGpuScene.
//
// The CPU scene builder produces an ordered, immutable command list
// (`bloom/runtime/prepared_gpu_scene.hpp`) whose semantic keys already encode the resolved
// operands, geometry and pinned shader digests. This executor turns the commands reachable from the
// terminal CompositionOutput into one bounded sequence of already-typed native operations (GpuSolid
// / GpuSolid::beginCovered / GpuImageUpload / GpuComposite translation / GpuComposite source-over),
// one outstanding native dispatch at a time, on the GpuDevice owner thread.
//
// It owns NO thread, service, Qt surface, viewer, or media decode path. It DOES own one
// GpuImageUpload pipeline: an ImageSource/VideoSource leaf command carries the already-decoded,
// colour-converted host image the CPU builder produced, and the executor uploads it once per
// builder semantic source key. The caller (the existing GPU preview service) drives it with
// non-blocking poll() calls. The content cache (GpuSceneCache) is supplied by the caller and keeps
// its own retained-byte budget.
//
// Design rules that this header is the contract for:
//  * begin() validates the whole scene (references, cycles, command count, descriptors, coverage
//    sizing) and refuses before any Vulkan resource exists.
//  * Traversal starts at the output command and consults the semantic content cache first: a cached
//    command cuts the whole subtree, so an unchanged scene performs ZERO native dispatches.
//  * A merge composites a transparent base at the merge window, then source-over bottom-to-top.
//    A composition output crops/pads with an exact zero-offset translation.
//  * Inputs are never mutated; origins and pixel aspect are preserved.
//  * The request byte budget bounds the actual LIVE unique pinned bytes (produced intermediates not
//    yet released, cached inputs pinned for this request, and the output), with aliases charged
//    once and the charge released at the last dependency. The content cache's own retained bytes
//    are a separate ledger. A conservative per-step headroom comes from the live ledger; the native
//    op then enforces its own actual VMA output/transient/ metadata. A long sequential graph whose
//    peak live set fits is accepted even if its cumulative allocation exceeds the budget.
//  * Cancellation, a per-native-job deadline, wrong-thread use and device failure all fail closed;
//    no image is ever published after a cancel. A native failure that does not PROVE fence
//    retirement poisons the request into owner-drain-required and refuses reuse until the owner
//    proves retirement or destroys the executor; the pins/scene are retained until then.
//  * The executor performs no full-frame readback. readbackResidentImage() stays test-only.

#include <bloom/render/gpu_composite.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/runtime/gpu_memory_budget.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace bloom::runtime {

enum class GpuSceneExecutorJobState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuSceneExecutorPollResult : std::uint8_t { Pending, Ready, Failure, WrongThread };

enum class GpuSceneExecutorDiagnosticCode : std::uint8_t {
    None,
    InvalidArgument,
    // The scene failed structural validation (empty, wrong terminal operation, empty window).
    InvalidScene,
    // A command referenced a missing/out-of-range command index.
    InvalidReference,
    // The reachable command graph contains a cycle.
    CycleDetected,
    // The scene has more commands than the configured ceiling.
    TooManyCommands,
    // A command's window/PAR/coverage size is malformed.
    MalformedDescriptor,
    // The scene contains an operation the executor cannot run.
    Unsupported,
    // The actual LIVE unique pinned image bytes (including cached inputs this request pins and the
    // output) would exceed the request byte budget. This is an actual VMA-byte check, never a
    // requested-extent estimate.
    OverBudget,
    WrongThread,
    DeviceUnavailable,
    // The device generation was lost while polling; this executor is terminal and must be
    // destroyed.
    DeviceLost,
    // A previous job has not been retired/taken yet.
    Busy,
    // A prior unproven native submission must be drained (or the executor destroyed) before any new
    // begin. The logical request already failed; this is a reuse gate, not the failure itself.
    OwnerDrainRequired,
    // A cache hit was bound to a different device ownership generation.
    CacheForeignImage,
    // A native begin() refused the request.
    DispatchRefused,
    // A produced image's actual descriptor did not match the scene's command descriptor.
    DescriptorMismatch,
    // A native job did not retire within the configured deadline.
    NativeTimeout,
    // A native failure did not prove the fence retired, so this executor retains every pin until a
    // later owner poll proves retirement (or it is destroyed/quarantined).
    NativeUnproven,
    Cancelled,
    InternalInvariant,
};

struct GpuSceneExecutorDiagnostic final {
    GpuSceneExecutorDiagnosticCode code = GpuSceneExecutorDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuSceneExecutorDiagnostic&,
                           const GpuSceneExecutorDiagnostic&) = default;
};

// Honest execution counters. `uploads` counts native GpuImageUpload begin() calls (one per cold
// source; a warm source is a content-cache hit and runs no upload). `readbacks` stays zero: the
// executor never reads back the output in the normal path. Cache counters are the observed
// GpuSceneCache results for this executor, not the cache's own lifetime counters.
struct GpuSceneExecutorCounters final {
    std::uint64_t sceneBegins = 0;
    std::uint64_t scenesCompleted = 0;
    std::uint64_t scenesFailed = 0;
    std::uint64_t scenesCancelled = 0;
    std::uint64_t budgetRefusals = 0;

    std::uint64_t outputCacheHits = 0;
    std::uint64_t outputCacheMisses = 0;
    std::uint64_t commandCacheHits = 0;
    std::uint64_t commandCacheMisses = 0;

    // Native op begin() calls, by operation family.
    std::uint64_t solidDispatches = 0;
    std::uint64_t coveredSolidDispatches = 0;
    // Native GpuPathCoverage begin() calls: the positive proof that a vector source's coverage was
    // rasterized on the device (not a host mask). A warm scene whose covered output is cached runs
    // zero of these.
    std::uint64_t coverageDispatches = 0;
    std::uint64_t translationDispatches = 0;
    std::uint64_t sourceOverDispatches = 0;
    // Distinct from the composite counters above: an affine placement (GpuAffine) and an explicit
    // blend mode (GpuBlend) are their own operation families. An empty/unchanged warm scene tree
    // runs zero of every one of these.
    std::uint64_t affineDispatches = 0;
    std::uint64_t blendDispatches = 0;
    // One OCIO ProcessEffect dispatch. A warm scene whose OCIO command output is already cached
    // runs zero of these.
    std::uint64_t ocioEffectDispatches = 0;
    // One PointResampleV1 dispatch. A warm scene whose resample output is already cached runs zero
    // of these.
    std::uint64_t pointResampleDispatches = 0;
    // Bounded OCIO native-program cache: creations, warm reuses, evictions, and refusals.
    std::uint64_t ocioProgramCreations = 0;
    std::uint64_t ocioProgramReuses = 0;
    std::uint64_t ocioProgramEvictions = 0;
    std::uint64_t ocioProgramRefusals = 0;
    // Actual VMA retained bytes currently charged to the OCIO program cache.
    std::uint64_t ocioRetainedProgramBytes = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t uploads = 0;
    std::uint64_t readbacks = 0;

    std::uint64_t cacheInsertions = 0;
    std::uint64_t cacheRefusals = 0;
    std::uint64_t nativeJobsCompleted = 0;
    std::uint64_t nativeJobFailures = 0;
    std::uint64_t nativeJobTimeouts = 0;
    std::uint64_t nativeJobCancellations = 0;

    std::uint64_t commandsExecuted = 0;
    std::uint64_t commandsServedFromCache = 0;
    std::uint64_t intermediatePinsReleased = 0;

    // Cumulative telemetry: actual produced allocation bytes and image count, plus the largest
    // single produced image. These are real VMA sizes, not requested extents.
    std::uint64_t cumulativeProducedImageBytes = 0;
    std::uint64_t peakStepImageBytes = 0;
    std::uint64_t producedImages = 0;

    // Live pin ledger: actual bytes of the unique images this request currently pins (produced
    // intermediates not yet released, cached inputs pinned while used, and the output), each alias
    // charged once. `peakLiveImageBytes` is the peak of the live ledger. The content cache's own
    // retained bytes are a separate ledger and are not included.
    std::uint64_t currentLiveImageBytes = 0;
    std::uint64_t peakLiveImageBytes = 0;
    // Actual bytes that would have been double-charged if an image already live under another
    // command index were charged again. Non-zero exactly when an aliased input was deduplicated.
    std::uint64_t aliasedImagePinBytes = 0;
};

struct GpuSceneExecutorProgress final {
    std::uint32_t completedSteps = 0;
    std::uint32_t totalSteps = 0;

    friend bool operator==(const GpuSceneExecutorProgress&,
                           const GpuSceneExecutorProgress&) = default;
};

struct GpuSceneExecutorBudgets final {
    // Per-operation native image ceiling handed to the owned pipelines. This is a configured UPPER
    // BOUND, not an allocation: it is capacity-sized so a large valid source is admitted, and each
    // primitive validates the ACTUAL requested image against the device's real maxResourceSize in
    // begin(). A permissive maximum never refuses pipeline creation.
    std::uint64_t maxImageBytes = gpuProducerMaxImageBytes();
    // Metadata ceiling handed to the composite translation op.
    std::uint64_t maxMetadataBytes = 16ULL * 1024ULL * 1024ULL;
    // Metadata ceiling for the affine sample buffer (16 bytes per output pixel). Affine's metadata
    // is far larger than the composite's: a 1080p output needs ~33 MiB, so this default is chosen
    // to actually fit 1080p plus a bounded margin. It is a real ceiling, never unlimited; the
    // per-request live ledger and the native byte budget still bound the simultaneous peak.
    std::uint64_t maxAffineMetadataBytes = 256ULL * 1024ULL * 1024ULL;
    // Structural ceilings validated before any Vulkan work.
    std::uint64_t maxCommands = 4096;
    std::uint64_t maxCoverageBytes = 256ULL * 1024ULL * 1024ULL;
    // Bounded OCIO native-program cache: entry count and the sum of the programs' ACTUAL VMA
    // retained allocation bytes (never a descriptor-declared sample estimate). The per-program
    // ceilings are forwarded to render::GpuOcioProgram::create.
    std::uint64_t maxOcioPrograms = 8;
    std::uint64_t maxOcioRetainedProgramBytes = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t maxOcioOwnedBytesPerProgram = 512ULL * 1024ULL * 1024ULL;
    std::uint64_t maxOcioLutBytesPerProgram = 256ULL * 1024ULL * 1024ULL;
    // A native job that has not retired by this deadline is cancelled and failed closed.
    std::uint64_t jobDeadlineMilliseconds = 5000;
};

struct GpuSceneExecutorCreateResult;

// Move-only, owner-thread executor. It binds to one GpuDevice ownership generation and one
// GpuSceneCache; both must outlive it. The owned GpuSolid/GpuComposite pipelines are created once
// at create().
class GpuSceneExecutor final {
  public:
    GpuSceneExecutor(const GpuSceneExecutor&) = delete;
    GpuSceneExecutor& operator=(const GpuSceneExecutor&) = delete;
    GpuSceneExecutor(GpuSceneExecutor&& other) noexcept;
    GpuSceneExecutor& operator=(GpuSceneExecutor&& other) noexcept;
    ~GpuSceneExecutor();

    // Device owner thread only. Foreign thread returns WrongThread without touching Vulkan.
    [[nodiscard]] static GpuSceneExecutorCreateResult
    create(render::GpuDevice& device, GpuSceneCache& cache,
           const GpuSceneExecutorBudgets& budgets = {});

    [[nodiscard]] GpuSceneExecutorJobState state() const noexcept;
    [[nodiscard]] const GpuSceneExecutorDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(const render::GpuDevice& device) const noexcept;
    [[nodiscard]] GpuSceneExecutorCounters counters() const noexcept;
    [[nodiscard]] GpuSceneExecutorProgress progress() const noexcept;

    // True while a previous native failure/deadline left a submission whose fence retirement is NOT
    // proven. The logical request has already failed and no image will be published; the caller
    // must keep polling on the owner thread (non-blocking) until this is false, or destroy the
    // executor, before reusing it or releasing GPU admission. begin() returns OwnerDrainRequired
    // meanwhile.
    [[nodiscard]] bool ownerDrainRequired() const noexcept;

    // True while this executor's own native-in-flight flag is set OR any owned native pipeline
    // still holds an unretired submission. This is deliberately broader than ownerDrainRequired():
    // the latter is only the reuse gate latched by an unproven failure, while an ordinary cancelled
    // or pending job is unretired WITHOUT latching it. Retirement is proven only when this is false
    // and state() is not Pending; a Ready output must be taken/discarded first. Additive accessor:
    // no field or layout change.
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;

    // True once a device generation loss was observed. This executor is terminal and unusable; it
    // must be destroyed on the owner thread and recreated on a new generation.
    [[nodiscard]] bool deviceLost() const noexcept;

    // Validates + plans the scene and returns None when accepted. The scene is retained for the job
    // lifetime. A refusal (OverBudget / InvalidScene / InvalidReference / CycleDetected / ...)
    // leaves the executor usable. The request byte budget bounds the actual LIVE unique pinned
    // image bytes for the whole request (deduped aliases), not the sum of every intermediate ever
    // allocated, so a long sequential graph whose peak live set fits is accepted even when its
    // cumulative allocation exceeds the budget.
    [[nodiscard]] GpuSceneExecutorDiagnostic begin(std::shared_ptr<const PreparedGpuScene> scene,
                                                   std::uint64_t requestByteBudget);

    // Non-blocking. Starts at most one native dispatch and completes at most one step per call.
    // While ownerDrainRequired() is true this only advances owner-thread retirement.
    [[nodiscard]] GpuSceneExecutorPollResult poll();

    // The resident output image while Ready; nullptr otherwise. Ownership stays with the executor.
    [[nodiscard]] const render::GpuImage* image() const noexcept;

    // Moves the output pin out and returns to Idle. The cache (if it accepted the output) may still
    // retain the image.
    [[nodiscard]] std::shared_ptr<const render::GpuImage> takeImage();

    // Requests cancellation. Resources/pins are retained until the in-flight native job retires;
    // no image is published after this call.
    void cancel() noexcept;

    // Process-global reported limitation: an owned pipeline could not drain an in-flight job within
    // its bounded teardown budget.
    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    struct Impl;
    explicit GpuSceneExecutor(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuSceneExecutorCreateResult final {
    std::unique_ptr<GpuSceneExecutor> executor;
    GpuSceneExecutorDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return executor != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::runtime
