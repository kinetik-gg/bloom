#include <bloom/runtime/gpu_process_frame.hpp>

#include "gpu_process_frame_preparation_private.hpp"
#include "gpu_process_frame_readback_private.hpp"
#include "gpu_process_frame_support_private.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime {

std::size_t
gpuProcessFrameByteBudgetForAvailable(const std::optional<std::size_t> availableBytes) noexcept {
    // Pure policy, no floor and no ceiling: a conservative quarter of what the host reports
    // available. Unknown availability is a small typed fallback, never an invented large budget.
    constexpr std::size_t kUnknownAvailabilityFallbackBytes = std::size_t{512} * 1024U * 1024U;
    constexpr std::size_t kAvailableShareDenominator = 4U;
    if (!availableBytes.has_value()) {
        return kUnknownAvailabilityFallbackBytes;
    }
    return *availableBytes / kAvailableShareDenominator;
}

std::size_t defaultGpuProcessFrameByteBudget() noexcept {
    return gpuProcessFrameByteBudgetForAvailable(machineMemorySample().availableBytes);
}

// One caller-visible request slot. The calling CPU worker builds the immutable `scene` BEFORE the
// slot is enqueued; the owner thread owns native execution and writes `outcome`. The caller owns
// `done` observation under the shared mutex. Each request carries its own completion condition
// variable, so two concurrent callers each wake exactly once and a queued request is cancelled
// independently of the running one.
struct GpuProcessFrameRequest final {
    std::shared_ptr<const CompiledCompositionPlan> plan;
    std::optional<EvaluationRequest> evaluation;
    CancellationToken cancellation;
    EvaluationProgressCallback progress;
    std::shared_ptr<const PreparedGpuOcioCommand> outputCommand;
    // Built on the calling CPU worker (never the GPU owner) and immutable thereafter.
    std::shared_ptr<const PreparedGpuScene> scene;

    bool done = false;
    GpuProcessFrameOutcome outcome;
    std::condition_variable completion;
};

struct GpuProcessFrameEvaluator::Impl final {
    GpuProcessFrameEvaluatorOptions options;

    std::jthread thread;
    mutable std::mutex mutex;
    std::condition_variable cv;

    bool initialized = false;
    std::atomic_bool gpuReady{false};
    std::atomic_bool stopRequested{false};
    std::atomic_bool retired{false};
    // The owner worker's thread identity. `evaluate()` rejects a call made from the owner thread
    // (a reentrant request issued from inside a progress callback), which would otherwise deadlock
    // waiting on itself.
    std::atomic<std::thread::id> ownerThreadId{};
    GpuProcessFrameDiagnostic availability;

    // Owner-thread only.
    std::unique_ptr<render::GpuDevice> device;
    std::unique_ptr<GpuSceneCache> cache;
    std::unique_ptr<GpuSceneExecutor> executor;
    // Final combined output-colour readback stage (process payload, plus the encoded output when a
    // prepared command is supplied). Created on the owner thread alongside the executor.
    std::unique_ptr<GpuOutputColorStage> outputColor;

    // Bounded FIFO of admitted requests. A caller admits its slot under `mutex`, wakes the owner,
    // and waits on the slot's own condition variable until the owner marks it done or shutdown
    // rejects it. The owner never runs more than one request at a time.
    std::deque<std::shared_ptr<GpuProcessFrameRequest>> queue;
    // CPU scene preparations in flight on calling workers. `inFlightPreparations + queue.size()` is
    // the bounded admission total, so a flood of callers cannot build unboundedly.
    std::size_t inFlightPreparations = 0;
    // Test-only fault counter (see failPreparationAllocationAt).
    std::atomic<std::uint32_t> preparationAllocations{0};
    bool stopping = false;

    void runOwner();
    GpuProcessFrameOutcome runRequest(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                                      const EvaluationRequest& request,
                                      const std::shared_ptr<const PreparedGpuScene>& scene,
                                      const CancellationToken& cancellation,
                                      const EvaluationProgressCallback& progress,
                                      std::shared_ptr<const PreparedGpuOcioCommand> outputCommand);
    GpuProcessFrameOutcome runRequestImpl(
        const std::shared_ptr<const CompiledCompositionPlan>& plan,
        const EvaluationRequest& request, const std::shared_ptr<const PreparedGpuScene>& scene,
        const CancellationToken& cancellation, const EvaluationProgressCallback& progress,
        std::shared_ptr<const PreparedGpuOcioCommand> outputCommand);
};

