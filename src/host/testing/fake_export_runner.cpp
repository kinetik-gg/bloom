#include <bloom/host/testing/fake_export_runner.hpp>

#include <fstream>

namespace bloom::host::testing {
namespace {

ExportCheckResult checkResult(const ExportCheck what, const ExportCheckOutcome outcome,
                              std::string code) {
    ExportCheckResult result;
    result.check = what;
    result.outcome = outcome;
    result.code = std::move(code);
    return result;
}

class FakeRun final : public ExportRun {
  public:
    FakeRun(ExportJobRunRequest request, FakeExportScript script)
        : request_(std::move(request)), script_(script) {
        for (const auto& output : request_.outputs) {
            auto& state = outputs_.emplace_back();
            state.output = output.record.id;
        }
        total_ = (request_.range.last - request_.range.first + 1) * outputs_.size();
    }

    void poll() override {
        if (phase_ == ExportJobRunPhase::Finished)
            return;
        if (stopRequested_) {
            for (auto& output : outputs_)
                if (output.state != ExportOutputState::Published)
                    output.state = ExportOutputState::Stopped;
            stopped_ = true;
            phase_ = ExportJobRunPhase::Finished;
            return;
        }
        if (phase_ == ExportJobRunPhase::Preparing) {
            phase_ = ExportJobRunPhase::Rendering;
            if (!outputs_.empty())
                outputs_.front().state = ExportOutputState::Rendering;
            log("export.fake.started", "Fake run started");
            return;
        }
        if (phase_ == ExportJobRunPhase::Rendering) {
            if (pauseRequested_) {
                paused_ = true;
                return;
            }
            paused_ = false;
            if (script_.waitingForViewerPolls > 0 &&
                frames_ >= script_.waitingForViewerAfterFrames &&
                waited_ < script_.waitingForViewerPolls) {
                ++waited_;
                route_ = ExportDeviceRoute::WaitingForViewer;
                return;
            }
            route_ = ExportDeviceRoute::Gpu;
            frames_ = std::min(total_, frames_ + script_.framesPerPoll);
            if (script_.outcome == FakeExportScript::Outcome::FailRendering &&
                frames_ > script_.failAtFrame) {
                ExportFailure failure;
                failure.code = ExportFailureCode::EncoderFailed;
                failure.output = outputs_.front().output;
                failure.frame = request_.range.first + script_.failAtFrame;
                failure.diagnosticCode = "export.fake.encoder-failed";
                failure.reason = "The fake encoder failed";
                failure_ = failure;
                outputs_.front().state = ExportOutputState::Failed;
                outputs_.front().verification = verification(outputs_.front(), false);
                phase_ = ExportJobRunPhase::Finished;
                return;
            }
            if (frames_ >= total_)
                phase_ = ExportJobRunPhase::Verifying;
            return;
        }
        if (phase_ == ExportJobRunPhase::Verifying) {
            const bool failVerification =
                script_.outcome == FakeExportScript::Outcome::FailVerification;
            for (std::size_t index = 0; index < outputs_.size(); ++index) {
                auto& output = outputs_[index];
                output.verification = verification(output, !failVerification);
                output.published.push_back(request_.outputs[index].destination.firstPath);
                if (script_.writeDestinations && !failVerification)
                    std::ofstream(request_.outputs[index].destination.firstPath, std::ios::binary)
                        << "fake";
                output.state = failVerification ? ExportOutputState::Failed
                               : script_.outcome == FakeExportScript::Outcome::SucceedWithWarnings
                                   ? ExportOutputState::PublishedWithWarnings
                                   : ExportOutputState::Published;
            }
            if (failVerification) {
                ExportFailure failure;
                failure.code = ExportFailureCode::VerificationFailed;
                failure.output = outputs_.front().output;
                failure.diagnosticCode = "export.verification.failed";
                failure.reason = "The published output did not pass verification";
                failure_ = failure;
            }
            phase_ = ExportJobRunPhase::Finished;
        }
    }

    void stop() override { stopRequested_ = true; }
    void setPaused(const bool paused) override { pauseRequested_ = paused; }
    [[nodiscard]] bool paused() const noexcept override { return paused_; }
    [[nodiscard]] bool pauseRequested() const noexcept override { return pauseRequested_; }
    [[nodiscard]] ExportJobRunPhase phase() const noexcept override { return phase_; }
    [[nodiscard]] bool stopped() const noexcept override { return stopped_; }
    [[nodiscard]] const std::optional<ExportFailure>& failure() const noexcept override {
        return failure_;
    }
    [[nodiscard]] std::span<const ExportOutputRunState> outputs() const noexcept override {
        return outputs_;
    }
    [[nodiscard]] std::uint64_t framesCompleted() const noexcept override { return frames_; }
    [[nodiscard]] std::uint64_t framesTotal() const noexcept override { return total_; }
    [[nodiscard]] std::optional<std::uint64_t> currentFrame() const noexcept override {
        if (phase_ != ExportJobRunPhase::Rendering)
            return std::nullopt;
        const auto perOutput = request_.range.last - request_.range.first + 1;
        return request_.range.first + (perOutput == 0 ? 0 : frames_ % perOutput);
    }
    [[nodiscard]] std::uint32_t outputIndex() const noexcept override {
        const auto perOutput = request_.range.last - request_.range.first + 1;
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(
            perOutput == 0 ? 0 : frames_ / perOutput, outputs_.empty() ? 0 : outputs_.size() - 1));
    }
    [[nodiscard]] ExportDeviceRoute route() const noexcept override { return route_; }
    [[nodiscard]] std::vector<ExportLogEntry> takeLog() override { return std::exchange(log_, {}); }

