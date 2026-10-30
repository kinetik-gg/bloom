// The per-output passes of ExportJobRunner (ADR 0033 section 6). The image pass is the still and
// range driver the former Qt export dialog's controller owned; the media pass wraps
// SequenceExportRunnerV1 as that controller's composition export did. The approval prompt is
// replaced by the AnalysisPolicy check against the preset's declared preservation (section 10).
#include "export_output_drivers.hpp"

#include <bloom/host/export_capacity_wait.hpp>
#include <bloom/host/frame_export_publication.hpp>
#include <bloom/host/frame_range_runner.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/output_analysis_attempt_runner.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/output/flat_exr_options.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_analysis_attempt.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <deque>
#include <variant>

namespace bloom::host::detail {
namespace {

using document::ExportFormat;
using output::OutputPresetV1;

ExportLogEntry entry(const runtime::DiagnosticSeverity severity, std::string code,
                     std::string message, const ExportOutputId output,
                     const std::optional<std::uint64_t> frame = std::nullopt) {
    return {.sequence = 0,
            .sinceStart = {},
            .severity = severity,
            .code = std::move(code),
            .output = output,
            .frame = frame,
            .message = std::move(message)};
}

ExportCheckResult check(const ExportCheck what, const ExportCheckOutcome outcome, std::string code,
                        ExportCheckValue expected = {}, ExportCheckValue observed = {},
                        std::optional<std::uint64_t> frame = std::nullopt) {
    return {what, outcome, std::move(code), std::move(expected), std::move(observed), frame};
}

ExportCheckOutcome overallOf(const std::vector<ExportCheckResult>& checks) {
    bool warning = false;
    for (const auto& result : checks) {
        if (result.outcome == ExportCheckOutcome::Failed)
            return ExportCheckOutcome::Failed;
        warning = warning || result.outcome == ExportCheckOutcome::Warning;
    }
    return warning ? ExportCheckOutcome::Warning : ExportCheckOutcome::Passed;
}

ExportPublicationOutcome
publicationOutcome(const platform::StagedArtifactPublicationOutcome value) {
    switch (value) {
    case platform::StagedArtifactPublicationOutcome::Published:
        return ExportPublicationOutcome::Published;
    case platform::StagedArtifactPublicationOutcome::PublishedWithDurabilityWarning:
        return ExportPublicationOutcome::PublishedWithDurabilityWarning;
    case platform::StagedArtifactPublicationOutcome::Superseded:
        return ExportPublicationOutcome::Superseded;
    case platform::StagedArtifactPublicationOutcome::ExternalModificationConflict:
        return ExportPublicationOutcome::ExternalModificationConflict;
    case platform::StagedArtifactPublicationOutcome::CancelledBeforePublication:
        return ExportPublicationOutcome::CancelledBeforePublication;
    case platform::StagedArtifactPublicationOutcome::FailedBeforePublication:
        return ExportPublicationOutcome::FailedBeforePublication;
    }
    return ExportPublicationOutcome::FailedBeforePublication;
}

std::optional<OutputPresetV1> imagePreset(const document::ExportOutputSettings& settings) {
    switch (settings.format) {
    case ExportFormat::Png:
        return OutputPresetV1::PngRgba8SrgbV1;
    case ExportFormat::OpenExr:
        return OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;
    case ExportFormat::Tiff:
        return OutputPresetV1::TiffRgba16SrgbV1;
    default:
        return std::nullopt;
    }
}

OutputPresetV1 mediaPreset(const ExportFormat format) {
    switch (format) {
    case ExportFormat::DnxhrMxf:
        return OutputPresetV1::DnxhrMxfV1;
    case ExportFormat::H264Mov:
        return OutputPresetV1::H264MovV1;
    case ExportFormat::H264Mp4:
        return OutputPresetV1::H264Mp4V1;
    case ExportFormat::HevcMov:
        return OutputPresetV1::HevcMovV1;
    case ExportFormat::HevcMp4:
        return OutputPresetV1::HevcMp4V1;
    case ExportFormat::Wav:
        return OutputPresetV1::PcmWavV1;
    default:
        return OutputPresetV1::ProResMovV1;
    }
}

std::optional<output::FlatExrCompressionV1> exrCompression(const std::string_view token) {
    if (token.empty() || token == "zip")
        return output::FlatExrCompressionV1::Zip;
    if (token == "piz")
        return output::FlatExrCompressionV1::Piz;
    if (token == "zips")
        return output::FlatExrCompressionV1::Zips;
    if (token == "none")
        return output::FlatExrCompressionV1::None;
    return std::nullopt;
}

ExportFailure failure(const ExportFailureCode code, std::string diagnostic, std::string reason,
                      const std::optional<std::uint64_t> frame = std::nullopt) {
    return {.code = code,
            .output = std::nullopt,
            .frame = frame,
            .diagnosticCode = std::move(diagnostic),
            .reason = std::move(reason),
            .summary = std::string(exportFailureSummary(code))};
}

// Shared bookkeeping for both passes.
class DriverBase : public ExportOutputDriver {
  protected:
    DriverBase(const ExportJobRunnerDependencies& dependencies, ExportDriverInput input)
        : dependencies_(dependencies), input_(std::move(input)) {
        report_.verification.output = input_.output.record.id;
        report_.state = ExportOutputState::Preparing;
        expected_ = exportExpectedPreservation(input_.output.record.settings);
    }

    // A frame waiting for capacity waits for the Viewer only while the Viewer is actually
    // rendering (a Viewer request waiting or admitted within the activity window); otherwise it
    // is a memory wait, after reusable caches already gave back what they could.
    [[nodiscard]] ExportDeviceRoute capacityWaitRoute() const {
        if (const auto* policy = waitPolicy()) {
            const auto viewer = policy->viewerPriority();
            if (viewer.waiting || viewer.active)
                return ExportDeviceRoute::WaitingForViewer;
        }
        return ExportDeviceRoute::WaitingForMemory;
    }
    [[nodiscard]] ExportDeviceRoute liveRoute(const ExportDeviceRoute route) const {
        return route == ExportDeviceRoute::WaitingForViewer ||
                       route == ExportDeviceRoute::WaitingForMemory
                   ? capacityWaitRoute()
                   : route;
    }
    [[nodiscard]] std::optional<std::uint64_t> memoryNeededBytes() const override {
        if (const auto* policy = waitPolicy())
            if (const auto need = policy->lastAwaitingNeed())
                return need->bytes;
        return std::nullopt;
    }
    [[nodiscard]] const runtime::GpuProcessingPhasePolicy* waitPolicy() const {
        return dependencies_.gpu ? dependencies_.gpu->processingPhasePolicy().get() : nullptr;
    }

    void log(const runtime::DiagnosticSeverity severity, std::string code, std::string message,
             std::optional<std::uint64_t> frame = std::nullopt) {
        log_.push_back(
            entry(severity, std::move(code), std::move(message), input_.output.record.id, frame));
    }

