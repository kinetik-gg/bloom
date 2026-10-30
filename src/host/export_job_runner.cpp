// ADR 0033 section 6: one captured job, its outputs one after another through the existing
// attempt -> policy approval -> publication path, then the job-level verification. Moved from
// src/ui/frame_export_controller*.cpp (the still/range and composition drivers, the COLOR-5
// deliverable preparation and the facet/failure descriptions, now typed results).
#include <bloom/host/export_job_runner.hpp>

#include "export_output_drivers.hpp"
#include "export_queue_values.hpp"

#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>

namespace bloom::host {

std::optional<runtime::EvaluationColorIntent>
exportColorIntent(const document::ColorSettings& settings,
                  const document::Composition& composition) {
    const auto& locator = settings.ocioConfig.locator;
    std::string_view uri;
    if (const auto* builtIn = std::get_if<document::BuiltInOcioConfigLocator>(&locator))
        uri = builtIn->uri;
    else if (const auto* relative = std::get_if<document::ProjectRelativeOciozLocator>(&locator))
        uri = relative->path;
    else if (const auto* archive = std::get_if<document::ExternalOciozLocator>(&locator))
        uri = archive->uri;
    else if (const auto* external = std::get_if<document::ExternalOcioConfigLocator>(&locator))
        uri = external->uri;
    const runtime::EvaluationColorIntent borrowed{
        .workingColorSpaceId = composition.workingColorSpaceId()
                                   ? std::string_view(*composition.workingColorSpaceId())
                                   : std::string_view(settings.processColorSpaceId),
        .ocioConfigRevision = settings.ocioConfig.expectedRevision.digest,
        .ocioConfigUri = uri};
    return runtime::captureEvaluationColorIntent(borrowed);
}

std::optional<ExportResolvedRange> resolveExportRange(const document::ExportJob& job,
                                                      const document::Project& project) {
    const auto* composition = project.findComposition(job.composition);
    if (composition == nullptr)
        return std::nullopt;
    const auto rate = composition->format().frameRate();
    const auto mapping = core::FrameTimeMapping::create(composition->duration(), rate.numerator(),
                                                        rate.denominator());
    if (!mapping)
        return std::nullopt;
    const auto maximum = mapping.value()->maximumFrameIndex();
    const auto& range = job.render.range;
    switch (range.kind) {
    case document::ExportRangeKind::Composition:
        return ExportResolvedRange{0, maximum};
    case document::ExportRangeKind::WorkArea: {
        const auto& area = composition->workArea();
        if (!area)
            return ExportResolvedRange{0, maximum};
        const auto first = mapping.value()->nearestFrameIndex(area->start);
        const auto end =
            core::FrameTimeMapping::create(area->end, rate.numerator(), rate.denominator());
        if (!end)
            return ExportResolvedRange{first, first};
        return ExportResolvedRange{first,
                                   std::clamp(end.value()->maximumFrameIndex(), first, maximum)};
    }
    case document::ExportRangeKind::Custom:
        if (range.first > range.last || range.last > maximum)
            return std::nullopt;
        return ExportResolvedRange{range.first, range.last};
    case document::ExportRangeKind::Frame:
        if (range.first > maximum)
            return std::nullopt;
        return ExportResolvedRange{range.first, range.first};
    }
    return std::nullopt;
}

struct ExportJobRunner::State {
    State(ExportJobRunnerDependencies dependenciesValue, ExportJobRunRequest requestValue)
        : dependencies(std::move(dependenciesValue)), request(std::move(requestValue)) {}
    ExportJobRunnerDependencies dependencies;
    ExportJobRunRequest request;
    std::optional<runtime::EvaluationColorIntent> colorIntent;
    ExportJobRunPhase phase = ExportJobRunPhase::Preparing;
    std::vector<ExportOutputRunState> outputs;
    std::size_t current = 0;
    std::unique_ptr<detail::ExportOutputDriver> driver;
    std::optional<ExportFailure> failure;
    bool stopRequested = false;
    bool stopped = false;
    bool pauseRequested = false;
    std::uint64_t completedBefore = 0;
    std::vector<ExportLogEntry> log;
    ExportDeviceRoute route = ExportDeviceRoute::None;

    [[nodiscard]] std::uint64_t perOutputFrames() const {
        return request.range.last - request.range.first + 1;
    }

    void note(const runtime::DiagnosticSeverity severity, std::string code, std::string message,
              std::optional<ExportOutputId> output = std::nullopt,
              std::optional<std::uint64_t> frame = std::nullopt) {
        log.push_back(
            detail::exportLog(severity, std::move(code), std::move(message), output, frame));
    }

    void startOutput() {
        while (current < outputs.size()) {
            if (stopRequested) {
                for (auto index = current; index < outputs.size(); ++index)
                    outputs[index].state = ExportOutputState::Skipped;
                current = outputs.size();
                break;
            }
            auto& run = request.outputs[current];
            outputs[current].state = ExportOutputState::Preparing;
            detail::ExportDriverInput input{.snapshot = request.snapshot,
                                            .composition = request.job.composition,
                                            .compositionName = request.compositionName,
                                            .colorIntent = *colorIntent,
                                            .assetBaseDirectory = request.assetBaseDirectory,
                                            .job = request.job,
                                            .output = run,
                                            .range = request.range,
                                            .encoders = request.encoders,
                                            .owner = request.owner};
            driver = detail::makeExportOutputDriver(dependencies, std::move(input));
            driver->setPaused(pauseRequested);
            note(runtime::DiagnosticSeverity::Information, "export.output.started",
                 "Rendering " + run.destination.firstPath.filename().string(), run.record.id);
            return;
        }
        phase = ExportJobRunPhase::Verifying;
    }

