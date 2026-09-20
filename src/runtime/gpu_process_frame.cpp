#include <bloom/runtime/gpu_process_frame.hpp>

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_process_readback.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
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
#include <optional>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime {
namespace {

using namespace std::chrono_literals;

GpuProcessFrameDiagnosticCode mapSceneDiagnostic(PreparedGpuSceneDiagnosticCode code) {
    switch (code) {
    case PreparedGpuSceneDiagnosticCode::UnsupportedOperation:
    case PreparedGpuSceneDiagnosticCode::UnsupportedTransform:
    case PreparedGpuSceneDiagnosticCode::UnsupportedBlend:
    case PreparedGpuSceneDiagnosticCode::UnsupportedRequest:
    case PreparedGpuSceneDiagnosticCode::MediaUnavailable:
        return GpuProcessFrameDiagnosticCode::SceneUnsupported;
    case PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded:
        return GpuProcessFrameDiagnosticCode::OverBudget;
    case PreparedGpuSceneDiagnosticCode::AllocationFailure:
        return GpuProcessFrameDiagnosticCode::BadAllocation;
    case PreparedGpuSceneDiagnosticCode::Cancelled:
        return GpuProcessFrameDiagnosticCode::Cancelled;
    case PreparedGpuSceneDiagnosticCode::InvalidRequest:
    case PreparedGpuSceneDiagnosticCode::InvalidPlan:
        return GpuProcessFrameDiagnosticCode::InvalidRequest;
    default:
        return GpuProcessFrameDiagnosticCode::InternalInvariant;
    }
}

GpuProcessFrameDiagnosticCode mapExecutorDiagnostic(GpuSceneExecutorDiagnosticCode code) {
    switch (code) {
    case GpuSceneExecutorDiagnosticCode::OverBudget:
        return GpuProcessFrameDiagnosticCode::OverBudget;
    case GpuSceneExecutorDiagnosticCode::Cancelled:
        return GpuProcessFrameDiagnosticCode::Cancelled;
    case GpuSceneExecutorDiagnosticCode::DeviceLost:
        return GpuProcessFrameDiagnosticCode::DeviceLost;
    case GpuSceneExecutorDiagnosticCode::DeviceUnavailable:
        return GpuProcessFrameDiagnosticCode::DeviceUnavailable;
    default:
        return GpuProcessFrameDiagnosticCode::InternalInvariant;
    }
}

GpuProcessFrameDiagnosticCode mapProcessReadbackCode(render::GpuProcessReadbackCode code) {
    switch (code) {
    case render::GpuProcessReadbackCode::None:
        return GpuProcessFrameDiagnosticCode::None;
    case render::GpuProcessReadbackCode::OverBudget:
        return GpuProcessFrameDiagnosticCode::ReadbackOverBudget;
    case render::GpuProcessReadbackCode::DeviceLost:
        return GpuProcessFrameDiagnosticCode::DeviceLost;
    case render::GpuProcessReadbackCode::DeviceUnavailable:
    case render::GpuProcessReadbackCode::WrongThread:
        return GpuProcessFrameDiagnosticCode::DeviceUnavailable;
    case render::GpuProcessReadbackCode::Cancelled:
        return GpuProcessFrameDiagnosticCode::Cancelled;
    default:
        return GpuProcessFrameDiagnosticCode::ReadbackFailed;
    }
}

// A progress callback is caller-supplied and may throw; it must never unwind out of the owner
// worker (or a scheduler task) and take the process down.
void safeProgress(const EvaluationProgressCallback& progress,
                  const EvaluationProgress& event) noexcept {
    if (!progress) {
        return;
    }
    try {
        progress(event);
    } catch (...) {
    }
}

GpuProcessFrameCounters snapshotCounters(const GpuSceneExecutor& executor) noexcept {
    const auto counters = executor.counters();
    GpuProcessFrameCounters result;
    result.sceneBegins = counters.sceneBegins;
    result.scenesCompleted = counters.scenesCompleted;
    result.outputCacheHits = counters.outputCacheHits;
    result.outputCacheMisses = counters.outputCacheMisses;
    result.solidDispatches = counters.solidDispatches;
    result.coveredSolidDispatches = counters.coveredSolidDispatches;
    result.translationDispatches = counters.translationDispatches;
    result.sourceOverDispatches = counters.sourceOverDispatches;
    result.uploads = counters.uploads;
    result.nativeDispatches = counters.dispatches;
    return result;
}

std::string outputSemanticKey(const PreparedGpuScene& scene) {
    const auto index = scene.outputCommand();
    const auto& commands = scene.commands();
    if (index == kInvalidGpuSceneCommand || static_cast<std::size_t>(index) >= commands.size()) {
        return {};
    }
    return std::visit([](const auto& command) -> const std::string& { return command.semanticKey; },
                      commands[index]);
}

GpuProcessFrameOutcome failure(GpuProcessFrameStatus status, GpuProcessFrameDiagnosticCode code,
                               std::string message) {
    GpuProcessFrameOutcome outcome;
    outcome.status = status;
    outcome.diagnostic = {code, std::move(message)};
    return outcome;
}

} // namespace