    // AnalysisPolicy: every analysed facet fits the preset's declared preservation.
    template <typename Facets, typename StateOf>
    bool acceptAnalysis(const Facets& facets, StateOf&& stateOf,
                        const std::optional<core::Sha256Digest> digest,
                        const std::optional<std::uint64_t> frame) {
        std::size_t index = 0;
        for (const auto& facet : facets) {
            if (index >= expected_.size())
                break;
            const auto [id, state] = stateOf(facet);
            if (!analysed_)
                report_.verification.facets[index] = {id, state};
            if (!exportPreservationFits(state, expected_[index])) {
                log(runtime::DiagnosticSeverity::Error, "export.check.analysis-policy",
                    std::string(output::outputFacetDisplayNameV1(id)) + " analysed as " +
                        std::to_string(static_cast<unsigned>(state)) + ", preset expects at most " +
                        std::to_string(static_cast<unsigned>(expected_[index])),
                    frame);
                fail(failure(
                    ExportFailureCode::AnalysisRefused, "export.check.analysis-policy",
                    (frame ? "Frame " + std::to_string(*frame) : std::string("This output")) +
                        " can't be exported with this preset (" +
                        std::string(output::outputFacetDisplayNameV1(id)) +
                        " would not be preserved), so the export stopped.",
                    frame));
                checks_.push_back(check(ExportCheck::AnalysisPolicy, ExportCheckOutcome::Failed,
                                        "export.check.analysis-policy.refused",
                                        static_cast<std::uint64_t>(expected_[index]),
                                        static_cast<std::uint64_t>(state), frame));
                return false;
            }
            ++index;
        }
        if (!analysed_) {
            analysed_ = true;
            report_.verification.analysisDigest = digest;
        }
        return true;
    }

    void fail(ExportFailure value) {
        if (report_.failure)
            return;
        report_.failure = std::move(value);
        report_.state = ExportOutputState::Failed;
        done_ = true;
    }

    void finishStopped() {
        report_.state = ExportOutputState::Stopped;
        done_ = true;
    }

    void finishPublished(std::vector<ExportCheckResult> checks) {
        if (!analysed_)
            checks_.push_back(check(ExportCheck::AnalysisPolicy, ExportCheckOutcome::NotRun,
                                    "export.check.analysis-policy.not-run"));
        else
            checks_.insert(checks_.begin(),
                           check(ExportCheck::AnalysisPolicy, ExportCheckOutcome::Passed,
                                 "export.check.analysis-policy.fits"));
        for (auto& value : checks)
            checks_.push_back(std::move(value));
        report_.verification.checks = std::move(checks_);
        report_.verification.overall = overallOf(report_.verification.checks);
        report_.state = report_.verification.overall == ExportCheckOutcome::Passed
                            ? ExportOutputState::Published
                            : ExportOutputState::PublishedWithWarnings;
        if (report_.verification.overall == ExportCheckOutcome::Failed) {
            report_.state = ExportOutputState::Failed;
            report_.failure =
                failure(ExportFailureCode::VerificationFailed, "export.verification.failed",
                        "The published output did not pass verification");
        }
        done_ = true;
    }

    ExportCheckResult provenanceCheck() const {
        const auto& provenance = report_.verification.provenance;
        // A video frame is a GPU frame only when its display transform ran on the GPU as well.
        const auto gpuFrames = provenance.cpuDisplayFrames > 0
                                   ? std::min(provenance.gpuFrames, provenance.gpuDisplayFrames)
                                   : provenance.gpuFrames;
        if (provenance.cpuReferenceFrames > 0 || provenance.cpuDisplayFrames > 0)
            return check(ExportCheck::DeviceProvenance, ExportCheckOutcome::Warning,
                         "export.check.device.cpu-reference",
                         provenance.gpuFrames + provenance.cpuReferenceFrames, gpuFrames);
        return check(ExportCheck::DeviceProvenance, ExportCheckOutcome::Passed,
                     "export.check.device.gpu", provenance.gpuFrames, provenance.gpuFrames);
    }

  public:
    [[nodiscard]] bool done() const override { return done_; }
    [[nodiscard]] ExportOutputState state() const override { return report_.state; }
    [[nodiscard]] std::vector<ExportLogEntry> takeLog() override { return std::exchange(log_, {}); }
    [[nodiscard]] ExportDriverReport takeReport() override {
        auto report = std::move(report_);
        for (auto& value : log_)
            report.log.push_back(std::move(value));
        log_.clear();
        if (report.verification.checks.empty() && report.state != ExportOutputState::Published)
            report.verification.checks = std::move(checks_);
        if (report.state == ExportOutputState::Failed || report.state == ExportOutputState::Stopped)
            report.verification.overall = report.state == ExportOutputState::Failed
                                              ? ExportCheckOutcome::Failed
                                              : ExportCheckOutcome::NotRun;
        return report;
    }

  protected:
    const ExportJobRunnerDependencies& dependencies_;
    ExportDriverInput input_;
    ExportDriverReport report_;
    std::vector<ExportCheckResult> checks_;
    std::array<output::OutputPreservationStateV1, output::kOutputAnalysisFacetCountV1> expected_{};
    std::vector<ExportLogEntry> log_;
    bool analysed_ = false;
    bool done_ = false;
};

// ---------------------------------------------------------------------------------------------
// Images: one attempt and one atomic publication per frame.

class ImageDriver final : public DriverBase {
  public:
    ImageDriver(const ExportJobRunnerDependencies& dependencies, ExportDriverInput input,
                const OutputPresetV1 preset)
        : DriverBase(dependencies, std::move(input)), preset_(preset), next_(input_.range.first),
          nextStart_(input_.range.first), depthLimit_(detail::sequenceExportFramesInFlightV1(
                                              dependencies.gpu.get(), std::nullopt)) {
        const auto& settings = input_.output.record.settings;
        if (preset_ == OutputPresetV1::FlatExrRgba32fLinRec709SceneV1) {
            const auto compression = exrCompression(settings.compression);
            if (!compression) {
                fail(failure(ExportFailureCode::PreflightRefused, "export.exr.compression",
                             "The EXR compression is not supported"));
                return;
            }
            exr_ = {settings.outputColorSpaceId, *compression};
        }
        const auto& availability = output::outputPresetAvailabilityV1(preset_);
        if (!availability.available) {
            fail(failure(ExportFailureCode::EncoderUnavailable, "export.preset.unavailable",
                         std::string(availability.reason)));
            return;
        }
        submitCompile();
    }

