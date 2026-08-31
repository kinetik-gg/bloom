#include <bloom/host/output_analysis_attempt_runner.hpp>

#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <type_traits>
#include <utility>

namespace bloom::host {

namespace {

struct ResolvingOutcomeV1 final {
    bool succeeded = false;
    platform::StagedArtifactError error = platform::StagedArtifactError::None;
    std::shared_ptr<const output::OutputAnalysisAttemptTargetV1> target = nullptr;
};
static_assert(runtime::TaskResultValue<ResolvingOutcomeV1>);

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
            const auto error = taken->value().has_value() ? taken->value()->error
                                                          : platform::StagedArtifactError::None;
            return OutputAnalysisAttemptOutcomeV1::failure(
                {OutputAnalysisAttemptStageV1::Resolving, false, error});
        }

        // Resolving succeeded: submit the Cpu stage, consuming its typed result (the retained
        // target) directly into the next task's closure -- never a wait/get/join.
        auto resolvedTarget = taken->value()->target;
        auto* ledger = state_->ledger;
        auto plan = state_->request.plan;
        auto evaluation = state_->request.evaluation;

        runtime::TaskRequest cpuRequest(
            "Analyze a frame export attempt", attemptOwner(state_->request.owner),
            runtime::TaskPriority::Foreground, runtime::TaskExecutor::Cpu);
        auto submission = state_->scheduler->submit<BuildOutcomeV1>(
            std::move(cpuRequest),
            [plan = std::move(plan), evaluation, resolvedTarget,
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
                auto analyzed = output::analyzeFlatExrRgba32fLinRec709SceneV1(
                    {.process = {.state = output::OutputAnalysisProcessSourceStateV1::Ready,
                                 .readyIdentity = identityResult.identity(),
                                 .missingDescriptor = std::nullopt}});
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
                     .target = *resolvedTarget},
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

    runtime::TaskRequest resolvingRequest(
        "Resolve an export target", attemptOwner(state->request.owner),
        runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo);
    auto submission = scheduler.submit<ResolvingOutcomeV1>(
        std::move(resolvingRequest),
        [&artifacts, targetPath, overwritePolicy](
            runtime::TaskContext& context) -> runtime::TaskResult<ResolvingOutcomeV1> {
            if (context.isCancellationRequested()) {
                return runtime::TaskResult<ResolvingOutcomeV1>::cancelled();
            }
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
            std::shared_ptr<const output::OutputAnalysisAttemptTargetV1> retained;
            try {
                retained = std::make_shared<const output::OutputAnalysisAttemptTargetV1>(
                    output::OutputAnalysisAttemptTargetV1{.targetKey = targetKey,
                                                          .observation = observation,
                                                          .targetPath = targetPath,
                                                          .overwritePolicy = overwritePolicy});
            } catch (const std::bad_alloc&) {
                return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                    {.succeeded = false,
                     .error = platform::StagedArtifactError::ResourceUnavailable});
            }
            return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                {.succeeded = true, .target = std::move(retained)});
        });

    if (!submission.accepted()) {
        return OutputAnalysisAttemptRunnerResultV1(submission.status);
    }
    state->resolvingHandle = std::move(submission.handle);
    return OutputAnalysisAttemptRunnerResultV1(OutputAnalysisAttemptRunnerV1(std::move(state)));
}

} // namespace bloom::host
