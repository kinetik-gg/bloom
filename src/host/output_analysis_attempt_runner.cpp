#include <bloom/host/output_analysis_attempt_runner.hpp>

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <new>
#include <type_traits>
#include <utility>

namespace bloom::host {

namespace {

// The PNG-only color half of the blocking stage, in the exact closed input vocabulary
// analyzePngRgba8SrgbV1 accepts. `display` is populated only when both fields below are the
// nominal Ready/Qualified pair.
struct ColorResolutionOutcomeV1 final {
    output::PngRgba8SrgbColorResolutionStateV1 colorResolution =
        output::PngRgba8SrgbColorResolutionStateV1::Ready;
    output::OutputAnalysisAdapterStateV1 adapter = output::OutputAnalysisAdapterStateV1::Qualified;
    output::OutputAnalysisAttemptDisplayProductsV1 display;
};

// Everything the blocking stage hands the Cpu stage, behind ONE shared_ptr: runtime::
// TaskResultValue caps a task result at four pointers (task_scheduler.hpp), and the retained
// target plus the PNG display products exceed that on their own. Bundling them keeps the task
// result a single small handle while still transferring both products by value semantics.
struct ResolvedAttemptInputsV1 final {
    output::OutputAnalysisAttemptTargetV1 target;
    ColorResolutionOutcomeV1 color;
};

struct ResolvingOutcomeV1 final {
    bool succeeded = false;
    // Distinguishes an allocation/internal failure of the PNG color stage (which fails the attempt
    // AT ColorPreparing) from a target-preflight failure (which fails it at Resolving). A merely
    // non-Ready color resolution is neither: it travels in `resolved->color` to the analyzer.
    bool colorStageFailed = false;
    platform::StagedArtifactError error = platform::StagedArtifactError::None;
    std::shared_ptr<const ResolvedAttemptInputsV1> resolved = nullptr;
};
static_assert(runtime::TaskResultValue<ResolvingOutcomeV1>);

// Maps the C2 in-process registry's own closed outcome set onto the analyzer's closed PNG
// color-resolution input states, one to one, per frame-output.md's "The five ocio.* codes map
// one-to-one from the corresponding typed color-resolution failures". `Ready` is handled by the
// caller (it is the only outcome that carries a resolved product to build a processor from).
//
// `LocatorKindRequiresHelper` names a real, planned locator kind that this in-process registry
// never resolves; it is unreachable for the built-in URI this code always passes, and it maps to
// `MissingResource` (`ocio.resource-missing`) -- the honest "the configuration resource this build
// can reach does not cover that locator" state -- rather than being collapsed into `Missing`.
// `UnsupportedVersion` (`ocio.version-unsupported`) has no producer at all in version 1: the
// built-in payload's version is frozen with the binary, so nothing can present a newer one.
[[nodiscard]] output::PngRgba8SrgbColorResolutionStateV1
mapRegistryOutcome(const color::OcioBuiltInRegistryOutcome outcome) noexcept {
    switch (outcome) {
    case color::OcioBuiltInRegistryOutcome::Ready:
        break; // unreachable here; the caller only maps a non-Ready outcome.
    case color::OcioBuiltInRegistryOutcome::Missing:
        return output::PngRgba8SrgbColorResolutionStateV1::Missing;
    case color::OcioBuiltInRegistryOutcome::Changed:
        return output::PngRgba8SrgbColorResolutionStateV1::Changed;
    case color::OcioBuiltInRegistryOutcome::Invalid:
        return output::PngRgba8SrgbColorResolutionStateV1::Invalid;
    case color::OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper:
        return output::PngRgba8SrgbColorResolutionStateV1::MissingResource;
    }
    return output::PngRgba8SrgbColorResolutionStateV1::Missing;
}

// Binds a built or reused processor handle to the retained display-product triple. The identity is
// an ALIASING shared_ptr into the handle's own DisplayProcessorIdentityV1 member -- never an
// independently adopted copy -- so the exported identity can never be paired with a different
// processor (frame-output.md: "Recomputing pixel hashes or substituting an equivalent-looking
// frame or processor at approval or export is forbidden").
[[nodiscard]] std::optional<output::OutputAnalysisAttemptDisplayProductsV1>
retainDisplayProducts(std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> handle) {
    core::Sha256Digest expectedRevision;
    if (const auto view = handle->identity().borrowedView(); view.has_value()) {
        expectedRevision = view->expectedOcioRevision();
    } else {
        return std::nullopt;
    }
    std::shared_ptr<const color::DisplayProcessorIdentityV1> identity(handle, &handle->identity());
    return output::OutputAnalysisAttemptDisplayProductsV1{.processor = std::move(handle),
                                                          .identity = std::move(identity),
                                                          .expectedOcioRevision = expectedRevision};
}

// The attempt graph's step 4. Never throws; a genuine allocation/internal failure is signalled by
// returning nullopt so the caller can fail the attempt AT the ColorPreparing stage, while every
// modelled configuration/adapter state returns a populated outcome that the analyzer turns into a
// truthful, non-approvable report.
[[nodiscard]] std::optional<ColorResolutionOutcomeV1>
resolvePngDisplayProducts(runtime::QualifiedDisplayProcessorProvider* const provider) noexcept {
    try {
        if (provider != nullptr) {
            const auto snapshot = provider->snapshot();
            if (snapshot.readiness == runtime::QualifiedDisplayProcessorReadiness::Ready &&
                snapshot.handle != nullptr) {
                auto products = retainDisplayProducts(snapshot.handle);
                if (!products.has_value()) {
                    return std::nullopt;
                }
                return ColorResolutionOutcomeV1{
                    .colorResolution = output::PngRgba8SrgbColorResolutionStateV1::Ready,
                    .adapter = output::OutputAnalysisAdapterStateV1::Qualified,
                    .display = std::move(*products)};
            }
        }

        auto resolution = color::resolveBloomNeutralV1BuiltIn(
            color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
            color::kBloomNeutralV1ConfigDigest);
        if (!resolution.ready()) {
            return ColorResolutionOutcomeV1{
                .colorResolution = mapRegistryOutcome(resolution.outcome()),
                .adapter = output::OutputAnalysisAdapterStateV1::Qualified,
                .display = {}};
        }
        auto resolved = std::move(resolution).takeResolved();
        if (!resolved.has_value()) {
            return std::nullopt;
        }

        // frame-output.md: "A resolved PNG configuration whose helper, processor, or execution
        // provider cannot run is an adapter-execution failure: Color keeps its Ready nominal tuple
        // while External Dependencies uses adapter.unavailable."
        auto built = color::buildBloomNeutralCpuDisplayProcessor(*resolved);
        auto handleValue = std::move(built).takeHandle();
        if (!handleValue.has_value()) {
            return ColorResolutionOutcomeV1{
                .colorResolution = output::PngRgba8SrgbColorResolutionStateV1::Ready,
                .adapter = output::OutputAnalysisAdapterStateV1::Unavailable,
                .display = {}};
        }
        auto handle = std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(
            std::move(*handleValue));
        auto products = retainDisplayProducts(std::move(handle));
        if (!products.has_value()) {
            return std::nullopt;
        }
        return ColorResolutionOutcomeV1{.colorResolution =
                                            output::PngRgba8SrgbColorResolutionStateV1::Ready,
                                        .adapter = output::OutputAnalysisAdapterStateV1::Qualified,
                                        .display = std::move(*products)};
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

enum class BuildFailureKindV1 : std::uint8_t {
    None,
    Evaluation,
    Identity,
    Analyzer,
    AttemptBuild,
};

struct BuildOutcomeV1 final {
    bool succeeded = false;
    BuildFailureKindV1 failureKind = BuildFailureKindV1::None;
    std::uint8_t rawCode = 0;
    std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt = nullptr;
};
static_assert(runtime::TaskResultValue<BuildOutcomeV1>);

[[nodiscard]] OutputAnalysisAttemptFailureV1
translateBuildFailure(const BuildOutcomeV1& outcome) noexcept {
    switch (outcome.failureKind) {
    case BuildFailureKindV1::Evaluation:
        return {OutputAnalysisAttemptStageV1::Evaluating, false,
                static_cast<runtime::EvaluationDiagnosticCode>(outcome.rawCode)};
    case BuildFailureKindV1::Identity:
        return {OutputAnalysisAttemptStageV1::Identifying, false,
                static_cast<output::ProcessFrameSemanticIdentityErrorCode>(outcome.rawCode)};
    case BuildFailureKindV1::Analyzer:
        return {OutputAnalysisAttemptStageV1::Analyzing, false,
                static_cast<output::OutputAnalysisAnalyzerErrorCodeV1>(outcome.rawCode)};
    case BuildFailureKindV1::AttemptBuild:
        return {OutputAnalysisAttemptStageV1::Analyzing, false,
                static_cast<output::OutputAnalysisAttemptErrorCodeV1>(outcome.rawCode)};
    case BuildFailureKindV1::None:
        break;
    }
    return {OutputAnalysisAttemptStageV1::Analyzing, false, std::monostate{}};
}

[[nodiscard]] runtime::TaskOwner attemptOwner(const runtime::TaskOwner requested) noexcept {
    return requested.isValid() ? requested
                               : runtime::TaskOwner{.kind = runtime::TaskOwnerKind::Export,
                                                    .id = runtime::TaskOwnerId::fromRaw(1)};
}

} // namespace

OutputAnalysisAttemptOutcomeV1 OutputAnalysisAttemptOutcomeV1::completed(
    std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt) noexcept {
    OutputAnalysisAttemptOutcomeV1 outcome;
    outcome.attempt_ = std::move(attempt);
    return outcome;
}

OutputAnalysisAttemptOutcomeV1
OutputAnalysisAttemptOutcomeV1::failure(OutputAnalysisAttemptFailureV1 failureValue) noexcept {
    OutputAnalysisAttemptOutcomeV1 outcome;
    outcome.failure_.emplace(failureValue); // trivially copyable; std::move() would be a no-op
    return outcome;
}

namespace detail {

struct OutputAnalysisAttemptRunnerState final {
    runtime::TaskScheduler* scheduler = nullptr;
    platform::StagedArtifactCoordinator* artifacts = nullptr;
    output::ExportResourceLedgerV1* ledger = nullptr;
    OutputAnalysisAttemptRequestV1 request;
    OutputAnalysisAttemptStageV1 stage = OutputAnalysisAttemptStageV1::Resolving;
    runtime::TaskHandle<ResolvingOutcomeV1> resolvingHandle;
    std::optional<runtime::TaskHandle<BuildOutcomeV1>> buildHandle;
    bool completed = false;
};

} // namespace detail

OutputAnalysisAttemptRunnerV1::OutputAnalysisAttemptRunnerV1(
    OutputAnalysisAttemptRunnerV1&&) noexcept = default;
OutputAnalysisAttemptRunnerV1&
OutputAnalysisAttemptRunnerV1::operator=(OutputAnalysisAttemptRunnerV1&&) noexcept = default;

OutputAnalysisAttemptRunnerV1::OutputAnalysisAttemptRunnerV1(
    std::unique_ptr<detail::OutputAnalysisAttemptRunnerState> state) noexcept
    : state_(std::move(state)) {}

OutputAnalysisAttemptRunnerV1::~OutputAnalysisAttemptRunnerV1() {
    if (state_ && !state_->completed) {
        requestCancellation();
    }
}

bool OutputAnalysisAttemptRunnerV1::isReady() const noexcept {
    if (!state_ || state_->completed || state_->scheduler == nullptr) {
        return false;
    }
    const auto id =
        state_->buildHandle.has_value() ? state_->buildHandle->id() : state_->resolvingHandle.id();
    const auto snapshot = state_->scheduler->snapshot(id);
    return snapshot.has_value() && runtime::isTerminal(snapshot->state);
}

void OutputAnalysisAttemptRunnerV1::requestCancellation() noexcept {
    if (!state_) {
        return;
    }
    if (state_->buildHandle.has_value()) {
        state_->buildHandle->cancel();
    } else {
        state_->resolvingHandle.cancel();
    }
}

std::optional<OutputAnalysisAttemptOutcomeV1> OutputAnalysisAttemptRunnerV1::tryComplete() {
    if (!state_ || state_->completed) {
        return std::nullopt;
    }

    if (!state_->buildHandle.has_value()) {
        // Still waiting on / just reached the Resolving stage.
        auto taken = state_->resolvingHandle.tryTakeResult();
        if (!taken.has_value()) {
            return std::nullopt;
        }
        if (taken->state() == runtime::TaskState::Cancelled) {
            state_->completed = true;
            return OutputAnalysisAttemptOutcomeV1::failure(
                {OutputAnalysisAttemptStageV1::Resolving, true, std::monostate{}});
        }
        if (taken->state() != runtime::TaskState::Succeeded || !taken->value().has_value() ||
            !taken->value()->succeeded) {
            state_->completed = true;
            if (taken->value().has_value() && taken->value()->colorStageFailed) {
                return OutputAnalysisAttemptOutcomeV1::failure(
                    {OutputAnalysisAttemptStageV1::ColorPreparing, false, std::monostate{}});
            }
            const auto error = taken->value().has_value() ? taken->value()->error
                                                          : platform::StagedArtifactError::None;
            return OutputAnalysisAttemptOutcomeV1::failure(
                {OutputAnalysisAttemptStageV1::Resolving, false, error});
        }

        // Resolving succeeded: submit the Cpu stage, consuming its typed result (the retained
        // target, and for PNG the retained display products) directly into the next task's closure
        // -- never a wait/get/join.
        auto resolved = taken->value()->resolved;
        if (resolved == nullptr) {
            state_->completed = true;
            return OutputAnalysisAttemptOutcomeV1::failure(
                {OutputAnalysisAttemptStageV1::Resolving, false, std::monostate{}});
        }
        const auto preset = state_->request.preset;
        auto* ledger = state_->ledger;
        auto plan = state_->request.plan;
        auto evaluation = state_->request.evaluation;

        runtime::TaskRequest cpuRequest(
            "Analyze a frame export attempt", attemptOwner(state_->request.owner),
            runtime::TaskPriority::Foreground, runtime::TaskExecutor::Cpu);
        auto submission = state_->scheduler->submit<BuildOutcomeV1>(
            std::move(cpuRequest),
            [plan = std::move(plan), evaluation, resolved, preset,
             ledger](runtime::TaskContext& context) -> runtime::TaskResult<BuildOutcomeV1> {
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }

                context.reportProgress(
                    {.phase = "Evaluating", .subphase = "", .completed = 0, .total = std::nullopt});
                const runtime::CpuCompositionEvaluator evaluator;
                auto evalResult = evaluator.evaluate(plan, evaluation, context.cancellation());
                if (evalResult.status() == runtime::EvaluationStatus::Cancelled) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }
                if (evalResult.status() != runtime::EvaluationStatus::Evaluated ||
                    evalResult.frame() == nullptr) {
                    const auto code = evalResult.diagnostics().empty()
                                          ? runtime::EvaluationDiagnosticCode::InternalInvariant
                                          : evalResult.diagnostics().front().code;
                    return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                        {.succeeded = false,
                         .failureKind = BuildFailureKindV1::Evaluation,
                         .rawCode = static_cast<std::uint8_t>(code)});
                }
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }

                context.reportProgress({.phase = "Identifying",
                                        .subphase = "",
                                        .completed = 0,
                                        .total = std::nullopt});
                const output::ProcessFrameSemanticIdentityV1Preparer identityPreparer;
                auto identityResult =
                    identityPreparer.prepare(evalResult.frame(), context.cancellation());
                if (identityResult.status() ==
                    output::ProcessFrameSemanticIdentityPreparationStatus::Cancelled) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }
                if (identityResult.status() !=
                        output::ProcessFrameSemanticIdentityPreparationStatus::Prepared ||
                    identityResult.identity() == nullptr) {
                    return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                        {.succeeded = false,
                         .failureKind = BuildFailureKindV1::Identity,
                         .rawCode = static_cast<std::uint8_t>(identityResult.error())});
                }
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }

                context.reportProgress(
                    {.phase = "Analyzing", .subphase = "", .completed = 0, .total = std::nullopt});
                const output::OutputAnalysisProcessSourceV1 processSource{
                    .state = output::OutputAnalysisProcessSourceStateV1::Ready,
                    .readyIdentity = identityResult.identity(),
                    .missingDescriptor = std::nullopt};
                // The preset-specific analyzer entry points -- never a preset enum plus a union of
                // optional fields (frame-output.md: "There is no public entry point that accepts a
                // preset enum plus a union of optional fields"). The PNG input's expected OCIO
                // revision is the persisted Bloom Neutral v1 expectedRevision, required whether or
                // not resolution succeeded; when resolution DID succeed it byte-equals the revision
                // embedded in the retained canonical DisplayProcessorIdentity, which the digest
                // stage independently re-checks.
                auto analyzed =
                    preset == output::OutputPresetV1::PngRgba8SrgbV1
                        ? output::analyzePngRgba8SrgbV1(
                              {.process = processSource,
                               .expectedOcioRevision = color::kBloomNeutralV1ConfigDigest,
                               .colorResolution = resolved->color.colorResolution,
                               .adapter = resolved->color.adapter})
                        : output::analyzeFlatExrRgba32fLinRec709SceneV1({.process = processSource});
                if (!analyzed.hasReport()) {
                    return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                        {.succeeded = false,
                         .failureKind = BuildFailureKindV1::Analyzer,
                         .rawCode = static_cast<std::uint8_t>(analyzed.error())});
                }
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }

                auto buildResult = output::buildOutputAnalysisAttemptV1(
                    {.frame = evalResult.frame(),
                     .processIdentity = identityResult.identity(),
                     .report = analyzed.report(),
                     .target = resolved->target,
                     .display = resolved->color.display},
                    *ledger);
                if (!buildResult) {
                    return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                        {.succeeded = false,
                         .failureKind = BuildFailureKindV1::AttemptBuild,
                         .rawCode = static_cast<std::uint8_t>(buildResult.error())});
                }
                return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                    {.succeeded = true, .attempt = buildResult.attempt()});
            });

        if (!submission.accepted()) {
            state_->completed = true;
            return OutputAnalysisAttemptOutcomeV1::failure(
                {OutputAnalysisAttemptStageV1::Evaluating, false, submission.status});
        }
        state_->buildHandle.emplace(std::move(submission.handle));
        return std::nullopt;
    }

    auto taken = state_->buildHandle->tryTakeResult();
    if (!taken.has_value()) {
        return std::nullopt;
    }
    state_->completed = true;
    if (taken->state() == runtime::TaskState::Cancelled) {
        return OutputAnalysisAttemptOutcomeV1::failure(
            {OutputAnalysisAttemptStageV1::Evaluating, true, std::monostate{}});
    }
    if (taken->state() != runtime::TaskState::Succeeded || !taken->value().has_value() ||
        !taken->value()->succeeded || taken->value()->attempt == nullptr) {
        if (taken->value().has_value()) {
            return OutputAnalysisAttemptOutcomeV1::failure(translateBuildFailure(*taken->value()));
        }
        return OutputAnalysisAttemptOutcomeV1::failure(
            {OutputAnalysisAttemptStageV1::Analyzing, false, std::monostate{}});
    }
    return OutputAnalysisAttemptOutcomeV1::completed(taken->value()->attempt);
}