    void poll() override {
        if (done_)
            return;
        if (auto* compiling = std::get_if<CompileHandle>(&inFlight_)) {
            auto result = compiling->tryTakeResult();
            if (!result)
                return;
            inFlight_.emplace<std::monostate>();
            if (result->state() == runtime::TaskState::Cancelled || stopRequested_)
                return finishStopped();
            if (result->state() != runtime::TaskState::Succeeded || !result->value() ||
                !*result->value())
                return fail(failure(ExportFailureCode::CompileFailed, "export.compile.failed",
                                    "The composition could not be compiled for export"));
            plan_ = *result->value();
            mediaDisplay_ = std::move(*mediaDisplaySlot_);
            compiled_ = true;
            report_.state = ExportOutputState::Rendering;
        } else if (auto* preparing = std::get_if<PrepareHandle>(&inFlight_)) {
            if (auto result = preparing->tryTakeResult()) {
                inFlight_.emplace<std::monostate>();
                if (result->state() == runtime::TaskState::Cancelled || stopRequested_)
                    return stopNow();
                if (!result->value() || !*result->value() || !(*result->value())->approvable())
                    return fail(failure(ExportFailureCode::AnalysisRefused, "export.exr.colour",
                                        "The EXR output colour space or compression is "
                                        "unavailable in this project",
                                        next_));
                approve(*result->value());
                if (done_)
                    return;
            }
        } else if (auto* publishing = std::get_if<PublishHandle>(&inFlight_)) {
            if (publishing->handle.tryTakeResult()) {
                auto result = std::move(*publishing->result);
                inFlight_.emplace<std::monostate>();
                handlePublication(std::move(result));
                if (done_)
                    return;
            }
        }
        if (!compiled_)
            return;
        if (stopRequested_) {
            if (std::holds_alternative<std::monostate>(inFlight_))
                stopNow();
            return;
        }
        // Analyses ahead of the ordered publication tail.
        for (std::size_t position = 0; position < window_.size(); ++position) {
            auto& frame = window_[position];
            if (!frame.runner)
                continue;
            auto outcome = frame.runner->tryComplete();
            if (!outcome)
                continue;
            frame.runner.reset();
            ++releaseEpoch_;
            const auto refusal = outputAnalysisCapacityRefusalV1(*outcome, processingPolicy());
            if (refusal != OutputAnalysisCapacityRefusalV1::None) {
                if (othersHoldMemory(position)) {
                    // Frames of this export hold memory: keep them running and retry this
                    // frame once something has completed.
                    frame.awaitingCapacity = true;
                    frame.externalWait = false;
                    frame.refusedAtEpoch = releaseEpoch_;
                    continue;
                }
                if (refusal == OutputAnalysisCapacityRefusalV1::Wait) {
                    if (!capacity_.waiting())
                        log(runtime::DiagnosticSeverity::Information, "export.capacity.waiting",
                            describeOutputAnalysisAttemptV1(*outcome) + "; " +
                                describeProcessingReservationsV1(processingPolicy()),
                            frame.index);
                    capacity_.refused(OutputAnalysisCapacityWaitV1::Clock::now(),
                                      processingPolicy());
                    route_ = capacityWaitRoute();
                    frame.awaitingCapacity = true;
                    frame.externalWait = true;
                    frame.refusedAtEpoch = releaseEpoch_;
                    continue;
                }
                log(runtime::DiagnosticSeverity::Error, "export.capacity.never-fits",
                    describeOutputAnalysisAttemptV1(*outcome) + "; " +
                        describeProcessingReservationsV1(processingPolicy()),
                    frame.index);
                capacity_.succeeded();
                return fail(failure(ExportFailureCode::ResourceRefused,
                                    "export.capacity.never-fits",
                                    "Frame " + std::to_string(frame.index) +
                                        " needs more memory than this computer can give the "
                                        "export, so the export stopped. Try a smaller "
                                        "resolution; the details are in the log.",
                                    frame.index));
            }
            frame.outcome.emplace(std::move(*outcome));
        }
        if (pendingRepublish_ && std::holds_alternative<std::monostate>(inFlight_)) {
            // Republish once no frame ahead is analysing or holding an analysed attempt.
            releaseAnalysedFrames();
            if (runningAnalyses() != 0)
                return;
            approve(std::exchange(pendingRepublish_, nullptr));
            return;
        }
        // The head frame enters the serial tail (EXR colour preparation, then publication).
        if (std::holds_alternative<std::monostate>(inFlight_) && !window_.empty() &&
            window_.front().outcome) {
            auto outcome = std::move(*window_.front().outcome);
            target_ = window_.front().target;
            window_.pop_front();
            handleAttempt(std::move(outcome));
            if (done_)
                return;
        }
        if (next_ > input_.range.last && std::holds_alternative<std::monostate>(inFlight_) &&
            window_.empty())
            return verify();
        waitForCapacity();
        if (done_)
            return;
        fill();
    }

    void stop() override {
        stopRequested_ = true;
        for (auto& frame : window_)
            if (frame.runner)
                frame.runner->requestCancellation();
        if (auto* compiling = std::get_if<CompileHandle>(&inFlight_))
            compiling->cancel();
        else if (auto* preparing = std::get_if<PrepareHandle>(&inFlight_))
            preparing->cancel();
        else if (auto* publishing = std::get_if<PublishHandle>(&inFlight_))
            publishing->handle.cancel();
        else if (!done_) {
            capacity_.succeeded();
            stopNow();
        }
    }

    void setPaused(const bool paused) override { pauseRequested_ = paused; }
    [[nodiscard]] bool pausedAtBoundary() const override {
        return pauseRequested_ && compiled_ && !done_ &&
               std::holds_alternative<std::monostate>(inFlight_) && window_.empty();
    }
    [[nodiscard]] std::uint64_t framesCompleted() const override { return published_; }
    [[nodiscard]] std::optional<std::uint64_t> currentFrame() const override {
        return done_ ? std::nullopt : std::optional(next_);
    }
    [[nodiscard]] ExportDeviceRoute route() const override { return liveRoute(route_); }

  private:
    using CompileHandle =
        runtime::TaskHandle<std::shared_ptr<const runtime::CompiledCompositionPlan>>;
    using PrepareHandle =
        runtime::TaskHandle<std::shared_ptr<const output::OutputAnalysisAttemptV1>>;
    struct PublishHandle final {
        runtime::TaskHandle<void> handle;
        std::shared_ptr<std::optional<FrameExportPublicationResultV1>> result;
    };
    // One frame analysed ahead of the ordered publication tail.
    struct WindowFrame final {
        std::uint64_t index = 0;
        std::filesystem::path target;
        std::optional<OutputAnalysisAttemptRunnerV1> runner;
        std::optional<OutputAnalysisAttemptOutcomeV1> outcome;
        bool awaitingCapacity = false;
        std::uint64_t refusedAtEpoch = 0;
        // Refused while nothing of this export held memory: retried on the capacity backoff.
        bool externalWait = false;
    };

    void stopNow() {
        for (auto& frame : window_)
            if (frame.runner)
                frame.runner->requestCancellation();
        window_.clear();
        finishStopped();
    }

    // Whether anything besides window_[position] retains processing memory.
    [[nodiscard]] bool othersHoldMemory(const std::size_t position) const {
        if (!std::holds_alternative<std::monostate>(inFlight_))
            return true;
        for (std::size_t index = 0; index < window_.size(); ++index)
            if (index != position && (window_[index].runner || window_[index].outcome))
                return true;
        return false;
    }

    [[nodiscard]] std::size_t runningAnalyses() const {
        return static_cast<std::size_t>(std::ranges::count_if(
            window_, [](const auto& frame) { return frame.runner.has_value(); }));
    }

    void submitCompile() {
        runtime::TaskRequest request("Compile composition for export", input_.owner,
                                     runtime::TaskPriority::Foreground, runtime::TaskExecutor::Cpu);
        // TIFF sequences also prepare their display once here, off the authoring thread, so every
        // frame attempt can run its output colour on the GPU.
        const bool tiff = preset_ == OutputPresetV1::TiffRgba16SrgbV1;
        auto submission = dependencies_.scheduler.submit<
            std::shared_ptr<const runtime::CompiledCompositionPlan>>(
            std::move(request),
            [snapshot = input_.snapshot, composition = input_.composition,
             &compiler = dependencies_.compiler, tiff, intent = input_.colorIntent,
             slot = mediaDisplaySlot_](runtime::TaskContext& context) {
                using Result =
                    runtime::TaskResult<std::shared_ptr<const runtime::CompiledCompositionPlan>>;
                if (context.isCancellationRequested())
                    return Result::cancelled();
                context.reportProgress({"Compiling composition", "", 0, std::nullopt});
                auto compiled = compiler.compile(
                    {.snapshot = snapshot, .compositionId = composition}, context.cancellation());
                if (compiled.status == runtime::SnapshotCompileStatus::Cancelled)
                    return Result::cancelled();
                if (compiled.status != runtime::SnapshotCompileStatus::Compiled ||
                    compiled.plan == nullptr)
                    return Result::failed(runtime::TaskDiagnostic{
                        .code = "bloom.host.export.compile-failed",
                        .severity = runtime::DiagnosticSeverity::Error,
                        .summary = "The composition could not be compiled for export",
                        .detail = {},
                        .suggestedAction = "Review the composition graph."});
                if (tiff) {
                    if (auto display = output::PreparedOutputDisplayV1::prepare(intent))
                        *slot = std::make_shared<OutputMediaDisplayCommandV1>(std::move(display),
                                                                              intent);
                }
                return Result::succeeded(compiled.plan);
            });
        if (!submission.accepted())
            return fail(failure(ExportFailureCode::ResourceRefused, "export.task.refused",
                                "The export could not be scheduled"));
        inFlight_.emplace<CompileHandle>(std::move(submission.handle));
    }

