#pragma once

#include <bloom/output/output_analysis_analyzer.hpp>
#include <bloom/output/output_analysis_attempt.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <variant>

// docs/architecture/frame-output.md "Pre-Approval Output Analysis Attempt": "The application
// controller submits each dependent stage only after consuming its predecessor's typed result; a
// worker never waits on another task or on the UI." Design decision 2 names the exact stage order
// (Resolving -> Evaluating -> Identifying -> Analyzing) and the "established mailbox idiom".
//
// This is bloom::host's binding of bloom::output's Qt-free stage functions/products
// (output_analysis_attempt.hpp) to the real bloom::runtime::TaskScheduler and
// bloom::platform::StagedArtifactCoordinator (design decision 1's module split), generalizing
// bloom/host/session_async_io.hpp's single-task AsyncSessionSave pattern to TWO chained tasks that
// cross an executor boundary the way this attempt graph's own stages do:
//   - Resolving is cheap blocking-I/O target preflight (platform::StagedArtifactCoordinator::
//     preflight()) -- submitted as one TaskExecutor::BlockingIo task. Only the plain-data outcome
//     (ArtifactTargetKey + ArtifactTargetObservation) is retained; the live
//     platform::StagedArtifactTarget handle this task obtains is deliberately let go out of scope
//     at the end of the task body (releasing the platform's active-target admission immediately)
//     rather than held open for the lifetime of a pending artist decision -- the job graph
//     revalidates via a second, real StagedArtifactCoordinator::preflight() call at publish time
//     (bloom/host/frame_export_publication.hpp), reusing the identical platform call rather than
//     inventing a way to keep the first live handle alive indefinitely. For the PNG preset this
//     same BlockingIo task additionally performs the attempt graph's step-4 color work
//     (ColorPreparing): resolving Bloom Neutral through the C2 in-process registry and reusing (or,
//     when the application published none, building) the qualified CPU display processor. It runs
//     here, not on the Cpu task, because processor preparation is the established one-time BLOCKING
//     stage (issue #97's decision 3, bloom/runtime/qualified_display_processor_provider.hpp), and
//     because it is a pure input to the analyzer, which is the very next stage.
//   - Evaluating, Identifying, and Analyzing are pure CPU work sharing one TaskExecutor::Cpu task
//     (runtime::CpuCompositionEvaluator, output::ProcessFrameSemanticIdentityV1Preparer,
//     output::analyzeFlatExrRgba32fLinRec709SceneV1, output::buildOutputAnalysisAttemptV1 in
//     sequence, each with its own between-stage cancellation checkpoint and TaskProgress phase) --
//     bundled into one task because none of the three cross an executor boundary from one another
///    and each has a hard data dependency on the one before it; "a worker never waits on another
//     task" is satisfied trivially (this task never touches a TaskHandle at all) while the
//     BlockingIo/Cpu executor split -- the concrete reason two SEPARATE tasks exist in this graph
//     -- is honored exactly. See the implementor's report for the full rationale.
//
// The controller itself (this file) never touches a runtime::CancellationToken or F1/analyzer/
// evaluator type directly inside a worker closure's cross-task synchronization; each task's own
// TaskContext is the only place a stage observes cancellation or reports progress, and the
// controller only ever calls TaskHandle<...>::tryTakeResult()/TaskScheduler::snapshot() from the
// authoring thread between submissions -- the "established mailbox idiom" the doc names,
// generalized from bloom/host/session_async_io.hpp's own single-stage use of it.
namespace bloom::host {

struct OutputAnalysisAttemptRequestV1 final {
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    runtime::EvaluationRequest evaluation;
    std::filesystem::path targetPath;
    platform::ArtifactOverwritePolicy overwritePolicy =
        platform::ArtifactOverwritePolicy::CreateOrReplace;
    runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Export, .id = {}};
    // Which closed preset contract this attempt analyzes for (issue #111). The typed preset selects
    // the analyzer entry point -- analyzePngRgba8SrgbV1 vs analyzeFlatExrRgba32fLinRec709SceneV1 --
    // and therefore whether the blocking stage additionally resolves Bloom Neutral and prepares the
    // qualified display processor at all ("EXR has no display product"). The default keeps every
    // pre-existing caller on the exact flat OpenEXR path it had before.
    output::OutputPresetV1 preset = output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;
    // Optional, non-owning: the application's already-published qualified display processor
    // (bloom/runtime/qualified_display_processor_provider.hpp -- the same one-shot startup handoff
    // the viewer's preview pipeline reads). When it is non-null AND Ready, the PNG blocking stage
    // REUSES that exact handle instead of resolving and building a second one -- the doc's
    // "preparation or EXACT REUSE of the qualified display processor". When it is null, Pending, or
    // Failed, the blocking stage resolves and builds its own; that is a real cost, never a silent
    // fallback to an unqualified transform. Must outlive the whole asynchronous operation.
    runtime::QualifiedDisplayProcessorProvider* displayProcessorProvider = nullptr;
};

