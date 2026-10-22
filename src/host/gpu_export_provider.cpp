#include <bloom/host/gpu_export_provider.hpp>

#include "gpu_export_provider_test_access.hpp"

#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/packed_host_retirement.hpp>

#include <mutex>
#include <stdexcept>
#include <utility>

namespace bloom::host {

struct GpuExportProvider::State final {
    explicit State(runtime::GpuProcessFrameEvaluatorOptions optionsValue,
                   std::shared_ptr<std::timed_mutex> gate)
        : options(std::move(optionsValue)),
          ownsProcessSampling(options.processingPhasePolicy == nullptr),
          bootstrapGate(std::move(gate)) {
        if (ownsProcessSampling) {
            packedHostOwner = runtime::PackedHostRetirementOwner::create();
            options.processingPhasePolicy =
                std::make_shared<runtime::GpuProcessingPhasePolicy>(32, packedHostOwner);
        }
    }

    runtime::GpuProcessFrameEvaluatorOptions options;
    const bool ownsProcessSampling;
    const std::shared_ptr<std::timed_mutex> bootstrapGate;
    std::shared_ptr<runtime::PackedHostRetirementOwner> packedHostOwner;
    std::mutex samplingMutex;
    std::shared_ptr<runtime::MemoryBudgetLedger> samplingLedger;
    // Same sampled root; escaped contexts keep its ledger alive through unregister.
    std::shared_ptr<runtime::OperationCache> sourcePool;
    std::optional<std::chrono::steady_clock::time_point> sampledAt;
    mutable std::mutex mutex;
    std::shared_ptr<runtime::GpuProcessFrameEvaluator> evaluator;
    // The owned bootstrap task. Kept so retirementComplete() can observe a genuinely terminal task
    // (including one cancelled before its lambda ran) without waiting on the scheduler or storing a
    // raw scheduler pointer.
    std::optional<runtime::TaskHandle<void>> bootstrapHandle;
    std::function<void(std::thread::id)> retirementObserver;
    // Separate from `mutex`: a multi-second OCIO compile on a CPU task must not block evaluator
    // coordination. Guarded independently.
    std::mutex displayMutex;
    std::optional<runtime::GpuOcioCompileOptions> displayCompile;
    // Shared, not unique: prepareGpuDisplayCommand() takes a shared reference under `displayMutex`
    // and then calls prepare() OUTSIDE it, so a concurrent setGpuDisplayCompileOptions() reset can
    // never destroy the preparer mid-prepare (UAF). The last reference retires it.
    std::shared_ptr<runtime::GpuOcioProgramPreparer> displayPreparer;
    // The production shared context: one off-UI preparer + qualified compile options resolved from
    // the packaged tools by the owning provider's CPU-worker bootstrap. When present it is the ONE
    // preparer used for output display and for the evaluator's media/effect transforms.
    std::shared_ptr<const runtime::GpuSceneOcioContext> ocioContext;
    bool bootstrapScheduled = false;
    bool bootstrapComplete = false;
    bool stopping = false;