    // Keeps up to depthLimit_ frames analysing ahead of the publication tail, in frame order, and
    // retries the earliest refused frame once something completed since its refusal.
    void fill() {
        // Issue #396: no frame starts before the GPU provider's bootstrap is terminal. Until then
        // the evaluator and the shared OCIO context are unpublished, and a frame would be
        // evaluated and colour-transformed on the CPU reference path on a GPU-capable machine.
        if (dependencies_.gpu != nullptr && dependencies_.gpu->bootstrapPending())
            return;
        for (auto& frame : window_) {
            if (pendingRepublish_)
                break;
            if (!frame.awaitingCapacity)
                continue;
            const bool due = frame.externalWait
                                 ? capacity_.retryDue(OutputAnalysisCapacityWaitV1::Clock::now())
                                 : frame.refusedAtEpoch != releaseEpoch_;
            if (due && !startAttempt(frame))
                return;
            break;
        }
        // The frame in the publication tail counts: a depth of one is one frame at a time.
        const auto tailFrames = std::holds_alternative<std::monostate>(inFlight_) ? 0U : 1U;
        while (!pauseRequested_ && !pendingRepublish_ && nextStart_ <= input_.range.last &&
               window_.size() + tailFrames < depthLimit_) {
            // Frames that publish to the same file (a name pattern without a frame number) stay
            // one at a time: a newer attempt's publication intent supersedes an older one.
            if (const auto path = exportFramePath(input_.job, input_.output, nextStart_);
                path && ((!std::holds_alternative<std::monostate>(inFlight_) && *path == target_) ||
                         std::ranges::any_of(window_, [&path](const auto& frame) {
                             return frame.target == *path;
                         })))
                break;
            window_.emplace_back();
            window_.back().index = nextStart_++;
            if (!startAttempt(window_.back()))
                return;
        }
        resolveStall();
    }

    // Nothing runs and the tail is idle while the earliest unanalysed frame waits for capacity:
    // only analysed later frames hold memory and they cannot publish first. Drop them (they
    // analyse again) and retry; with nothing left to drop the refusal stops the export.
    void resolveStall() {
        if (done_ || !std::holds_alternative<std::monostate>(inFlight_) || runningAnalyses() != 0)
            return;
        const auto waiting =
            std::ranges::find_if(window_, [](const auto& frame) { return frame.awaitingCapacity; });
        if (waiting == window_.end() || waiting->externalWait ||
            waiting->refusedAtEpoch != releaseEpoch_)
            return;
        bool released = false;
        for (auto later = std::next(waiting); later != window_.end(); ++later) {
            if (later->outcome) {
                later->outcome.reset();
                later->awaitingCapacity = true;
                later->refusedAtEpoch = releaseEpoch_;
                released = true;
            }
        }
        if (released) {
            ++releaseEpoch_;
            fill();
            return;
        }
        const auto index = waiting->index;
        log(runtime::DiagnosticSeverity::Error, "export.attempt.detail",
            "processing admission refused with nothing else in flight", index);
        fail(failure(ExportFailureCode::ResourceRefused, "export.attempt.failed",
                     "The export couldn't reserve its processing memory, so it stopped.", index));
    }

    bool startAttempt(WindowFrame& frame) {
        const auto* composition = input_.snapshot.project().findComposition(input_.composition);
        const FrameRangeRequestV1 frames{.destination = input_.output.destination.basePath,
                                         .firstFrame = input_.range.first,
                                         .lastFrame = input_.range.last,
                                         .frameRate = composition->format().frameRate(),
                                         .duration = composition->duration()};
        const auto time = FrameRangeRunnerV1::timeForFrame(frames, frame.index);
        const auto path = exportFramePath(input_.job, input_.output, frame.index);
        if (!time || !path) {
            fail(failure(ExportFailureCode::Internal, "export.frame.time",
                         "This frame has no exact time or file name", frame.index));
            return false;
        }
        frame.target = *path;
        OutputAnalysisAttemptRequestV1 request{
            .plan = plan_,
            .evaluation = {.time = *time,
                           .output = plan_->output(),
                           .resolution = runtime::CompositionFormatResolution{},
                           .quality = runtime::EvaluationQuality::Reference,
                           .colorIntent = input_.colorIntent,
                           .pixelStorageByteLimit = 0,
                           .bypassLookNodes = !input_.output.record.settings.bakeLook},
            .assetBaseDirectory = input_.assetBaseDirectory,
            .targetPath = frame.target,
            .overwritePolicy = input_.output.refuseExisting
                                   ? platform::ArtifactOverwritePolicy::CreateOnly
                                   : platform::ArtifactOverwritePolicy::CreateOrReplace,
            .owner = input_.owner,
            .preset = preset_,
            .displayProcessorProvider = dependencies_.displayProcessor,
            .gpuProvider = dependencies_.gpu,
            .mediaDisplay = mediaDisplay_,
            .useRootPixelCapacity = true};
        auto begin = beginOutputAnalysisAttemptV1(dependencies_.scheduler, dependencies_.artifacts,
                                                  *dependencies_.ledger, std::move(request));
        if (!begin) {
            fail(failure(ExportFailureCode::ResourceRefused, "export.attempt.refused",
                         "The frame analysis could not be started", frame.index));
            return false;
        }
        frame.runner.emplace(std::move(begin).takeHandle());
        frame.awaitingCapacity = false;
        frame.externalWait = false;
        return true;
    }

    [[nodiscard]] const runtime::GpuProcessingPhasePolicy* processingPolicy() const {
        return dependencies_.gpu ? dependencies_.gpu->processingPhasePolicy().get() : nullptr;
    }

    // ADR 0033 section 9: a frame refused while nothing of this export held memory waits at its
    // boundary (route WaitingForViewer, no ETA) and is retried with bounded backoff; it stays
    // stoppable and pausable. Pausing drops the waiting frames so the pause reaches its boundary.
    void waitForCapacity() {
        const auto waiting =
            std::ranges::find_if(window_, [](const auto& frame) { return frame.awaitingCapacity; });
        if (waiting == window_.end())
            return;
        if (pauseRequested_) {
            nextStart_ = waiting->index;
            for (auto later = waiting; later != window_.end(); ++later)
                if (later->runner)
                    later->runner->requestCancellation();
            window_.erase(waiting, window_.end());
            capacity_.succeeded();
            return;
        }
        if (!waiting->externalWait)
            return;
        const auto now = OutputAnalysisCapacityWaitV1::Clock::now();
        if (capacity_.stalled(now, processingPolicy())) {
            const auto index = waiting->index;
            log(runtime::DiagnosticSeverity::Error, "export.capacity.stalled",
                "No processing phase was admitted for " +
                    std::to_string(OutputAnalysisCapacityWaitV1::kStallAfter.count()) +
                    " s while waiting; " + describeProcessingReservationsV1(processingPolicy()),
                index);
            capacity_.succeeded();
            fail(failure(ExportFailureCode::ResourceRefused, "export.capacity.stalled",
                         "Bloom waited for memory to export frame " + std::to_string(index) +
                             ", but other work kept holding it, so the export stopped. "
                             "Close a RAM preview or other exports and try again; the "
                             "details are in the log.",
                         index));
        }
    }