enum class OutputAnalysisAttemptStageV1 : std::uint8_t {
    Resolving,
    // PNG only: bounded Bloom Neutral resolution plus preparation or exact reuse of the qualified
    // display processor (frame-output.md's attempt-graph step 4 and its ordered stage vocabulary).
    // A non-Ready color resolution or an unusable adapter is NOT reported here -- it is a truthful
    // analyzer input state that still produces a completed, non-approvable attempt. Only an
    // allocation or internal-invariant failure of this stage itself fails the attempt at it.
    ColorPreparing,
    Evaluating,
    Identifying,
    Analyzing,
};

// Mirrors bloom/host/save_publication.hpp's SavePublicationFailurePayload shape: exactly one
// alternative is meaningful per `stage()`. std::monostate covers submission refusal and
// cancellation (no per-stage diagnostic payload beyond "which stage was current").
using OutputAnalysisAttemptFailurePayloadV1 =
    std::variant<std::monostate, runtime::TaskSubmissionStatus, platform::StagedArtifactError,
                 runtime::EvaluationDiagnosticCode, output::ProcessFrameSemanticIdentityErrorCode,
                 output::OutputAnalysisAnalyzerErrorCodeV1,
                 output::OutputAnalysisAttemptErrorCodeV1>;

class OutputAnalysisAttemptFailureV1 final {
  public:
    OutputAnalysisAttemptFailureV1() = default;
    template <typename Payload>
    OutputAnalysisAttemptFailureV1(const OutputAnalysisAttemptStageV1 stage, const bool cancelled,
                                   Payload payload)
        : stage_(stage), cancelled_(cancelled), payload_(std::move(payload)) {}

    [[nodiscard]] OutputAnalysisAttemptStageV1 stage() const noexcept { return stage_; }
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }
    [[nodiscard]] const OutputAnalysisAttemptFailurePayloadV1& payload() const noexcept {
        return payload_;
    }
    template <typename Payload> [[nodiscard]] const Payload* payloadAs() const noexcept {
        return std::get_if<Payload>(&payload_);
    }

  private:
    OutputAnalysisAttemptStageV1 stage_ = OutputAnalysisAttemptStageV1::Resolving;
    bool cancelled_ = false;
    OutputAnalysisAttemptFailurePayloadV1 payload_;
};

class [[nodiscard]] OutputAnalysisAttemptOutcomeV1 final {
  public:
    [[nodiscard]] static OutputAnalysisAttemptOutcomeV1
    completed(std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt) noexcept;
    [[nodiscard]] static OutputAnalysisAttemptOutcomeV1
    failure(OutputAnalysisAttemptFailureV1 failure) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept { return attempt_ != nullptr; }
    [[nodiscard]] const std::shared_ptr<const output::OutputAnalysisAttemptV1>&
    attempt() const& noexcept {
        return attempt_;
    }
    [[nodiscard]] const std::shared_ptr<const output::OutputAnalysisAttemptV1>&
    attempt() const&& = delete;
    [[nodiscard]] const OutputAnalysisAttemptFailureV1* failure() const& noexcept {
        return failure_.has_value() ? &*failure_ : nullptr;
    }
    [[nodiscard]] const OutputAnalysisAttemptFailureV1* failure() const&& = delete;

  private:
    std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt_;
    std::optional<OutputAnalysisAttemptFailureV1> failure_;
};

namespace detail {
struct OutputAnalysisAttemptRunnerState;
}

class OutputAnalysisAttemptRunnerResultV1;