  private:
    void log(std::string code, std::string message) {
        ExportLogEntry entry;
        entry.code = std::move(code);
        entry.message = std::move(message);
        log_.push_back(std::move(entry));
    }

    ExportOutputVerification verification(const ExportOutputRunState& output, const bool passed) {
        ExportOutputVerification result;
        result.output = output.output;
        result.checks.push_back(checkResult(ExportCheck::AnalysisPolicy, ExportCheckOutcome::Passed,
                                            "export.check.analysis-policy.fits"));
        result.checks.push_back(checkResult(
            ExportCheck::ColourTags,
            passed ? ExportCheckOutcome::Passed : ExportCheckOutcome::Failed,
            passed ? "export.check.colour-tags.match" : "export.check.colour-tags.mismatch"));
        if (script_.outcome == FakeExportScript::Outcome::SucceedWithWarnings)
            result.checks.push_back(checkResult(ExportCheck::DeviceProvenance,
                                                ExportCheckOutcome::Warning,
                                                "export.check.device.cpu-reference"));
        result.overall = !passed ? ExportCheckOutcome::Failed
                         : script_.outcome == FakeExportScript::Outcome::SucceedWithWarnings
                             ? ExportCheckOutcome::Warning
                             : ExportCheckOutcome::Passed;
        result.artifacts.publishedArtifacts = passed ? 1 : 0;
        result.provenance.gpuFrames = frames_;
        return result;
    }

    ExportJobRunRequest request_;
    FakeExportScript script_;
    std::vector<ExportOutputRunState> outputs_;
    std::vector<ExportLogEntry> log_;
    std::optional<ExportFailure> failure_;
    ExportJobRunPhase phase_ = ExportJobRunPhase::Preparing;
    ExportDeviceRoute route_ = ExportDeviceRoute::None;
    std::uint64_t frames_ = 0;
    std::uint64_t total_ = 0;
    std::uint32_t waited_ = 0;
    bool stopRequested_ = false;
    bool stopped_ = false;
    bool pauseRequested_ = false;
    bool paused_ = false;
};

} // namespace

void FakeExportRunBackend::push(FakeExportScript script) {
    std::scoped_lock lock(mutex_);
    scripts_.push_back(script);
}

void FakeExportRunBackend::setExisting(const std::filesystem::path& firstPath,
                                       const std::uint64_t generation) {
    std::scoped_lock lock(mutex_);
    existing_[firstPath] = generation;
}

void FakeExportRunBackend::clearExisting(const std::filesystem::path& firstPath) {
    std::scoped_lock lock(mutex_);
    existing_.erase(firstPath);
}

std::size_t FakeExportRunBackend::runsStarted() const {
    std::scoped_lock lock(mutex_);
    return started_;
}

std::unique_ptr<ExportRun> FakeExportRunBackend::start(ExportJobRunnerDependencies,
                                                       ExportJobRunRequest request) {
    FakeExportScript script;
    {
        std::scoped_lock lock(mutex_);
        ++started_;
        if (!scripts_.empty()) {
            script = scripts_.front();
            scripts_.pop_front();
        }
    }
    return std::make_unique<FakeRun>(std::move(request), script);
}

ExportDestinationSample
FakeExportRunBackend::sampleDestination(const ExportDestinationSampleRequest& request) {
    ExportDestinationSample sample;
    sample.firstPath = request.firstPath;
    sample.lastPath = request.lastPath;
    sample.fileCount = request.fileCount;
    sample.folderExists = true;
    sample.folderWritable = true;
    sample.freeBytes = 1ULL << 40U;
    std::scoped_lock lock(mutex_);
    if (const auto found = existing_.find(request.firstPath); found != existing_.end()) {
        sample.existing = std::max<std::uint64_t>(1, request.fileCount);
        core::Sha256Hasher hasher;
        const auto text = request.firstPath.string() + "#" + std::to_string(found->second);
        (void)hasher.update(std::as_bytes(std::span(text.data(), text.size())));
        sample.fingerprint = hasher.finalize();
    }
    return sample;
}

} // namespace bloom::host::testing