    void handleAttempt(OutputAnalysisAttemptOutcomeV1 outcome) {
        if (!stopRequested_) {
            switch (outputAnalysisCapacityRefusalV1(outcome, processingPolicy())) {
            case OutputAnalysisCapacityRefusalV1::Wait:
                if (!capacity_.waiting())
                    log(runtime::DiagnosticSeverity::Information, "export.capacity.waiting",
                        describeOutputAnalysisAttemptV1(outcome) + "; " +
                            describeProcessingReservationsV1(processingPolicy()),
                        next_);
                capacity_.refused(OutputAnalysisCapacityWaitV1::Clock::now(), processingPolicy());
                route_ = capacityWaitRoute();
                return;
            case OutputAnalysisCapacityRefusalV1::NeverFits:
                log(runtime::DiagnosticSeverity::Error, "export.capacity.never-fits",
                    describeOutputAnalysisAttemptV1(outcome) + "; " +
                        describeProcessingReservationsV1(processingPolicy()),
                    next_);
                capacity_.succeeded();
                return fail(failure(ExportFailureCode::ResourceRefused,
                                    "export.capacity.never-fits",
                                    "Frame " + std::to_string(next_) +
                                        " needs more memory than this computer can give the "
                                        "export, so the export stopped. Try a smaller "
                                        "resolution; the details are in the log.",
                                    next_));
            case OutputAnalysisCapacityRefusalV1::None:
                capacity_.succeeded();
                break;
            }
        }
        if (const auto& provenance = outcome.gpuProvenance();
            provenance && provenance->gpuEvaluated()) {
            auto& counters = report_.verification.provenance;
            ++counters.gpuFrames;
            counters.gpuNativeDispatches += provenance->counters.nativeDispatches;
            counters.gpuReadbacks += provenance->counters.readbacks;
            counters.gpuReadbackSubmissions += provenance->readbackSubmissions;
            counters.gpuTransferredPayloads += provenance->transferredPayloads;
            counters.gpuProcessPayloadBytes += provenance->processPayloadBytes;
            counters.gpuEncodedPayloadBytes += provenance->encodedPayloadBytes;
            if (provenance->deviceOwnershipEpoch != 0)
                counters.gpuDeviceOwnershipEpoch = provenance->deviceOwnershipEpoch;
            route_ = ExportDeviceRoute::Gpu;
        } else if (outcome) {
            ++report_.verification.provenance.cpuReferenceFrames;
            route_ = ExportDeviceRoute::CpuReference;
            if (report_.verification.provenance.cpuReferenceFrames == 1)
                log(runtime::DiagnosticSeverity::Warning, "export.device.cpu-reference",
                    "This frame was evaluated on the CPU reference path", next_);
        }
        if (!outcome) {
            const auto* attemptFailure = outcome.failure();
            if ((attemptFailure && attemptFailure->cancelled()) || stopRequested_)
                return finishStopped();
            auto code = ExportFailureCode::EvaluationFailed;
            const auto frameText = std::to_string(next_);
            log(runtime::DiagnosticSeverity::Error, "export.attempt.detail",
                describeOutputAnalysisAttemptV1(outcome), next_);
            std::string reason =
                attemptFailure &&
                        attemptFailure->stage() == OutputAnalysisAttemptStageV1::Evaluating
                    ? "Bloom couldn't render frame " + frameText +
                          ", so the export stopped. The details are in the log."
                    : "Bloom couldn't check frame " + frameText +
                          " before exporting, so the export stopped. This is a bug in Bloom; the "
                          "details are in the log.";
            if (attemptFailure) {
                if (const auto* phase =
                        attemptFailure->payloadAs<runtime::GpuProcessFrameDiagnosticCode>();
                    phase && (*phase == runtime::GpuProcessFrameDiagnosticCode::BudgetRefused ||
                              *phase == runtime::GpuProcessFrameDiagnosticCode::AwaitingCapacity)) {
                    code = ExportFailureCode::ResourceRefused;
                    reason = "The export couldn't reserve its processing memory, so it stopped.";
                }
                if (const auto* resource =
                        attemptFailure->payloadAs<output::OutputAnalysisAttemptErrorCodeV1>();
                    resource &&
                    *resource ==
                        output::OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed) {
                    code = ExportFailureCode::ResourceRefused;
                    reason = "The export couldn't reserve enough memory, so it stopped.";
                }
            }
            return fail(failure(code, "export.attempt.failed", std::move(reason), next_));
        }
        if (stopRequested_)
            return finishStopped();
        auto attempt = outcome.attempt();
        if (!attempt || !attempt->approvable() || !attempt->digest() || !attempt->report()) {
            log(runtime::DiagnosticSeverity::Error, "export.attempt.detail",
                describeOutputAnalysisAttemptV1(outcome), next_);
            return fail(failure(ExportFailureCode::AnalysisRefused,
                                "export.analysis.not-approvable",
                                "Bloom couldn't check frame " + std::to_string(next_) +
                                    " before exporting, so the export stopped. This is a bug in "
                                    "Bloom; the details are in the log.",
                                next_));
        }
        if (!acceptAnalysis(
                attempt->report()->view().facets,
                [](const auto& facet) { return std::pair{facet.facet, facet.state}; },
                attempt->digest(), next_))
            return;
        if (preset_ == OutputPresetV1::FlatExrRgba32fLinRec709SceneV1 &&
            (!exr_.outputColorSpaceId.empty() ||
             exr_.compression != output::FlatExrCompressionV1::Zip))
            return prepareExr(std::move(attempt));
        approve(attempt);
    }

    // COLOR-5 deliverable colour preparation (formerly in the Qt export dialog's controller).
    void prepareExr(std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt) {
        auto submission =
            dependencies_.scheduler.submit<std::shared_ptr<const output::OutputAnalysisAttemptV1>>(
                runtime::TaskRequest("Prepare EXR deliverable", input_.owner,
                                     runtime::TaskPriority::Foreground,
                                     runtime::TaskExecutor::BlockingIo),
                [attempt = std::move(attempt), options = exr_,
                 ledger = dependencies_.ledger](runtime::TaskContext& context) {
                    using Result =
                        runtime::TaskResult<std::shared_ptr<const output::OutputAnalysisAttemptV1>>;
                    if (context.isCancellationRequested())
                        return Result::cancelled();
                    context.reportProgress(
                        {"ColorPreparing", "EXR output colour", 0, std::nullopt});
                    auto result = output::prepareFlatExrAttemptV1(*attempt, options, *ledger);
                    if (context.isCancellationRequested())
                        return Result::cancelled();
                    return Result::succeeded(result ? result.attempt() : nullptr);
                });
        if (!submission.accepted())
            return fail(failure(ExportFailureCode::ResourceRefused, "export.task.refused",
                                "EXR colour preparation could not be started", next_));
        inFlight_.emplace<PrepareHandle>(std::move(submission.handle));
    }

    // Analysed frames waiting behind the tail give their retained memory back and analyse again.
    void releaseAnalysedFrames() {
        for (auto& frame : window_) {
            if (frame.outcome) {
                frame.outcome.reset();
                frame.awaitingCapacity = true;
                frame.externalWait = false;
                frame.refusedAtEpoch = releaseEpoch_ - 1;
            }
        }
    }