    void finishOutput() {
        auto& state = outputs[current];
        auto report = driver->takeReport();
        state.verification = std::move(report.verification);
        state.published = std::move(report.published);
        state.state = report.state;
        completedBefore += driver->framesCompleted();
        for (auto& entry : report.log)
            log.push_back(std::move(entry));
        if (report.failure) {
            failure = report.failure;
            failure->output = state.output;
            note(runtime::DiagnosticSeverity::Error, failure->diagnosticCode, failure->reason,
                 state.output, failure->frame);
        } else if (report.state == ExportOutputState::Stopped) {
            stopped = true;
        } else {
            note(runtime::DiagnosticSeverity::Information, "export.output.published",
                 "Published " + (state.published.empty()
                                     ? std::string()
                                     : state.published.front().filename().string()),
                 state.output);
        }
        driver.reset();
        ++current;
        if (failure || stopped) {
            for (auto index = current; index < outputs.size(); ++index) {
                outputs[index].state = ExportOutputState::Skipped;
                outputs[index].verification = detail::notRunVerification(outputs[index].output);
            }
            current = outputs.size();
            phase = ExportJobRunPhase::Finished;
            return;
        }
        startOutput();
    }
};

ExportJobRunner::ExportJobRunner(ExportJobRunnerDependencies dependencies,
                                 ExportJobRunRequest request)
    : state_(std::make_unique<State>(std::move(dependencies), std::move(request))) {
    auto& s = *state_;
    for (const auto& output : s.request.outputs)
        s.outputs.emplace_back().output = output.record.id;
    const auto* composition =
        s.request.snapshot.project().findComposition(s.request.job.composition);
    if (composition == nullptr) {
        s.failure = detail::exportFailure(ExportFailureCode::CompositionMissing,
                                          "export.composition-missing",
                                          "The job's composition no longer exists");
        s.phase = ExportJobRunPhase::Finished;
        return;
    }
    s.colorIntent = exportColorIntent(s.request.colorSettings, *composition);
    if (!s.colorIntent) {
        s.failure = detail::exportFailure(ExportFailureCode::Internal, "export.color-intent",
                                          "The export colour intent could not be captured");
        s.phase = ExportJobRunPhase::Finished;
        return;
    }
    s.note(runtime::DiagnosticSeverity::Information, "export.job.started",
           "Frames " + std::to_string(s.request.range.first) + " to " +
               std::to_string(s.request.range.last));
    s.phase = ExportJobRunPhase::Rendering;
    s.startOutput();
}

ExportJobRunner::~ExportJobRunner() {
    if (state_ && state_->driver)
        state_->driver->stop();
}

void ExportJobRunner::poll() {
    auto& s = *state_;
    if (s.phase == ExportJobRunPhase::Rendering && s.driver) {
        s.driver->poll();
        s.route = s.driver->route();
        s.outputs[s.current].state = s.driver->state();
        if (s.driver->done())
            s.finishOutput();
    }
    if (s.phase == ExportJobRunPhase::Verifying) {
        // Job-level checks over what was published: every output already verified its own
        // artifacts on reopen, so the job is the aggregate.
        s.note(runtime::DiagnosticSeverity::Information, "export.job.verified",
               "Every output verified");
        s.phase = ExportJobRunPhase::Finished;
    }
}

void ExportJobRunner::stop() {
    auto& s = *state_;
    s.stopRequested = true;
    if (s.driver)
        s.driver->stop();
    else if (s.phase != ExportJobRunPhase::Finished) {
        s.stopped = true;
        s.phase = ExportJobRunPhase::Finished;
    }
}

void ExportJobRunner::setPaused(const bool paused) {
    state_->pauseRequested = paused;
    if (state_->driver)
        state_->driver->setPaused(paused);
}

bool ExportJobRunner::paused() const noexcept {
    return state_->pauseRequested && state_->driver && state_->driver->pausedAtBoundary();
}
bool ExportJobRunner::pauseRequested() const noexcept { return state_->pauseRequested; }
ExportJobRunPhase ExportJobRunner::phase() const noexcept { return state_->phase; }
bool ExportJobRunner::stopped() const noexcept { return state_->stopped; }
const std::optional<ExportFailure>& ExportJobRunner::failure() const noexcept {
    return state_->failure;
}
std::span<const ExportOutputRunState> ExportJobRunner::outputs() const noexcept {
    return state_->outputs;
}
std::uint64_t ExportJobRunner::framesCompleted() const noexcept {
    return state_->completedBefore + (state_->driver ? state_->driver->framesCompleted() : 0);
}
std::uint64_t ExportJobRunner::framesTotal() const noexcept {
    return state_->perOutputFrames() * state_->outputs.size();
}
std::optional<std::uint64_t> ExportJobRunner::currentFrame() const noexcept {
    return state_->driver ? state_->driver->currentFrame() : std::nullopt;
}
std::uint32_t ExportJobRunner::outputIndex() const noexcept {
    return static_cast<std::uint32_t>(std::min(state_->current, state_->outputs.size()));
}
ExportDeviceRoute ExportJobRunner::route() const noexcept { return state_->route; }
std::vector<ExportLogEntry> ExportJobRunner::takeLog() {
    auto& s = *state_;
    if (s.driver)
        for (auto& entry : s.driver->takeLog())
            s.log.push_back(std::move(entry));
    return std::exchange(s.log, {});
}

} // namespace bloom::host