    void refreshCapacity(const runtime::CancellationToken& cancellation) {
        if (!ownsProcessSampling || cancellation.isCancellationRequested()) {
            return;
        }
        // This bounded probe is serialized only with other probes from the same process root;
        // evaluator coordination and publication never acquire this mutex.
        const std::lock_guard samplingLock(samplingMutex);
        const auto now = std::chrono::steady_clock::now();
        if (sampledAt && now >= *sampledAt && now - *sampledAt < std::chrono::seconds(5)) {
            return;
        }
        auto sample = options.processingPhasePolicy->sampleHostMemory(cancellation);
        if (cancellation.isCancellationRequested()) {
            return;
        }
        if (!samplingLedger) {
            const auto ledger = std::make_shared<runtime::MemoryBudgetLedger>(
                sample.physicalBytes, sample.machine.availableBytes.value_or(0));
            ledger->beginProcessSampling();
            auto pool = std::shared_ptr<runtime::OperationCache>(
                new runtime::OperationCache(ledger->allocate().operationCacheByteBudget, *ledger),
                [ledger](runtime::OperationCache* cache) { delete cache; });
            const auto originalProvider = options.mediaContextProvider;
            const auto originalContext = options.mediaContext;
            const auto initial = originalProvider ? originalProvider() : originalContext;
            const auto policy = initial.preparedUploadCache
                                    ? initial.preparedUploadCache->configuredPolicy()
                                    : runtime::PreparedUploadRetentionPolicy{};
            auto prepared = std::make_shared<runtime::GpuPreparedUploadCache>(
                pool, packedHostOwner, policy.payloadBytes, policy.entries);
            // Capture the retention bundle, never State: contexts may outlive this provider.
            std::function<runtime::GpuSceneMediaContext()> wrapped = [pool, prepared,
                                                                      originalProvider,
                                                                      originalContext, policy] {
                auto context = originalProvider ? originalProvider() : originalContext;
                if (context.preparedUploadCache &&
                    context.preparedUploadCache->configuredPolicy() != policy)
                    throw std::invalid_argument("incompatible provider prepared retention policy");
                context.operationCache = pool;
                context.preparedUploadCache = prepared;
                return context;
            };
            {
                const std::lock_guard stateLock(mutex);
                if (stopping) {
                    pool->closePreparedUploads();
                    return;
                }
                samplingLedger = ledger;
                sourcePool = std::move(pool);
                options.mediaContextProvider = std::move(wrapped);
            }
        }
        const auto deliveredAt = std::chrono::steady_clock::now();
        const auto state = samplingLedger->applySample(std::move(sample), deliveredAt);
        options.processingPhasePolicy->applyHostSample(state, samplingLedger->physicalByteBudget(),
                                                       deliveredAt);
        sampledAt = deliveredAt;
    }
};

runtime::GpuSceneMediaContext
detail::GpuExportProviderTestAccess::mediaContext(const GpuExportProvider& provider) {
    if (!provider.state_)
        return {};
    std::function<runtime::GpuSceneMediaContext()> source;
    runtime::GpuSceneMediaContext context;
    {
        const std::lock_guard lock(provider.state_->mutex);
        source = provider.state_->options.mediaContextProvider;
        context = provider.state_->options.mediaContext;
    }
    return source ? source() : context;
}

GpuExportProvider::GpuExportProvider(runtime::GpuProcessFrameEvaluatorOptions options,
                                     std::shared_ptr<std::timed_mutex> gate)
    : state_(std::make_shared<State>(std::move(options), std::move(gate))) {}

GpuExportProvider::~GpuExportProvider() {
    beginShutdown();
    // Coordinated callers (the app shutdown coordinator in the UI, or shutdownAndWait() headless)
    // have already proven retirement and released the evaluator. Any started owned host
    // reclaimer has separately exited and been collected, so there is nothing to join. If a
    // caller violates that contract, releasing here joins the native owner worker on this thread;
    // that is a lifecycle bug, not a supported path.
    collectRetired();
    state_.reset();
}

std::shared_ptr<GpuExportProvider>
GpuExportProvider::create(runtime::GpuProcessFrameEvaluatorOptions options,
                          std::shared_ptr<std::timed_mutex> gate) {
    return std::shared_ptr<GpuExportProvider>(
        new GpuExportProvider(std::move(options), std::move(gate)));
}

void GpuExportProvider::prepare(runtime::TaskScheduler& scheduler) {
    if (state_ == nullptr) {
        return;
    }
    std::shared_ptr<State> state;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->bootstrapScheduled) {
            return;
        }
        state_->bootstrapScheduled = true;
        if (state_->stopping) {
            // Shutdown was already requested before any bootstrap was scheduled: there is no
            // device work to wait on, so retirement is trivially complete.
            state_->bootstrapComplete = true;
            return;
        }
        state = state_;
    }

    // Shared ownership, not a borrowed `this`: the provider may be destroyed while this one-time
    // bootstrap is still queued or running. A shutdown that races in leaves the created evaluator
    // to be destroyed on this worker.
    runtime::TaskRequest request(
        "Bootstrap the GPU final-render evaluator",
        {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(2)},
        runtime::TaskPriority::Foreground, runtime::TaskExecutor::Cpu);
    auto submission = scheduler.submit<void>(
        std::move(request),
        [state = std::move(state)](runtime::TaskContext& context) -> runtime::TaskResult<void> {
            bool stopping = false;
            {
                std::lock_guard lock(state->mutex);
                stopping = state->stopping;
            }
            if (!stopping) {
                state->refreshCapacity(context.cancellation());
                // Qualify the packaged GPU shader tools exactly once on this CPU worker (never the
                // UI thread or the evaluator's GPU owner thread) and publish the one shared context
                // to both the evaluator it creates and its own output-display preparation. A typed
                // refusal (missing/invalid tools, cancellation) leaves the context null and the
                // caller on the unchanged CPU reference path; nothing is poisoned for a retry.
                if (state->options.ocioContext == nullptr &&
                    state->options.ocioResolver != nullptr) {
                    const auto resolved = state->options.ocioResolver->resolve(
                        [&context] { return context.isCancellationRequested(); });
                    if (resolved.hasValue()) {
                        state->options.ocioContext = resolved.context;
                        std::lock_guard lock(state->displayMutex);
                        state->ocioContext = resolved.context;
                    }
                }
                // Interpreters explicitly share this neutral gate across owned provider roots.
                // Only native construction/its failed or raced-stop cleanup is serialized; task
                // and frame execution remain independent. Never wait under a state/registry lock.
                std::unique_lock<std::timed_mutex> bootstrapLock;
                if (state->bootstrapGate) {
                    bootstrapLock = std::unique_lock(*state->bootstrapGate, std::defer_lock);
                    while (!bootstrapLock.try_lock_for(std::chrono::milliseconds(1))) {
                        {
                            const std::lock_guard lock(state->mutex);
                            stopping = state->stopping;
                        }
                        if (stopping || context.isCancellationRequested()) {
                            const std::lock_guard lock(state->mutex);
                            state->bootstrapComplete = true;
                            return runtime::TaskResult<void>::cancelled();
                        }
                    }
                }
                {
                    const std::lock_guard lock(state->mutex);
                    stopping = state->stopping;
                }
                if (stopping || context.isCancellationRequested()) {
                    const std::lock_guard lock(state->mutex);
                    state->bootstrapComplete = true;
                    return runtime::TaskResult<void>::cancelled();
                }
                // Declared AFTER the lock: an unpublishable evaluator joins/cleans before unlock.
                std::shared_ptr<runtime::GpuProcessFrameEvaluator> created;
                try {
                    // Blocks only this scheduled worker until the device/cache/executor bootstrap
                    // finishes, or fails and publishes a typed diagnostic.
                    created = runtime::GpuProcessFrameEvaluator::create(state->options);
                } catch (...) {
                    created = nullptr;
                }
                if (created != nullptr) {
                    std::lock_guard lock(state->mutex);
                    if (!state->stopping && !context.isCancellationRequested()) {
                        state->evaluator = std::move(created);
                    }
                    // A shutdown that raced in leaves `created` destroyed here on this worker.
                }
            }
            std::lock_guard lock(state->mutex);
            state->bootstrapComplete = true;
            return runtime::TaskResult<void>::succeeded();
        });
    {
        std::lock_guard lock(state_->mutex);
        if (submission.accepted()) {
            state_->bootstrapHandle = std::move(submission.handle);
        } else {
            // The scheduler refused new work outright: no bootstrap can ever run.
            state_->bootstrapComplete = true;
        }
    }
}