    void approve(const std::shared_ptr<const output::OutputAnalysisAttemptV1>& attempt) {
        tailAttempt_ = attempt;
        const auto digest = attempt->digest();
        if (!digest)
            return fail(failure(ExportFailureCode::AnalysisRefused, "export.analysis.digest",
                                "Bloom couldn't check frame " + std::to_string(next_) +
                                    " before exporting, so the export stopped. This is a bug in "
                                    "Bloom; the details are in the log.",
                                next_));
        auto approval = approveFrameExportV1(dependencies_.publications, attempt, *digest);
        if (!approval)
            return fail(failure(ExportFailureCode::AnalysisRefused, "export.approval.refused",
                                "The export could not be approved", next_));
        auto shared = std::make_shared<std::optional<FrameExportPublicationResultV1>>();
        auto request = std::shared_ptr<FrameExportRequestV1>(std::move(approval).takeRequest());
        auto submission = dependencies_.scheduler.submit<void>(
            runtime::TaskRequest("Publish exported frame", input_.owner,
                                 runtime::TaskPriority::Foreground,
                                 runtime::TaskExecutor::BlockingIo),
            [&artifacts = dependencies_.artifacts, request, shared,
             scratch = dependencies_.scratchDirectory](runtime::TaskContext& context) {
                output::OutputExportProgressCallbackV1 progress =
                    [&context](const output::OutputExportProgressV1& update) {
                        context.reportProgress({"Publishing", "", update.completed,
                                                update.total == 0
                                                    ? std::nullopt
                                                    : std::optional<std::uint64_t>(update.total)});
                    };
                shared->emplace(executeExportPublication(
                    context, artifacts, std::move(*request), scratch,
                    output::systemOutputExportClockV1(), std::move(progress)));
                return runtime::TaskResult<void>::succeeded();
            });
        if (!submission.accepted())
            return fail(failure(ExportFailureCode::ResourceRefused, "export.task.refused",
                                "The frame publication could not be scheduled", next_));
        inFlight_.emplace<PublishHandle>(PublishHandle{std::move(submission.handle), shared});
    }

    void handlePublication(std::optional<FrameExportPublicationResultV1> result) {
        if (!result)
            return stopRequested_
                       ? finishStopped()
                       : fail(failure(ExportFailureCode::Internal, "export.publication.missing",
                                      "The publication ended without a result", next_));
        const auto* publication = result->publication();
        const auto outcome =
            publication ? publication->outcome
                        : platform::StagedArtifactPublicationOutcome::FailedBeforePublication;
        worst_ = std::max(worst_, publicationOutcome(outcome));
        if (!publication || !publication->targetWasPublished()) {
            if (outcome == platform::StagedArtifactPublicationOutcome::CancelledBeforePublication ||
                (result->failure() &&
                 result->failure()->stage() == FrameExportPublicationStageV1::Cancelled) ||
                stopRequested_)
                return finishStopped();
            if (outcome == platform::StagedArtifactPublicationOutcome::ExternalModificationConflict)
                return fail(
                    failure(ExportFailureCode::PublicationConflict, "export.publication.conflict",
                            "The destination changed outside Bloom; it was not replaced", next_));
            // Frames analysed ahead hold the memory this publication's scratch needs: continue
            // one frame at a time and publish the same approved frame again once they released.
            if (result->failure() &&
                result->failure()->payloadAs<FrameExportResourceExhaustedV1>() && tailAttempt_ &&
                !republishing_ && std::ranges::any_of(window_, [](const auto& frame) {
                    return frame.runner.has_value() || frame.outcome.has_value();
                })) {
                depthLimit_ = 1;
                republishing_ = true;
                pendingRepublish_ = tailAttempt_;
                releaseAnalysedFrames();
                return;
            }
            auto code = ExportFailureCode::WriteFailed;
            std::string reason = "The frame could not be published; earlier frames are unaffected";
            if (result->failure()) {
                if (result->failure()->payloadAs<FrameExportDeadlineExceededV1>() ||
                    result->failure()->payloadAs<FrameExportNoProgressExceededV1>())
                    code = ExportFailureCode::DeadlineExceeded;
                if (result->failure()->payloadAs<FrameExportResourceExhaustedV1>())
                    code = ExportFailureCode::ResourceRefused;
            }
            return fail(failure(code, "export.publication.failed", std::move(reason), next_));
        }
        auto& artifacts = report_.verification.artifacts;
        if (published_ == 0)
            artifacts.firstPath = target_;
        artifacts.lastPath = target_;
        artifacts.totalBytes += result->artifactByteCount();
        if (const auto digest = result->artifactDigest();
            !digest || !digests_.update(std::as_bytes(std::span(digest->bytes()))))
            missingDigest_ = true;
        if (published_ == 0 || target_ != report_.published.back()) {
            if (report_.published.size() < 2)
                report_.published.push_back(target_);
            else
                report_.published.back() = target_;
        }
        ++published_;
        ++next_;
        ++releaseEpoch_;
        tailAttempt_.reset();
        republishing_ = false;
    }

    void verify() {
        auto& artifacts = report_.verification.artifacts;
        artifacts.publishedArtifacts = published_;
        artifacts.publication = worst_;
        if (!missingDigest_)
            artifacts.digest = digests_.finalize();
        const auto expected = input_.range.last - input_.range.first + 1;
        std::vector<ExportCheckResult> checks;
        checks.push_back(check(ExportCheck::Reopen, ExportCheckOutcome::Passed,
                               "export.check.reopen.verified", expected, published_));
        checks.push_back(
            check(ExportCheck::FrameCount,
                  published_ == expected ? ExportCheckOutcome::Passed : ExportCheckOutcome::Failed,
                  "export.check.frame-count", expected, published_));
        for (const auto what : {ExportCheck::Timing, ExportCheck::ColourTags,
                                ExportCheck::Container, ExportCheck::Audio})
            checks.push_back(check(what, ExportCheckOutcome::NotApplicable, "export.check.image"));
        checks.push_back(
            check(ExportCheck::Naming,
                  expected > 1 ? ExportCheckOutcome::Passed : ExportCheckOutcome::NotApplicable,
                  "export.check.naming.ascending"));
        checks.push_back(
            check(ExportCheck::ArtifactDigest,
                  missingDigest_ ? ExportCheckOutcome::Failed : ExportCheckOutcome::Passed,
                  "export.check.artifact-digest"));
        checks.push_back(check(ExportCheck::Publication,
                               worst_ == ExportPublicationOutcome::Published
                                   ? ExportCheckOutcome::Passed
                                   : ExportCheckOutcome::Warning,
                               "export.check.publication"));
        checks.push_back(provenanceCheck());
        finishPublished(std::move(checks));
    }

