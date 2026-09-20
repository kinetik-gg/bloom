#include <bloom/host/gpu_export_provider.hpp>

#include <bloom/runtime/gpu_ocio_context.hpp>

#include <mutex>
#include <utility>

namespace bloom::host {

struct GpuExportProvider::State final {
    explicit State(runtime::GpuProcessFrameEvaluatorOptions optionsValue)
        : options(std::move(optionsValue)) {}

    runtime::GpuProcessFrameEvaluatorOptions options;
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
};

GpuExportProvider::GpuExportProvider(runtime::GpuProcessFrameEvaluatorOptions options)
    : state_(std::make_shared<State>(std::move(options))) {}

GpuExportProvider::~GpuExportProvider() {
    beginShutdown();
    // Coordinated callers (the app shutdown coordinator in the UI, or shutdownAndWait() headless)
    // have already proven retirement and released the evaluator, so there is nothing to join. If a
    // caller violates that contract, releasing here joins the owner worker on this thread; that is
    // a lifecycle bug, not a supported path.
    collectRetired();
    state_.reset();
}

std::shared_ptr<GpuExportProvider>
GpuExportProvider::create(runtime::GpuProcessFrameEvaluatorOptions options) {
    return std::shared_ptr<GpuExportProvider>(new GpuExportProvider(std::move(options)));
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
                    if (!state->stopping) {
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

void GpuExportProvider::beginShutdown() noexcept {
    if (state_ == nullptr) {
        return;
    }
    std::shared_ptr<runtime::GpuProcessFrameEvaluator> handle;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        handle = state_->evaluator;
    }
    if (handle != nullptr) {
        handle->beginShutdown();
    }
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
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!retirementComplete() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool complete = retirementComplete();
    collectRetired();
    return complete;
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