// One caller-visible request slot. The owner thread owns `outcome`/`hasResult`; the caller owns
// `done`/`hasResult` observation under the shared mutex. Each request carries its own completion
// condition variable, so two concurrent callers each wake exactly once and a queued request is
// cancelled independently of the running one.
struct GpuProcessFrameRequest final {
    std::shared_ptr<const CompiledCompositionPlan> plan;
    std::optional<EvaluationRequest> evaluation;
    CancellationToken cancellation;
    EvaluationProgressCallback progress;

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

    // Bounded FIFO of admitted requests. A caller admits its slot under `mutex`, wakes the owner,
    // and waits on the slot's own condition variable until the owner marks it done or shutdown
    // rejects it. The owner never runs more than one request at a time.
    std::deque<std::shared_ptr<GpuProcessFrameRequest>> queue;
    bool stopping = false;

    void runOwner();
    GpuProcessFrameOutcome runRequest(std::shared_ptr<const CompiledCompositionPlan> plan,
                                      const EvaluationRequest& request,
                                      const CancellationToken& cancellation,
                                      const EvaluationProgressCallback& progress);
    GpuProcessFrameOutcome runRequestImpl(std::shared_ptr<const CompiledCompositionPlan> plan,
                                          const EvaluationRequest& request,
                                          const CancellationToken& cancellation,
                                          const EvaluationProgressCallback& progress);
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
    std::shared_ptr<const CompiledCompositionPlan> plan, const EvaluationRequest& request,
    const CancellationToken& cancellation, const EvaluationProgressCallback& progress) {
    try {
        return runRequestImpl(std::move(plan), request, cancellation, progress);
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
    std::shared_ptr<const CompiledCompositionPlan> plan, const EvaluationRequest& request,
    const CancellationToken& cancellation, const EvaluationProgressCallback& progress) {
    if (executor == nullptr || device == nullptr || cache == nullptr) {
        return failure(GpuProcessFrameStatus::DeviceUnavailable,
                       GpuProcessFrameDiagnosticCode::DeviceUnavailable,
                       "the GPU executor is not initialized");
    }
    if (plan == nullptr) {
        return failure(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::InvalidRequest,
                       "no compiled plan");
    }

    // Respect a prior unproven native submission before reusing the executor: drain on the owner
    // thread with a real bounded deadline, never reuse under an unretired submission.
    if (executor->ownerDrainRequired() || executor->hasUnretiredSubmission()) {
        const auto drainDeadline = std::chrono::steady_clock::now() + options.nativeDeadline;
        while ((executor->ownerDrainRequired() || executor->hasUnretiredSubmission()) &&
               std::chrono::steady_clock::now() < drainDeadline) {
            static_cast<void>(executor->poll());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (executor->ownerDrainRequired() || executor->hasUnretiredSubmission()) {
            return failure(GpuProcessFrameStatus::Failed,
                           GpuProcessFrameDiagnosticCode::DeviceUnavailable,
                           "a prior native submission is not retired; refusing reuse");
        }
    }

    safeProgress(progress, {.stage = EvaluationProgressStage::Preflight,
                            .operation = std::nullopt,
                            .completed = 0,
                            .total = std::nullopt});

    // 1. The genuine CPU-side prepared scene (same builder the resident preview route uses). It
    // resolves the exact operands/geometry without allocating a full RGBA CPU image.
    CpuGpuSceneBuilder builder;
    auto built = builder.build(plan, request, cancellation);
    if (!built.hasValue()) {
        const auto code = mapSceneDiagnostic(built.diagnostic.code);
        const auto status = code == GpuProcessFrameDiagnosticCode::SceneUnsupported
                                ? GpuProcessFrameStatus::UnsupportedGpuSubset
                            : code == GpuProcessFrameDiagnosticCode::Cancelled
                                ? GpuProcessFrameStatus::Cancelled
                                : GpuProcessFrameStatus::Failed;
        return failure(status, code, built.diagnostic.message);
    }
    std::shared_ptr<const PreparedGpuScene> scene = std::move(built.scene);
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

    // 5. The ONE final RGBA32F readback, at the CPU output-adapter boundary, through the production
    // bounded/cancellable primitive. The host-buffer peak across the whole publish path is
    // max(staging VMA allocation + readback vector, readback vector + immutable image), NOT the sum
    // of all three: the staging allocation is released when the fence retires, before the immutable
    // image is built. Both phases hold two pixel-sized buffers, so the preflight requires
    // 2 * pixelsBytes <= readbackByteBudget. The readback primitive independently re-checks the
    // ACTUAL allocator-rounded staging size plus the eventual vector against the same budget before
    // it submits, so allocator rounding cannot exceed the allowance.
    const auto& descriptor = scene->outputDescriptor();
    const std::uint64_t width = descriptor.dataWindow().extent().width();
    const std::uint64_t height = descriptor.dataWindow().extent().height();
    if (width == 0 || height == 0 || width > UINT64_MAX / height ||
        width * height > options.readbackByteBudget / (2U * sizeof(render::Rgba32f))) {
        outcome.status = GpuProcessFrameStatus::Failed;
        outcome.diagnostic = {GpuProcessFrameDiagnosticCode::ReadbackOverBudget,
                              "the final readback host buffers exceed the readback byte budget"};
        return outcome;
    }

    render::GpuProcessReadback readback;
    if (!readback.begin(image, options.readbackByteBudget)) {
        outcome.status = GpuProcessFrameStatus::Failed;
        outcome.diagnostic = {mapProcessReadbackCode(readback.diagnostic().code),
                              readback.diagnostic().message};
        return outcome;
    }
    const auto readbackDeadline = std::chrono::steady_clock::now() + options.nativeDeadline;
    while (readback.state() == render::GpuProcessReadbackState::Pending) {
        if (cancellation.isCancellationRequested() || stopRequested.load()) {
            readback.cancel();
        }
        static_cast<void>(readback.poll());
        if (readback.state() == render::GpuProcessReadbackState::Pending) {
            if (std::chrono::steady_clock::now() >= readbackDeadline) {
                readback.cancel();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (readback.state() != render::GpuProcessReadbackState::Ready) {
        const auto code = mapProcessReadbackCode(readback.diagnostic().code);
        outcome.status = code == GpuProcessFrameDiagnosticCode::Cancelled
                             ? GpuProcessFrameStatus::Cancelled
                             : GpuProcessFrameStatus::Failed;
        outcome.diagnostic = {code, readback.diagnostic().message};
        return outcome;
    }

    // 6. Publish an immutable Rgba32fImage from the readback bytes.
    auto imageBuilder = render::Rgba32fImageBuilder::create(
        descriptor, static_cast<std::size_t>(options.readbackByteBudget));
    if (!imageBuilder) {
        outcome.status = GpuProcessFrameStatus::Failed;
        outcome.diagnostic = {GpuProcessFrameDiagnosticCode::BadAllocation,
                              "the process image host buffer could not be allocated"};
        return outcome;
    }
    auto pixels = readback.take();
    outcome.counters.readbacks = 1;
    auto* imageBuilderPtr = imageBuilder.value();
    const auto originY = descriptor.dataWindow().originY();
    for (std::uint32_t row = 0; row < height; ++row) {
        auto destination = imageBuilderPtr->row(originY + static_cast<std::int64_t>(row));
        if (!destination) {
            outcome.status = GpuProcessFrameStatus::Failed;
            outcome.diagnostic = {GpuProcessFrameDiagnosticCode::InternalInvariant,
                                  "the process image row could not be addressed"};
            return outcome;
        }
        const auto sourceOffset = static_cast<std::size_t>(row) * width;
        std::memcpy(destination.value()->data(), pixels.data() + sourceOffset,
                    static_cast<std::size_t>(width) * sizeof(render::Rgba32f));
    }
    pixels.clear();
    pixels.shrink_to_fit();
    auto frozen = std::move(*imageBuilderPtr).freeze();
    if (!frozen) {
        outcome.status = GpuProcessFrameStatus::Failed;
        outcome.diagnostic = {GpuProcessFrameDiagnosticCode::InternalInvariant,
                              "the process image could not be frozen"};
        return outcome;
    }
    auto processImage = std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));

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
                    gpuReady.store(true, std::memory_order_release);
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

        auto outcome =
            runRequest(active->plan, *active->evaluation, active->cancellation, active->progress);

        std::lock_guard lock(mutex);
        active->outcome = std::move(outcome);
        active->done = true;
        active->completion.notify_all();
    }

    // Device-generation resources must be destroyed on the owner thread that created them.
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
    const CancellationToken& cancellation, EvaluationProgressCallback progress) {
    if (impl_ == nullptr) {
        return failure(GpuProcessFrameStatus::Failed,
                       GpuProcessFrameDiagnosticCode::InternalInvariant, "no evaluator");
    }
    if (!impl_->options.enabled) {
        return failure(GpuProcessFrameStatus::Disabled, GpuProcessFrameDiagnosticCode::Disabled,
                       "GPU process-frame evaluation is disabled");
    }
    // Reentrancy: caller-owned lifetime forbids destroying the evaluator or issuing another
    // evaluate() from inside a progress callback on the owner thread. A non-owner queued caller is
    // fine; only the owner thread itself is rejected, before any wait, so it cannot deadlock on its
    // own completion.
    if (impl_->ownerThreadId.load(std::memory_order_acquire) == std::this_thread::get_id()) {
        return failure(GpuProcessFrameStatus::Failed,
                       GpuProcessFrameDiagnosticCode::InternalInvariant,
                       "reentrant GPU evaluate from the owner thread is not permitted");
    }

    // Admit one request slot under the shared mutex. Shutdown rejects new calls; a full queue
    // applies bounded back-pressure. The request is queued after the currently running one and
    // never shares its completion.
    auto slot = std::make_shared<GpuProcessFrameRequest>();
    slot->plan = std::move(plan);
    slot->evaluation = request;
    slot->cancellation = cancellation;
    slot->progress = std::move(progress);
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
        if (impl_->queue.size() >= impl_->options.maxQueuedRequests) {
            return failure(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::OverBudget,
                           "the GPU evaluator request queue is full");
        }
        impl_->queue.push_back(slot);
    }
    impl_->cv.notify_all();

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
    return impl_ == nullptr || impl_->retired.load(std::memory_order_acquire);
}

} // namespace bloom::runtime