    OutputPresetV1 preset_;
    output::FlatExrRgba32fOptionsV1 exr_;
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan_;
    std::uint64_t next_ = 0;
    std::uint64_t published_ = 0;
    std::filesystem::path target_;
    core::Sha256Hasher digests_;
    bool missingDigest_ = false;
    ExportPublicationOutcome worst_ = ExportPublicationOutcome::Published;
    ExportDeviceRoute route_ = ExportDeviceRoute::None;
    bool stopRequested_ = false;
    bool pauseRequested_ = false;
    bool compiled_ = false;
    // The compile, then the ordered tail of the head frame: EXR colour preparation, publication.
    std::variant<std::monostate, CompileHandle, PrepareHandle, PublishHandle> inFlight_;
    std::deque<WindowFrame> window_;
    std::uint64_t nextStart_ = 0;
    std::size_t depthLimit_ = 1;
    std::uint64_t releaseEpoch_ = 0;
    // The frame in the publication tail, kept so a publication refused for memory held by frames
    // analysed ahead can be published again once they released it.
    std::shared_ptr<const output::OutputAnalysisAttemptV1> tailAttempt_;
    std::shared_ptr<const output::OutputAnalysisAttemptV1> pendingRepublish_;
    bool republishing_ = false;
    std::shared_ptr<OutputMediaDisplayCommandV1> mediaDisplay_;
    std::shared_ptr<std::shared_ptr<OutputMediaDisplayCommandV1>> mediaDisplaySlot_ =
        std::make_shared<std::shared_ptr<OutputMediaDisplayCommandV1>>();
    OutputAnalysisCapacityWaitV1 capacity_;
};

// ---------------------------------------------------------------------------------------------
// Movies and WAV: SequenceExportRunnerV1 (one compile, one acknowledged frame in flight, one
// publication).

std::string mediaProfile(const document::ExportOutputSettings& settings) {
    switch (settings.format) {
    case ExportFormat::ProResMov:
        return settings.codecProfile.empty() ? "hq" : settings.codecProfile;
    case ExportFormat::DnxhrMxf:
        return "dnxhr_" +
               (settings.codecProfile.empty() ? std::string("hq") : settings.codecProfile);
    case ExportFormat::H264Mov:
    case ExportFormat::H264Mp4:
        return "high";
    case ExportFormat::HevcMov:
    case ExportFormat::HevcMp4:
        return settings.codecProfile.empty() ? "main" : settings.codecProfile;
    case ExportFormat::Wav:
        return settings.codecProfile == "s16" ? "pcm_s16le" : "pcm_s24le";
    default:
        return {};
    }
}

class MediaDriver final : public DriverBase {
  public:
    MediaDriver(const ExportJobRunnerDependencies& dependencies, ExportDriverInput input)
        : DriverBase(dependencies, std::move(input)) {
        const auto& settings = input_.output.record.settings;
        const auto preset = mediaPreset(settings.format);
        const auto availability = output::outputPresetAvailabilityV1(preset);
        if (!availability.available) {
            fail(failure(ExportFailureCode::EncoderUnavailable, "export.preset.unavailable",
                         std::string(availability.reason)));
            return;
        }
        if (!dependencies_.retirement) {
            fail(failure(ExportFailureCode::Internal, "export.retirement.missing",
                         "Movie export needs the encoder retirement owner"));
            return;
        }
        const auto* composition = input_.snapshot.project().findComposition(input_.composition);
        const bool h264 =
            settings.format == ExportFormat::H264Mov || settings.format == ExportFormat::H264Mp4;
        const bool hevc =
            settings.format == ExportFormat::HevcMov || settings.format == ExportFormat::HevcMp4;
        const auto& encoders = input_.encoders;
        const bool softwareH264 = encoders.openh264 == ExportEncoderStatus::OpenH264::Installed;
        hardware_ =
            hevc || (h264 && (settings.encoder == document::ExportEncoderPreference::Hardware ||
                              (settings.encoder == document::ExportEncoderPreference::Automatic &&
                               !softwareH264 && encoders.vaapiH264)));
        const auto profile = mediaProfile(settings);
        SequenceExportRequestV1 request{
            .composition = {input_.snapshot, input_.composition},
            .range = {.destination = input_.output.destination.firstPath,
                      .firstFrame = input_.range.first,
                      .lastFrame = input_.range.last,
                      .frameRate = composition->format().frameRate(),
                      .duration = composition->duration()},
            .preset = preset,
            .profile = settings.format == ExportFormat::Wav ? std::string() : profile,
            .audio = settings.format == ExportFormat::Wav || settings.includeAudio,
            .hardware = hardware_,
            .openh264Consent = false,
            .sampleRate = settings.audioSampleRate,
            .pcmCodec = settings.format == ExportFormat::Wav ? profile : "pcm_s16le",
            .bwfDescription = {},
            .assetBaseDirectory = input_.assetBaseDirectory,
            .worker = {},
            .startFrame = settings.naming.startFrame,
            .framePadding = settings.naming.framePadding,
            .namePattern = settings.naming.pattern,
            .workingColorSpaceId = std::string(input_.colorIntent.workingColorSpaceId),
            .ocioConfigRevision = input_.colorIntent.ocioConfigRevision,
            .ocioConfigUri = std::string(input_.colorIntent.ocioConfigUri)};
        request.displayName = settings.displayName;
        request.viewName = settings.viewName;
        request.bypassLookNodes = !settings.bakeLook;
        request.videoBitrateKbps = settings.videoBitrateKbps;
        request.vaapiDevice = encoders.vaapiRenderNode;
        const bool mp4 =
            settings.format == ExportFormat::H264Mp4 || settings.format == ExportFormat::HevcMp4;
        // MP4 carries AAC audio; MOV and MXF carry PCM.
        if (mp4)
            request.pcmCodec = "aac";
        if (input_.output.artifactByteLimit)
            request.artifactByteLimit = input_.output.artifactByteLimit;
        request.gpuProvider = dependencies_.gpu;
        request.owner = input_.owner;
        request.useCachedFrames = settings.useCachedFrames;
        runner_ = std::make_unique<SequenceExportRunnerV1>(
            dependencies_.scheduler, dependencies_.compiler, dependencies_.publications,
            dependencies_.artifacts, *dependencies_.ledger, std::move(request),
            *dependencies_.retirement, dependencies_.ledger);
        report_.state = ExportOutputState::Rendering;
    }

    void poll() override {
        if (done_ || !runner_)
            return;
        runner_->poll();
        if (runner_->stage() == SequenceExportStageV1::AwaitingApproval && !approvalAsked_) {
            approvalAsked_ = true;
            const auto* analysis = runner_->analysis();
            const auto frame = runner_->frameApprovalDigest();
            if (!analysis || !frame) {
                runner_->cancel();
                return;
            }
            if (!acceptAnalysis(
                    analysis->facets,
                    [](const auto& facet) { return std::pair{facet.facet, facet.preservation}; },
                    analysis->digest, std::nullopt)) {
                runner_->cancel();
                return;
            }
            if (!runner_->approve(analysis->digest, *frame)) {
                fail(failure(ExportFailureCode::AnalysisRefused, "export.approval.refused",
                             "The export could not be approved"));
                runner_->cancel();
                return;
            }
        }
        if (const auto& result = runner_->result())
            finish(*result);
    }

    void stop() override {
        stopRequested_ = true;
        if (runner_)
            runner_->cancel();
        else if (!done_)
            finishStopped();
    }
    void setPaused(const bool paused) override {
        if (runner_)
            runner_->setPaused(paused);
    }
    [[nodiscard]] bool pausedAtBoundary() const override {
        return runner_ && runner_->pausedAtBoundary();
    }
    [[nodiscard]] std::uint64_t framesCompleted() const override {
        return runner_ ? runner_->encodedFrames() : 0;
    }
    [[nodiscard]] std::optional<std::uint64_t> currentFrame() const override {
        if (!runner_ || done_)
            return std::nullopt;
        return input_.range.first + runner_->encodedFrames();
    }
    [[nodiscard]] ExportDeviceRoute route() const override {
        return runner_ && runner_->waitingForCapacity() ? capacityWaitRoute() : liveRoute(route_);
    }