std::shared_ptr<runtime::GpuProcessFrameEvaluator> GpuExportProvider::evaluator() const {
    if (state_ == nullptr) {
        return nullptr;
    }
    std::lock_guard lock(state_->mutex);
    return state_->evaluator;
}

bool GpuExportProvider::prepared() const noexcept {
    if (state_ == nullptr) {
        return true;
    }
    std::lock_guard lock(state_->mutex);
    return state_->bootstrapComplete;
}

bool GpuExportProvider::deviceAvailable() const {
    const auto handle = evaluator();
    return handle != nullptr && handle->gpuAvailable();
}

std::shared_ptr<runtime::GpuProcessingPhasePolicy>
GpuExportProvider::processingPhasePolicy() const noexcept {
    return state_ ? state_->options.processingPhasePolicy : nullptr;
}

std::shared_ptr<runtime::PackedHostRetirementOwner>
GpuExportProvider::ownedPackedHostRetirementOwner() const noexcept {
    return state_ ? state_->packedHostOwner : nullptr;
}

void GpuExportProvider::refreshProcessingCapacity(const runtime::CancellationToken& cancellation) {
    if (state_) {
        state_->refreshCapacity(cancellation);
    }
}

void GpuExportProvider::beginShutdown() noexcept {
    if (state_ == nullptr) {
        return;
    }
    std::shared_ptr<runtime::GpuProcessFrameEvaluator> handle;
    std::shared_ptr<runtime::OperationCache> sourcePool;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        sourcePool = state_->sourcePool;
        handle = state_->evaluator;
    }
    if (handle != nullptr) {
        handle->beginShutdown();
    }
    if (sourcePool)
        sourcePool->closePreparedUploads();
    if (state_->packedHostOwner)
        state_->packedHostOwner->beginShutdown();
}

bool GpuExportProvider::retirementComplete() const {
    if (state_ == nullptr) {
        return true;
    }
    std::lock_guard lock(state_->mutex);
    if (!state_->bootstrapScheduled) {
        // No bootstrap was ever requested: nothing can be in flight.
        return true;
    }
    if (state_->evaluator != nullptr) {
        // A device was published: completion is the owner worker's genuine retirement, never the
        // bootstrap flag.
        return state_->evaluator->retirementComplete();
    }
    if (state_->bootstrapComplete) {
        // No evaluator was published and the bootstrap reached its terminal state without one.
        return true;
    }
    // The bootstrap was scheduled but has not terminalized. It may still be queued or constructing
    // its device; `stopping` is only a signal and is deliberately NOT treated as completion.
    // Observe the owned task's genuine terminal result instead: a task cancelled before its lambda
    // ran still publishes a terminal state through its handle, so this cannot wait forever.
    if (state_->bootstrapHandle.has_value() &&
        state_->bootstrapHandle->tryTakeResult().has_value()) {
        state_->bootstrapComplete = true;
        return true;
    }
    return false;
}