// Marks a queued-but-not-yet-run request done with a Cancelled outcome and wakes its caller. Called
// by the owner on shutdown and by evaluate() when a queued request observes cancellation.
void completeQueuedAsCancelled(const std::shared_ptr<GpuProcessFrameRequest>& request) {
    request->outcome =
        failure(GpuProcessFrameStatus::Cancelled, GpuProcessFrameDiagnosticCode::Cancelled,
                "the queued GPU request was cancelled before execution");
    request->done = true;
    request->completion.notify_all();
}

GpuProcessFrameOutcome GpuProcessFrameEvaluator::Impl::runRequest(
    const std::shared_ptr<const CompiledCompositionPlan>& plan, const EvaluationRequest& request,
    const std::shared_ptr<const PreparedGpuScene>& scene, const CancellationToken& cancellation,
    const EvaluationProgressCallback& progress,
    std::shared_ptr<const PreparedGpuOcioCommand> outputCommand) {
    try {
        return runRequestImpl(plan, request, scene, cancellation, progress,
                              std::move(outputCommand));
    } catch (const std::bad_alloc&) {
        return failure(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::BadAllocation,
                       "the request failed to allocate its host buffers");
    } catch (const std::exception&) {
        return failure(GpuProcessFrameStatus::Failed,
                       GpuProcessFrameDiagnosticCode::InternalInvariant,
                       "the request failed with an internal error");
    } catch (...) {
        return failure(GpuProcessFrameStatus::Failed,
                       GpuProcessFrameDiagnosticCode::InternalInvariant,
                       "the request failed with an unknown internal error");
    }
}