OutputAnalysisAttemptRunnerResultV1::OutputAnalysisAttemptRunnerResultV1(
    const runtime::TaskSubmissionStatus status) noexcept
    : submissionFailure_(status) {}

OutputAnalysisAttemptRunnerResultV1::OutputAnalysisAttemptRunnerResultV1(
    OutputAnalysisAttemptRunnerV1 handle) noexcept
    : handle_(std::move(handle)) {}

OutputAnalysisAttemptRunnerV1 OutputAnalysisAttemptRunnerResultV1::takeHandle() && noexcept {
    if (!handle_.has_value()) {
        std::terminate();
    }
    return std::move(*handle_);
}

OutputAnalysisAttemptRunnerResultV1 beginOutputAnalysisAttemptV1(
    runtime::TaskScheduler& scheduler, platform::StagedArtifactCoordinator& artifacts,
    output::ExportResourceLedgerV1& ledger, OutputAnalysisAttemptRequestV1 request) {
    const auto targetPath = request.targetPath;
    const auto overwritePolicy = request.overwritePolicy;
    // Direct aggregate-brace construction (never T's own implicit default constructor, which
    // std::make_unique<T>() would call with zero arguments): OutputAnalysisAttemptRequestV1 has no
    // default constructor of its own (runtime::EvaluationRequest's OperationIndex member has none
    // either), so `request` -- the one field with no usable default -- must be supplied inline
    // here rather than default-constructed then assigned.
    auto state = std::unique_ptr<detail::OutputAnalysisAttemptRunnerState>(
        new detail::OutputAnalysisAttemptRunnerState{
            .scheduler = &scheduler,
            .artifacts = &artifacts,
            .ledger = &ledger,
            .request = std::move(request),
            .stage = OutputAnalysisAttemptStageV1::Resolving,
            .resolvingHandle = {},
            .buildHandle = std::nullopt,
            .completed = false,
        });

    const auto preset = state->request.preset;
    auto* const displayProvider = state->request.displayProcessorProvider;

    runtime::TaskRequest resolvingRequest(
        "Resolve an export target", attemptOwner(state->request.owner),
        runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo);
    auto submission = scheduler.submit<ResolvingOutcomeV1>(
        std::move(resolvingRequest),
        [&artifacts, targetPath, overwritePolicy, preset, displayProvider](
            runtime::TaskContext& context) -> runtime::TaskResult<ResolvingOutcomeV1> {
            if (context.isCancellationRequested()) {
                return runtime::TaskResult<ResolvingOutcomeV1>::cancelled();
            }
            context.reportProgress(
                {.phase = "Resolving", .subphase = "", .completed = 0, .total = std::nullopt});
            auto preflightResult = artifacts.preflight({.targetPath = targetPath,
                                                        .overwritePolicy = overwritePolicy,
                                                        .expectedTarget = std::nullopt});
            if (!preflightResult) {
                return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                    {.succeeded = false, .error = preflightResult.error()});
            }
            auto target = std::move(preflightResult).takeTarget();
            const auto targetKey = target.targetKey();
            const auto observation = target.observation();
            // `target` goes out of scope here: the live platform active-target admission it holds
            // is released immediately rather than kept open for the duration of a pending artist
            // decision (see this file's own header-level rationale comment).
            ResolvedAttemptInputsV1 inputs{.target = {.targetKey = targetKey,
                                                      .observation = observation,
                                                      .targetPath = targetPath,
                                                      .overwritePolicy = overwritePolicy},
                                           .color = {}};

            // EXR has no display product and therefore never enters ColorPreparing at all.
            if (preset == output::OutputPresetV1::PngRgba8SrgbV1) {
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<ResolvingOutcomeV1>::cancelled();
                }
                context.reportProgress({.phase = "ColorPreparing",
                                        .subphase = "",
                                        .completed = 0,
                                        .total = std::nullopt});
                auto color = resolvePngDisplayProducts(displayProvider);
                if (!color.has_value()) {
                    return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                        {.succeeded = false, .colorStageFailed = true});
                }
                inputs.color = std::move(*color);
            }

            std::shared_ptr<const ResolvedAttemptInputsV1> retained;
            try {
                retained = std::make_shared<const ResolvedAttemptInputsV1>(std::move(inputs));
            } catch (const std::bad_alloc&) {
                return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                    {.succeeded = false,
                     .error = platform::StagedArtifactError::ResourceUnavailable});
            }
            return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                {.succeeded = true, .resolved = std::move(retained)});
        });

    if (!submission.accepted()) {
        return OutputAnalysisAttemptRunnerResultV1(submission.status);
    }
    state->resolvingHandle = std::move(submission.handle);
    return OutputAnalysisAttemptRunnerResultV1(OutputAnalysisAttemptRunnerV1(std::move(state)));
}

} // namespace bloom::host