  private:
    void finish(const SequenceExportResultV1& result) {
        auto& provenance = report_.verification.provenance;
        provenance.gpuFrames = result.gpuEvaluatedFrames;
        provenance.cpuReferenceFrames = result.encodedFrames > result.gpuEvaluatedFrames
                                            ? result.encodedFrames - result.gpuEvaluatedFrames
                                            : 0;
        provenance.gpuNativeDispatches = result.gpuNativeDispatches;
        provenance.gpuReadbacks = result.gpuReadbacks;
        provenance.gpuReadbackSubmissions = result.gpuReadbackSubmissions;
        provenance.gpuTransferredPayloads = result.gpuTransferredPayloads;
        provenance.gpuProcessPayloadBytes = result.gpuProcessPayloadBytes;
        provenance.gpuEncodedPayloadBytes = result.gpuEncodedPayloadBytes;
        provenance.gpuDeviceOwnershipEpoch = result.gpuDeviceOwnershipEpoch;
        provenance.framesReused = result.framesReused;
        provenance.gpuDisplayFrames = result.gpuDisplayFrames;
        provenance.cpuDisplayFrames = result.cpuDisplayFrames;
        route_ = provenance.cpuReferenceFrames > 0 || provenance.cpuDisplayFrames > 0
                     ? ExportDeviceRoute::CpuReference
                     : ExportDeviceRoute::Gpu;
        if (const auto* analysis = runner_->analysis())
            provenance.encoder = analysis->implementationNote;
        if (report_.failure)
            return void(done_ = true);
        if (!result.published()) {
            if (stopRequested_ ||
                (result.failure && result.failure->reason == media::provider::Error::Cancelled))
                return finishStopped();
            if (result.publication.outcome ==
                platform::StagedArtifactPublicationOutcome::ExternalModificationConflict)
                return fail(failure(ExportFailureCode::PublicationConflict,
                                    "export.publication.conflict",
                                    "The destination changed outside Bloom; it was not replaced"));
            auto code = ExportFailureCode::EncoderFailed;
            if (result.failure) {
                using media::provider::Error;
                switch (result.failure->reason) {
                case Error::Oversized:
                    code = ExportFailureCode::DestinationFull;
                    break;
                case Error::Timeout:
                    code = ExportFailureCode::DeadlineExceeded;
                    break;
                case Error::Busy:
                    code = ExportFailureCode::ResourceRefused;
                    break;
                case Error::Io:
                    code = ExportFailureCode::WriteFailed;
                    break;
                default:
                    break;
                }
            }
            // A per-frame refusal already carries a plain sentence (and its diagnostic); any other
            // media failure detail is technical and goes to the log under a plain sentence.
            const std::string detail =
                result.failure ? result.failure->detail : std::string("no publication result");
            const bool readable = !result.diagnostic.empty();
            log(runtime::DiagnosticSeverity::Error, "export.media.detail",
                readable ? result.diagnostic : detail);
            return fail(failure(code, "export.media.failed",
                                readable ? detail
                                         : std::string(exportFailureSummary(code)) +
                                               " The details are in the log."));
        }
        const auto& settings = input_.output.record.settings;
        auto& artifacts = report_.verification.artifacts;
        artifacts.publishedArtifacts = 1;
        artifacts.firstPath = input_.output.destination.firstPath;
        artifacts.lastPath = artifacts.firstPath;
        artifacts.publication = publicationOutcome(result.publication.outcome);
        report_.published.push_back(artifacts.firstPath);
        const auto expectedFrames = input_.range.last - input_.range.first + 1;
        std::vector<ExportCheckResult> checks;
        if (result.evidence) {
            const auto& evidence = *result.evidence;
            artifacts.totalBytes = evidence.artifactBytes;
            artifacts.digest = evidence.artifact;
            checks.push_back(check(ExportCheck::Reopen,
                                   evidence.result == media::provider::QcResult::Pass
                                       ? ExportCheckOutcome::Passed
                                       : ExportCheckOutcome::Failed,
                                   "export.check.reopen.worker"));
            const bool wav = settings.format == ExportFormat::Wav;
            checks.push_back(
                check(ExportCheck::FrameCount,
                      wav || evidence.frameCount == expectedFrames
                          ? (wav ? ExportCheckOutcome::NotApplicable : ExportCheckOutcome::Passed)
                          : ExportCheckOutcome::Failed,
                      "export.check.frame-count", expectedFrames, evidence.frameCount));
            checks.push_back(check(
                ExportCheck::Timing,
                evidence.duration.denominator > 0 ? ExportCheckOutcome::Passed
                                                  : ExportCheckOutcome::Failed,
                "export.check.timing", {},
                ExportRationalValue{evidence.duration.numerator, evidence.duration.denominator}));
            const auto& tags = evidence.colourTags;
            const auto expectedTags = result.expectedColourTags;
            if (wav) {
                checks.push_back(check(ExportCheck::ColourTags, ExportCheckOutcome::NotApplicable,
                                       "export.check.colour-tags.audio"));
            } else {
                const ExportColourTags observed{
                    static_cast<std::uint8_t>(std::max(tags.primaries, 2)),
                    static_cast<std::uint8_t>(std::max(tags.transfer, 2)),
                    static_cast<std::uint8_t>(std::max(tags.matrix, 2)), tags.range == 2};
                const ExportColourTags expected{
                    static_cast<std::uint8_t>(std::max(expectedTags.primaries, 2)),
                    static_cast<std::uint8_t>(std::max(expectedTags.transfer, 2)),
                    static_cast<std::uint8_t>(std::max(expectedTags.matrix, 2)),
                    expectedTags.range == 2};
                checks.push_back(check(ExportCheck::ColourTags,
                                       observed == expected ? ExportCheckOutcome::Passed
                                                            : ExportCheckOutcome::Failed,
                                       observed == expected ? "export.check.colour-tags.match"
                                                            : "export.check.colour-tags.mismatch",
                                       expected, observed));
            }
            checks.push_back(
                check(ExportCheck::Container,
                      evidence.container == result.expectedContainer ? ExportCheckOutcome::Passed
                                                                     : ExportCheckOutcome::Failed,
                      "export.check.container", result.expectedContainer, evidence.container));
            const bool audio = settings.format == ExportFormat::Wav || settings.includeAudio;
            checks.push_back(
                check(ExportCheck::Audio,
                      audio ? ExportCheckOutcome::Passed : ExportCheckOutcome::NotApplicable,
                      "export.check.audio", {}, evidence.audioSamples));
        } else {
            checks.push_back(check(ExportCheck::Reopen, ExportCheckOutcome::Failed,
                                   "export.check.reopen.missing"));
        }
        checks.push_back(check(ExportCheck::Naming, ExportCheckOutcome::NotApplicable,
                               "export.check.naming.single-file"));
        checks.push_back(
            check(ExportCheck::ArtifactDigest,
                  artifacts.digest ? ExportCheckOutcome::Passed : ExportCheckOutcome::Failed,
                  "export.check.artifact-digest"));
        checks.push_back(check(ExportCheck::Publication,
                               artifacts.publication == ExportPublicationOutcome::Published
                                   ? ExportCheckOutcome::Passed
                                   : ExportCheckOutcome::Warning,
                               "export.check.publication"));
        checks.push_back(provenanceCheck());
        finishPublished(std::move(checks));
    }

    std::unique_ptr<SequenceExportRunnerV1> runner_;
    bool approvalAsked_ = false;
    bool stopRequested_ = false;
    bool hardware_ = false;
    ExportDeviceRoute route_ = ExportDeviceRoute::None;
};

} // namespace

// DPX (issue #369) is declared in the 1.29 vocabulary and the catalogue; this build reports it
// unavailable with a typed reason rather than writing a different format.
class UnavailableDriver final : public DriverBase {
  public:
    UnavailableDriver(const ExportJobRunnerDependencies& dependencies, ExportDriverInput input,
                      std::string reason)
        : DriverBase(dependencies, std::move(input)) {
        fail(failure(ExportFailureCode::EncoderUnavailable, "export.preset.unavailable",
                     std::move(reason)));
    }
    void poll() override {}
    void stop() override {}
    void setPaused(bool) override {}
    [[nodiscard]] bool pausedAtBoundary() const override { return false; }
    [[nodiscard]] std::uint64_t framesCompleted() const override { return 0; }
    [[nodiscard]] std::optional<std::uint64_t> currentFrame() const override {
        return std::nullopt;
    }
    [[nodiscard]] ExportDeviceRoute route() const override { return ExportDeviceRoute::None; }
};

std::unique_ptr<ExportOutputDriver>
makeExportOutputDriver(const ExportJobRunnerDependencies& dependencies, ExportDriverInput input) {
    if (input.output.record.settings.format == ExportFormat::Dpx)
        return std::make_unique<UnavailableDriver>(dependencies, std::move(input),
                                                   std::string(kExportDpxUnavailableReason));
    if (const auto preset = imagePreset(input.output.record.settings))
        return std::make_unique<ImageDriver>(dependencies, std::move(input), *preset);
    return std::make_unique<MediaDriver>(dependencies, std::move(input));
}

} // namespace bloom::host::detail