class OutputAnalysisAttemptRunnerV1 final {
  public:
    // Defined (= default) in the .cpp, where detail::OutputAnalysisAttemptRunnerState is a complete
    // type -- std::unique_ptr<Incomplete>'s move-assignment operator requires completeness to
    // destroy the previously owned object, so these cannot be defaulted inline here (the same
    // pimpl idiom bloom::platform::StagedArtifactCoordinator's own move operations already use).
    OutputAnalysisAttemptRunnerV1(OutputAnalysisAttemptRunnerV1&&) noexcept;
    OutputAnalysisAttemptRunnerV1& operator=(OutputAnalysisAttemptRunnerV1&&) noexcept;
    OutputAnalysisAttemptRunnerV1(const OutputAnalysisAttemptRunnerV1&) = delete;
    OutputAnalysisAttemptRunnerV1& operator=(const OutputAnalysisAttemptRunnerV1&) = delete;
    // Same typed-abandonment contract as ~AsyncSessionSave(): requests cancellation of whichever
    // task is currently in flight, then drops the handle. Releases the target admission the
    // Resolving task briefly held and, if the Cpu stage already built an attempt before this
    // destructor runs, its resource reservation once the last reference drops.
    ~OutputAnalysisAttemptRunnerV1();

    // Non-blocking, non-consuming poll of whichever task is currently active. See
    // bloom/host/session_async_io.hpp's identical "Readiness" documentation.
    [[nodiscard]] bool isReady() const noexcept;
    // May be called from any thread at any time, exactly like AsyncSessionSave::
    // requestCancellation().
    void requestCancellation() noexcept;
    // Advances the internal state machine: while the active task is not yet terminal, returns
    // nullopt. When Resolving completes successfully, this call submits the Cpu stage and returns
    // nullopt again (still running). Only once the LAST stage reaches a scheduler-terminal state
    // does this return the full typed outcome, exactly once.
    [[nodiscard]] std::optional<OutputAnalysisAttemptOutcomeV1> tryComplete();

  private:
    friend class OutputAnalysisAttemptRunnerResultV1;
    friend OutputAnalysisAttemptRunnerResultV1
    beginOutputAnalysisAttemptV1(runtime::TaskScheduler&, platform::StagedArtifactCoordinator&,
                                 output::ExportResourceLedgerV1&, OutputAnalysisAttemptRequestV1);

    explicit OutputAnalysisAttemptRunnerV1(
        std::unique_ptr<detail::OutputAnalysisAttemptRunnerState> state) noexcept;

    std::unique_ptr<detail::OutputAnalysisAttemptRunnerState> state_;
};

class [[nodiscard]] OutputAnalysisAttemptRunnerResultV1 final {
  public:
    OutputAnalysisAttemptRunnerResultV1(OutputAnalysisAttemptRunnerResultV1&&) noexcept = default;
    OutputAnalysisAttemptRunnerResultV1&
    operator=(OutputAnalysisAttemptRunnerResultV1&&) noexcept = default;
    OutputAnalysisAttemptRunnerResultV1(const OutputAnalysisAttemptRunnerResultV1&) = delete;
    OutputAnalysisAttemptRunnerResultV1&
    operator=(const OutputAnalysisAttemptRunnerResultV1&) = delete;
    ~OutputAnalysisAttemptRunnerResultV1() = default;

    [[nodiscard]] explicit operator bool() const noexcept { return handle_.has_value(); }
    [[nodiscard]] std::optional<runtime::TaskSubmissionStatus> submissionFailure() const noexcept {
        return submissionFailure_;
    }
    [[nodiscard]] OutputAnalysisAttemptRunnerV1 takeHandle() && noexcept;

  private:
    friend OutputAnalysisAttemptRunnerResultV1
    beginOutputAnalysisAttemptV1(runtime::TaskScheduler&, platform::StagedArtifactCoordinator&,
                                 output::ExportResourceLedgerV1&, OutputAnalysisAttemptRequestV1);

    explicit OutputAnalysisAttemptRunnerResultV1(runtime::TaskSubmissionStatus status) noexcept;
    explicit OutputAnalysisAttemptRunnerResultV1(OutputAnalysisAttemptRunnerV1 handle) noexcept;

    std::optional<runtime::TaskSubmissionStatus> submissionFailure_;
    std::optional<OutputAnalysisAttemptRunnerV1> handle_;
};

// Submits the Resolving task. `scheduler`, `artifacts`, and `ledger` must outlive the whole
// asynchronous operation (identical threading rule to AsyncSessionSave's captured references).
[[nodiscard]] OutputAnalysisAttemptRunnerResultV1 beginOutputAnalysisAttemptV1(
    runtime::TaskScheduler& scheduler, platform::StagedArtifactCoordinator& artifacts,
    output::ExportResourceLedgerV1& ledger, OutputAnalysisAttemptRequestV1 request);

} // namespace bloom::host