GpuProcessFrameOutcome GpuProcessFrameEvaluator::Impl::runRequestImpl(
    const std::shared_ptr<const CompiledCompositionPlan>& plan, const EvaluationRequest& request,
    const std::shared_ptr<const PreparedGpuScene>& scene, const CancellationToken& cancellation,
    const EvaluationProgressCallback& progress,
    std::shared_ptr<const PreparedGpuOcioCommand> outputCommand) {
    if (executor == nullptr || device == nullptr || cache == nullptr) {
        return failure(GpuProcessFrameStatus::DeviceUnavailable,
                       GpuProcessFrameDiagnosticCode::DeviceUnavailable,
                       "the GPU executor is not initialized");
    }
    if (plan == nullptr || scene == nullptr) {
        return failure(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::InvalidRequest,
                       "no prepared scene");
    }

    // Respect a prior unproven native submission before reusing the executor: drain on the owner
    // thread with a real bounded deadline, never reuse under an unretired submission.
    const auto outputColorUnretired = [this] {
        return outputColor != nullptr && outputColor->hasUnretiredSubmission();
    };
    if (executor->ownerDrainRequired() || executor->hasUnretiredSubmission() ||
        outputColorUnretired()) {
        const auto drainDeadline = std::chrono::steady_clock::now() + options.nativeDeadline;
        while ((executor->ownerDrainRequired() || executor->hasUnretiredSubmission() ||
                outputColorUnretired()) &&
               std::chrono::steady_clock::now() < drainDeadline) {
            static_cast<void>(executor->poll());
            if (outputColor != nullptr) {
                static_cast<void>(outputColor->poll());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (executor->ownerDrainRequired() || executor->hasUnretiredSubmission() ||
            outputColorUnretired()) {
            return failure(GpuProcessFrameStatus::Failed,
                           GpuProcessFrameDiagnosticCode::DeviceUnavailable,
                           "a prior native submission is not retired; refusing reuse");
        }
    }

    // 1. The prepared scene was built by the CALLING CPU worker before this request was enqueued,
    // so no media decode, OCIO configuration, or shader compilation runs on the GPU owner thread.
    // The owner only performs native work on the immutable scene.
    if (cancellation.isCancellationRequested() || stopRequested.load()) {
        return failure(GpuProcessFrameStatus::Cancelled, GpuProcessFrameDiagnosticCode::Cancelled,
                       "cancelled before GPU dispatch");
    }

    // 2. Admit the scene through the real executor.
    const auto begun = executor->begin(scene, options.requestByteBudget);
    if (begun.code != GpuSceneExecutorDiagnosticCode::None) {
        const auto code = mapExecutorDiagnostic(begun.code);
        return failure(code == GpuProcessFrameDiagnosticCode::Cancelled
                           ? GpuProcessFrameStatus::Cancelled
                           : GpuProcessFrameStatus::Failed,
                       code, begun.message);
    }

    safeProgress(progress, {.stage = EvaluationProgressStage::Operation,
                            .operation = request.output,
                            .completed = 0,
                            .total = std::nullopt});

    // 3. Non-blocking executor poll, bounded by the per-native-job deadline and cancellation. At
    // most one native dispatch starts per poll(); the loop sleeps between polls instead of
    // spinning.
    const auto startedAt = std::chrono::steady_clock::now();
    GpuSceneExecutorPollResult poll = GpuSceneExecutorPollResult::Pending;
    bool deadlineExceeded = false;
    for (;;) {
        if (cancellation.isCancellationRequested() || stopRequested.load()) {
            executor->cancel();
        }
        poll = executor->poll();
        if (poll != GpuSceneExecutorPollResult::Pending) {
            break;
        }
        if (std::chrono::steady_clock::now() - startedAt >= options.nativeDeadline) {
            deadlineExceeded = true;
            executor->cancel();
            // Bounded, deadline-respecting drain: no fixed iteration count and no busy spin.
            const auto drainDeadline = std::chrono::steady_clock::now() + options.nativeDeadline;
            while (std::chrono::steady_clock::now() < drainDeadline) {
                poll = executor->poll();
                if (poll != GpuSceneExecutorPollResult::Pending) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    GpuProcessFrameOutcome outcome;
    outcome.counters = snapshotCounters(*executor);
    // Genuine device identity for diagnostics: read directly from the actual native device on this
    // owner thread. Never fabricated; stays zero for outcomes that never reached a device.
    outcome.deviceOwnershipEpoch = device->ownershipEpoch();
    if (cancellation.isCancellationRequested() || stopRequested.load()) {
        outcome.status = GpuProcessFrameStatus::Cancelled;
        outcome.diagnostic = {GpuProcessFrameDiagnosticCode::Cancelled,
                              "cancelled during GPU dispatch; no frame was published"};
        return outcome;
    }
    if (deadlineExceeded) {
        outcome.status = GpuProcessFrameStatus::Failed;
        outcome.diagnostic = {GpuProcessFrameDiagnosticCode::DeadlineExceeded,
                              "the native scene did not retire within the request deadline"};
        return outcome;
    }
    if (poll != GpuSceneExecutorPollResult::Ready) {
        const auto code = mapExecutorDiagnostic(executor->diagnostic().code);
        outcome.status = code == GpuProcessFrameDiagnosticCode::Cancelled
                             ? GpuProcessFrameStatus::Cancelled
                             : GpuProcessFrameStatus::Failed;
        outcome.diagnostic = {code, executor->diagnostic().message.empty()
                                        ? std::string("the native scene failed")
                                        : executor->diagnostic().message};
        return outcome;
    }

    // 4. Take the resident output image (still GPU-resident).
    auto image = executor->takeImage();
    if (image == nullptr) {
        outcome.status = GpuProcessFrameStatus::Failed;
        outcome.diagnostic = {GpuProcessFrameDiagnosticCode::InternalInvariant,
                              "the executor reported Ready without an output image"};
        return outcome;
    }

    // 5-6. The ONE final combined readback plus the immutable process-image publication, owned by
    // the private readback helper. One submission transfers the exact process payload and, when a
    // prepared command was supplied, exactly one encoded payload; the two are never folded into one
    // transfer and the counters report them separately. With a null command this is the identity
    // arm: process payload only, one payload, no colour work.
    const auto& descriptor = scene->outputDescriptor();
    if (outputColor == nullptr) {
        outcome.status = GpuProcessFrameStatus::Failed;
        outcome.diagnostic = {GpuProcessFrameDiagnosticCode::DeviceUnavailable,
                              "the GPU output-colour stage is not initialized"};
        return outcome;
    }
    auto transfer = detail::runFinalCombinedReadback(
        *outputColor, descriptor, std::move(outputCommand), std::move(image),
        options.readbackByteBudget, options.nativeDeadline, cancellation, stopRequested);
    if (transfer.status != GpuProcessFrameStatus::Evaluated || transfer.processImage == nullptr) {
        outcome.counters.readbacks = transfer.readbacks;
        outcome.status = transfer.status;
        outcome.diagnostic = {transfer.diagnosticCode, std::move(transfer.diagnosticMessage)};
        return outcome;
    }
    outcome.counters.readbacks = transfer.readbacks;
    auto processImage = std::move(transfer.processImage);

    // 7. A genuine immutable ProcessFrame with GPU provenance, built through the real private
    // constructor. The semantic-identity inputs (semantics versions, color intent, quality) are the
    // exact CPU reference values, so a bit-equal GPU frame yields the same semantic identity.
    ProcessFrameIdentity identity{
        .plan = plan,
        .time = request.time,
        .output = request.output,
        .resolution = request.resolution,
        .quality = request.quality,
        .colorIntent = request.colorIntent,
        .provider = EvaluationProvider::GpuResident,
        .evaluatorSemanticsVersion = kCpuCompositionEvaluatorSemanticsVersion,
        .animationSamplingSemanticsVersion = plan->animationSamplingSemanticsVersion(),
        .imagePrimitiveSemanticsVersion = render::kCpuImagePrimitiveSemanticsVersion,
        .roi = request.roi,
        .bypassLookNodes = request.bypassLookNodes,
    };
    std::vector<EvaluatedOperationBounds> bounds(scene->bounds().begin(), scene->bounds().end());
    auto frame = GpuProcessFrameEvaluator::publishGpuProcessFrame(
        std::move(identity), std::move(processImage), std::move(bounds),
        std::string("gpu:") + outputSemanticKey(*scene));

    outcome.status = GpuProcessFrameStatus::Evaluated;
    outcome.frame = std::move(frame);
    // The encoded output transferred by the SAME single submission, kept distinct from the process
    // payload. The identity arm reports None with no encoded bytes.
    outcome.encodedArm = transfer.encodedArm;
    outcome.encodedEffectRgba32f = std::move(transfer.encodedEffectRgba32f);
    outcome.encodedDisplayRgba8 = std::move(transfer.encodedDisplayRgba8);
    outcome.outputCommandIdentity = transfer.outputCommandIdentity;
    outcome.outputColorCounters = transfer.outputColorCounters;
    outcome.diagnostic = {};
    return outcome;
}

std::shared_ptr<const ProcessFrame> GpuProcessFrameEvaluator::publishGpuProcessFrame(
    ProcessFrameIdentity identity, std::shared_ptr<const render::Rgba32fImage> processImage,
    std::vector<EvaluatedOperationBounds> bounds, std::string contentHash) {
    return std::shared_ptr<const ProcessFrame>(
        new ProcessFrame(std::move(identity), std::move(processImage), OperationCacheStatistics{},
                         std::move(bounds), std::vector<CompiledValue>{}, std::move(contentHash)));
}

void GpuProcessFrameEvaluator::Impl::runOwner() {
    ownerThreadId.store(std::this_thread::get_id(), std::memory_order_release);
    // Bootstrap must never terminate the owner thread or leave create() waiting. Every failure path
    // publishes a typed availability diagnostic, marks the evaluator initialized, and wakes
    // create()/evaluate(); the request loop then serves only cancellations until shutdown.
    try {
        render::GpuDeviceCreationOptions creation;
        creation.loader_path = options.loaderPath;
        auto created = render::GpuDevice::create(creation);
        if (created) {
            device = std::move(created.device);
            auto createdCache = GpuSceneCache::create(*device, options.cacheBudgets);
            if (!createdCache) {
                availability = {GpuProcessFrameDiagnosticCode::CacheUnavailable,
                                createdCache.diagnostic.message};
            } else {
                cache = std::move(createdCache.cache);
                auto createdExecutor =
                    GpuSceneExecutor::create(*device, *cache, options.executorBudgets);
                if (!createdExecutor) {
                    availability = {GpuProcessFrameDiagnosticCode::ExecutorUnavailable,
                                    createdExecutor.diagnostic.message};
                } else {
                    executor = std::move(createdExecutor.executor);
                    auto createdOutputColor = GpuOutputColorStage::create(*device);
                    if (!createdOutputColor) {
                        availability = {GpuProcessFrameDiagnosticCode::ExecutorUnavailable,
                                        createdOutputColor.diagnostic.message};
                        executor.reset();
                    } else {
                        outputColor = std::move(createdOutputColor.stage);
                        gpuReady.store(true, std::memory_order_release);
                    }
                }
            }
        } else {
            availability = {GpuProcessFrameDiagnosticCode::DeviceUnavailable,
                            created.diagnostic.message};
        }
    } catch (const std::exception&) {
        gpuReady.store(false, std::memory_order_release);
        availability = {GpuProcessFrameDiagnosticCode::DeviceUnavailable,
                        "GPU bootstrap failed with an internal error"};
    } catch (...) {
        gpuReady.store(false, std::memory_order_release);
        availability = {GpuProcessFrameDiagnosticCode::DeviceUnavailable,
                        "GPU bootstrap failed with an unknown internal error"};
    }

    {
        std::lock_guard lock(mutex);
        initialized = true;
    }
    cv.notify_all();

    for (;;) {
        std::shared_ptr<GpuProcessFrameRequest> active;
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [this] { return stopping || !queue.empty(); });
            // Shutdown drains every queued request as a typed cancellation, then exits. A request
            // that already observed cancellation while queued is also resolved here.
            if (stopping) {
                for (auto& queued : queue) {
                    completeQueuedAsCancelled(queued);
                }
                queue.clear();
                break;
            }
            active = std::move(queue.front());
            queue.pop_front();
        }

        if (active->cancellation.isCancellationRequested() || stopRequested.load()) {
            std::lock_guard lock(mutex);
            completeQueuedAsCancelled(active);
            continue;
        }

        const auto& evaluation = active->evaluation;
        if (!evaluation.has_value()) {
            std::lock_guard lock(mutex);
            active->outcome = failure(GpuProcessFrameStatus::Failed,
                                      GpuProcessFrameDiagnosticCode::InternalInvariant,
                                      "the queued GPU request has no evaluation");
            active->done = true;
            active->completion.notify_all();
            continue;
        }

        auto outcome = runRequest(active->plan, *evaluation, active->scene, active->cancellation,
                                  active->progress, std::move(active->outputCommand));

        std::lock_guard lock(mutex);
        active->outcome = std::move(outcome);
        active->done = true;
        active->completion.notify_all();
    }

    // Device-generation resources must be destroyed on the owner thread that created them.
    outputColor.reset();
    executor.reset();
    cache.reset();
    device.reset();
    // Publish retirement last: a caller that observes this true may release the evaluator without
    // joining a live owner thread; the thread function is about to return.
    retired.store(true, std::memory_order_release);
}

GpuProcessFrameEvaluator::GpuProcessFrameEvaluator(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GpuProcessFrameEvaluator::~GpuProcessFrameEvaluator() {
    if (impl_ != nullptr) {
        // Caller-owned lifetime contract: the evaluator must be destroyed only from a non-owner
        // thread, and must not be destroyed or re-evaluated from inside a progress callback (which
        // runs on the owner thread). Destroying it on the owner thread would join the owner thread
        // to itself and deadlock; evaluate() therefore rejects a reentrant owner-thread call with a
        // typed failure before waiting. Under that contract this join is safe.
        impl_->stopRequested.store(true, std::memory_order_release);
        {
            std::lock_guard lock(impl_->mutex);
            impl_->stopping = true;
        }
        impl_->cv.notify_all();
        if (impl_->thread.joinable()) {
            impl_->thread.join();
        }
    }
}

std::unique_ptr<GpuProcessFrameEvaluator>
GpuProcessFrameEvaluator::create(const GpuProcessFrameEvaluatorOptions& options) {
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    auto* implPointer = impl.get();
    if (options.enabled) {
        impl->thread = std::jthread([implPointer] { implPointer->runOwner(); });
    } else {
        std::lock_guard lock(impl->mutex);
        impl->initialized = true;
        impl->availability = {GpuProcessFrameDiagnosticCode::Disabled,
                              "GPU process-frame evaluation is disabled"};
        // No owner worker exists, so retirement is already complete.
        impl->retired.store(true, std::memory_order_release);
    }
    {
        std::unique_lock lock(impl->mutex);
        impl->cv.wait(lock, [implPointer] { return implPointer->initialized; });
    }
    return std::unique_ptr<GpuProcessFrameEvaluator>(new GpuProcessFrameEvaluator(std::move(impl)));
}

bool GpuProcessFrameEvaluator::gpuAvailable() const noexcept {
    return impl_ != nullptr && impl_->gpuReady.load(std::memory_order_acquire);
}

GpuProcessFrameDiagnostic GpuProcessFrameEvaluator::availabilityDiagnostic() const {
    if (impl_ == nullptr) {
        return {GpuProcessFrameDiagnosticCode::InternalInvariant, "no evaluator"};
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->gpuReady.load(std::memory_order_acquire) ? GpuProcessFrameDiagnostic{}
                                                           : impl_->availability;
}

GpuProcessFrameOutcome GpuProcessFrameEvaluator::evaluate(
    std::shared_ptr<const CompiledCompositionPlan> plan, const EvaluationRequest& request,
    const CancellationToken& cancellation, EvaluationProgressCallback progress,
    std::shared_ptr<const PreparedGpuOcioCommand> outputCommand) {
    if (impl_ == nullptr) {
        return failure(GpuProcessFrameStatus::Failed,
                       GpuProcessFrameDiagnosticCode::InternalInvariant, "no evaluator");
    }
    if (!impl_->options.enabled) {
        return failure(GpuProcessFrameStatus::Disabled, GpuProcessFrameDiagnosticCode::Disabled,
                       "GPU process-frame evaluation is disabled");
    }
    // Reentrancy: a progress callback runs on the CALLING CPU worker (scene preparation) or, for
    // the native-phase event, on the owner. Reject a nested evaluate() from either the same calling
    // thread or the owner thread before any wait, so it cannot deadlock on its own completion.
    static thread_local bool insideEvaluate = false;
    if (insideEvaluate ||
        impl_->ownerThreadId.load(std::memory_order_acquire) == std::this_thread::get_id()) {
        return failure(GpuProcessFrameStatus::Failed,
                       GpuProcessFrameDiagnosticCode::InternalInvariant,
                       "reentrant GPU evaluate is not permitted");
    }
    struct InsideEvaluateGuard final {
        bool& flag;
        ~InsideEvaluateGuard() { flag = false; }
    } insideEvaluateGuard{insideEvaluate};
    insideEvaluate = true;

    // Bounded admission BEFORE any CPU preparation: `inFlightPreparations + queue.size()` is the
    // total admitted set, so a flood of callers cannot build scenes unboundedly. Shutdown rejects
    // new calls; a full set applies back-pressure.
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) {
            return failure(GpuProcessFrameStatus::Failed,
                           GpuProcessFrameDiagnosticCode::DeviceUnavailable,
                           "the GPU evaluator is shutting down; refusing new requests");
        }
        if (!impl_->gpuReady.load(std::memory_order_acquire)) {
            return failure(GpuProcessFrameStatus::DeviceUnavailable,
                           impl_->availability.code == GpuProcessFrameDiagnosticCode::None
                               ? GpuProcessFrameDiagnosticCode::DeviceUnavailable
                               : impl_->availability.code,
                           impl_->availability.message);
        }
        if (impl_->inFlightPreparations + impl_->queue.size() >= impl_->options.maxQueuedRequests) {
            return failure(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::OverBudget,
                           "the GPU evaluator request queue is full");
        }
        ++impl_->inFlightPreparations;
    }
    // Scope-bound reservation release: EVERY exit -- including a bad_alloc from scene preparation,
    // slot allocation, or queue insertion -- releases exactly one reservation. A successful enqueue
    // converts the reservation into a queue slot and marks it released under the same lock.
    struct ReservationGuard final {
        Impl* impl;
        bool released = false;
        ~ReservationGuard() {
            if (released) {
                return;
            }
            released = true;
            std::lock_guard lock(impl->mutex);
            if (impl->inFlightPreparations > 0) {
                --impl->inFlightPreparations;
            }
        }
    } reservationGuard{impl_.get()};

    std::shared_ptr<GpuProcessFrameRequest> slot;
    try {
        if (cancellation.isCancellationRequested()) {
            return failure(GpuProcessFrameStatus::Cancelled,
                           GpuProcessFrameDiagnosticCode::Cancelled,
                           "cancelled before CPU scene preparation");
        }

        safeProgress(progress, {.stage = EvaluationProgressStage::Preflight,
                                .operation = std::nullopt,
                                .completed = 0,
                                .total = std::nullopt});

        // Prepare the immutable scene HERE, on the calling CPU worker, never on the GPU owner
        // thread. Media decode, OCIO configuration, and shader compilation all run on this thread;
        // the owner only performs native work on the finished scene.
        auto preparation = detail::prepareExportScene(impl_->options, plan, request, cancellation);
        if (preparation.scene == nullptr) {
            return failure(preparation.status, preparation.diagnosticCode,
                           std::move(preparation.diagnosticMessage));
        }

        // Test-only deterministic fault seam: simulate the slot allocation failing after admission
        // and CPU preparation, so the reservation-release path is exercised through the public API.
        if (impl_->options.failPreparationAllocationAt != 0 &&
            impl_->preparationAllocations.fetch_add(1, std::memory_order_relaxed) + 1U ==
                impl_->options.failPreparationAllocationAt) {
            throw std::bad_alloc();
        }

        slot = std::make_shared<GpuProcessFrameRequest>();
        slot->plan = std::move(plan);
        slot->evaluation = request;
        slot->cancellation = cancellation;
        slot->progress = std::move(progress);
        slot->outputCommand = std::move(outputCommand);
        slot->scene = std::move(preparation.scene);
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->stopping) {
                return failure(GpuProcessFrameStatus::Failed,
                               GpuProcessFrameDiagnosticCode::DeviceUnavailable,
                               "the GPU evaluator is shutting down; refusing new requests");
            }
            if (cancellation.isCancellationRequested()) {
                return failure(GpuProcessFrameStatus::Cancelled,
                               GpuProcessFrameDiagnosticCode::Cancelled,
                               "cancelled before GPU dispatch");
            }
            impl_->queue.push_back(slot); // may throw; the guard still releases the reservation
            if (impl_->inFlightPreparations > 0) {
                --impl_->inFlightPreparations;
            }
            reservationGuard.released = true;
        }
        impl_->cv.notify_all();
    } catch (const std::bad_alloc&) {
        return failure(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::BadAllocation,
                       "the GPU request failed to allocate its host buffers");
    } catch (const std::exception&) {
        return failure(GpuProcessFrameStatus::Failed,
                       GpuProcessFrameDiagnosticCode::InternalInvariant,
                       "the GPU request failed with an internal error");
    } catch (...) {
        return failure(GpuProcessFrameStatus::Failed,
                       GpuProcessFrameDiagnosticCode::InternalInvariant,
                       "the GPU request failed with an unknown internal error");
    }

    // Wait on this request's own condition variable only. While waiting, if this caller's token is
    // cancelled and this slot is still queued (not running), remove it and return Cancelled
    // promptly without touching the active request. A bounded wait_for keeps this responsive.
    std::unique_lock lock(impl_->mutex);
    while (!slot->done) {
        if (cancellation.isCancellationRequested()) {
            const auto owned = std::find(impl_->queue.begin(), impl_->queue.end(), slot);
            if (owned != impl_->queue.end()) {
                impl_->queue.erase(owned);
                completeQueuedAsCancelled(slot);
            }
        }
        slot->completion.wait_for(lock, std::chrono::milliseconds(2));
    }
    GpuProcessFrameOutcome outcome = std::move(slot->outcome);
    slot->outcome = {};
    return outcome;
}

void GpuProcessFrameEvaluator::beginShutdown() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->stopRequested.store(true, std::memory_order_release);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->cv.notify_all();
    // Every queued request is resolved by the owner loop. The join is deliberately NOT here: safe
    // join belongs to controlled destruction (the caller owns this object's lifetime), and joining
    // from a request served by this same object would self-deadlock.
}

bool GpuProcessFrameEvaluator::retirementComplete() const noexcept {
    if (impl_ == nullptr) {
        return true;
    }
    if (!impl_->retired.load(std::memory_order_acquire)) {
        return false;
    }
    // The owner worker has retired, but an active calling worker may still be preparing a scene (or
    // converting its reservation into a queue slot). Retirement is complete only when no such
    // caller is in flight, so an owner that observes completion may safely release the evaluator
    // without racing a live `evaluate()`.
    std::lock_guard lock(impl_->mutex);
    return impl_->inFlightPreparations == 0;
}

} // namespace bloom::runtime