void GpuExportProvider::collectRetired() noexcept {
    if (state_ == nullptr) {
        return;
    }
    std::shared_ptr<runtime::GpuProcessFrameEvaluator> handle;
    std::function<void(std::thread::id)> observer;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->evaluator == nullptr || !state_->evaluator->retirementComplete()) {
            return;
        }
        handle = std::move(state_->evaluator);
        observer = state_->retirementObserver;
    }
    if (observer) {
        observer(std::this_thread::get_id());
    }
    handle.reset(); // joins a finished owner thread; no native work remains
}

bool GpuExportProvider::shutdownAndWait(const std::chrono::milliseconds timeout) noexcept {
    beginShutdown();
    const auto owner = ownedPackedHostRetirementOwner();
    const auto complete = [&] {
        return retirementComplete() && (!owner || owner->exitAcknowledged());
    };
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!complete() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!complete())
        return false;
    collectRetired();
    return !owner || owner->collect();
}

void GpuExportProvider::setRetirementObserver(std::function<void(std::thread::id)> observer) {
    if (state_ == nullptr) {
        return;
    }
    std::lock_guard lock(state_->mutex);
    state_->retirementObserver = std::move(observer);
}

void GpuExportProvider::setGpuDisplayCompileOptions(runtime::GpuOcioCompileOptions options) {
    if (state_ == nullptr) {
        return;
    }
    std::lock_guard lock(state_->displayMutex);
    state_->displayCompile = std::move(options);
    state_->displayPreparer.reset(); // rebuild lazily against the new options
}

bool GpuExportProvider::gpuDisplayPreparationAvailable() const noexcept {
    if (state_ == nullptr) {
        return false;
    }
    std::lock_guard lock(state_->displayMutex);
    if (state_->ocioContext != nullptr && state_->ocioContext->preparer != nullptr) {
        return true;
    }
    return state_->displayCompile.has_value() &&
           runtime::validGpuOcioCompileOptions(*state_->displayCompile);
}

runtime::GpuOcioPreparationResult GpuExportProvider::prepareGpuDisplayCommand(
    const color::ResolvedBloomNeutralConfig& config, const std::string_view display,
    const std::string_view view, const runtime::GpuOcioCommandGeometry geometry,
    const runtime::GpuOcioCancellation& cancel) {
    if (state_ == nullptr) {
        return {nullptr, runtime::GpuOcioPreparationError::InvalidRequest,
                "no GPU export provider"};
    }
    // Take an immutable (preparer, options) pair TOGETHER under the display lock. In production the
    // pair is the one shared GpuSceneOcioContext resolved by the bootstrap on a CPU worker, so the
    // SAME preparer serves output display and the evaluator's media/effect transforms; the shared
    // reference keeps it alive for the whole prepare() call. prepare() runs outside the lock (the
    // compiler/cache are internally synchronized). The explicit compile-options path is retained
    // only as a focused test seam and is itself lifetime-safe.
    runtime::GpuOcioCompileOptions compile;
    std::shared_ptr<runtime::GpuOcioProgramPreparer> preparer;
    {
        std::lock_guard lock(state_->displayMutex);
        if (state_->ocioContext != nullptr && state_->ocioContext->preparer != nullptr) {
            compile = state_->ocioContext->compileOptions;
            preparer = state_->ocioContext->preparer;
        } else {
            if (!state_->displayCompile.has_value() ||
                !runtime::validGpuOcioCompileOptions(*state_->displayCompile)) {
                return {nullptr, runtime::GpuOcioPreparationError::InvalidRequest,
                        "no qualified GPU display compile options were supplied"};
            }
            if (state_->displayPreparer == nullptr) {
                state_->displayPreparer = std::make_shared<runtime::GpuOcioProgramPreparer>(
                    runtime::GpuOcioPreparerBudgets{});
            }
            compile = *state_->displayCompile;
            preparer = state_->displayPreparer;
        }
    }
    runtime::GpuOcioTransformSpec spec;
    spec.kind = runtime::GpuOcioTransformKind::Display;
    spec.display = std::string(display);
    spec.view = std::string(view);
    return preparer->prepare(config, spec, geometry, compile, cancel);
}

} // namespace bloom::host
