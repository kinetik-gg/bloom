// ExportQueueService (ADR 0033): the queue's runtime owner. pump() is the only publisher.
#include "export_queue_service_state.hpp"
#include "export_queue_values.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/platform/destination_facts.hpp>

#include <algorithm>
#include <atomic>
#include <sstream>

namespace bloom::host {
namespace {

using detail::JobRuntime;
using Clock = std::chrono::steady_clock;

ExportCommandAvailability unknownJob() {
    return {false, ExportUnavailableCode::UnknownJob, "The job does not exist"};
}

// Forwards a prepared import operation (built on a worker) into an ordinary transaction.
class PreparedImportOperation final : public commands::Operation {
  public:
    explicit PreparedImportOperation(std::shared_ptr<commands::Operation> inner)
        : inner_(std::move(inner)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return inner_->typeId(); }
    [[nodiscard]] commands::OperationResult apply(document::Draft& draft) const override {
        return inner_->apply(draft);
    }

  private:
    std::shared_ptr<commands::Operation> inner_;
};

std::optional<std::filesystem::path> assetPath(const document::AssetLocator& locator,
                                               const std::optional<std::filesystem::path>& folder) {
    if (locator.portability == "external") {
        std::string_view text = locator.path;
        if (text.starts_with("file://"))
            text.remove_prefix(7);
        return std::filesystem::path(std::string(text)).lexically_normal();
    }
    if (!folder)
        return std::nullopt;
    return (*folder / locator.path).lexically_normal();
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Subscription

ExportQueueSubscription::ExportQueueSubscription(std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}
ExportQueueSubscription::ExportQueueSubscription(ExportQueueSubscription&&) noexcept = default;
ExportQueueSubscription&
ExportQueueSubscription::operator=(ExportQueueSubscription&&) noexcept = default;
ExportQueueSubscription::~ExportQueueSubscription() { close(); }

std::uint64_t ExportQueueSubscription::serial() const noexcept {
    if (!state_)
        return 0;
    std::scoped_lock lock(state_->mutex);
    return state_->serial;
}
ExportQueueSubscriptionState ExportQueueSubscription::state() const noexcept {
    if (!state_)
        return ExportQueueSubscriptionState::Closed;
    std::scoped_lock lock(state_->mutex);
    return state_->closed ? ExportQueueSubscriptionState::Closed
                          : ExportQueueSubscriptionState::Open;
}
std::optional<std::uint64_t>
ExportQueueSubscription::waitForChange(const std::uint64_t observed,
                                       const std::stop_token& stop) const {
    if (!state_)
        return std::nullopt;
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock, stop, [&] { return state_->closed || state_->serial != observed; });
    if (state_->closed || stop.stop_requested())
        return std::nullopt;
    return state_->serial;
}
void ExportQueueSubscription::close() noexcept {
    if (!state_)
        return;
    {
        std::scoped_lock lock(state_->mutex);
        state_->closed = true;
    }
    state_->changed.notify_all();
}

// ---------------------------------------------------------------------------------------------
// Service internals

namespace {

struct Pump final {
    ExportQueueService::State& s;
    std::vector<ExportQueueEventPayload> produced;

    void log(const ExportJobId id, JobRuntime& job, ExportLogEntry entry) {
        entry.sequence = s.nextLogSequence++;
        entry.sinceStart =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - job.started);
        job.log.push_back(entry);
        ++job.logCount;
        job.lastLogSequence = entry.sequence;
        while (job.log.size() > s.dependencies.limits.logEntriesPerRun)
            job.log.pop_front();
        std::scoped_lock lock(s.mutex);
        auto& shared = s.logs[id];
        shared.push_back(std::move(entry));
        while (shared.size() > s.dependencies.limits.logEntriesPerRun)
            shared.pop_front();
    }

    void readSession() {
        auto& session = s.dependencies.session;
        auto decoded = session.decodedSnapshot();
        if (decoded)
            s.snapshot = decoded.snapshot();
        else
            s.snapshot.reset();
        if (const auto* colour = session.colorSettings())
            s.colorSettings = *colour;
        else
            s.colorSettings.reset();
        const auto state = session.stateSnapshot();
        s.editable = s.snapshot.has_value() && state.editability.has_value();
        s.projectFolder = state.displayPath
                              ? std::optional(state.displayPath->value().parent_path())
                              : std::nullopt;
    }

    void syncJobs() {
        // Acknowledged terminal transient jobs leave the queue.
        for (auto it = s.acknowledged.begin(); it != s.acknowledged.end();) {
            const auto job = s.jobs.find(*it);
            if (job != s.jobs.end() && !isTerminal(job->second.state)) {
                ++it;
                continue;
            }
            s.transients.erase(*it);
            it = s.acknowledged.erase(it);
        }
        const auto records = detail::allRecords(s);
        for (const auto* record : records) {
            auto& job = s.jobs[record->id];
            job.transient = s.transients.contains(record->id);
            for (const auto& output : record->outputs)
                (void)job.outputs[output.id];
        }
        for (auto it = s.jobs.begin(); it != s.jobs.end();) {
            const auto id = it->first;
            const bool present =
                std::ranges::any_of(records, [id](const auto* record) { return record->id == id; });
            auto& job = it->second;
            if (present) {
                ++it;
                continue;
            }
            if (job.state == ExportJobState::Waiting) {
                std::erase(s.waiting, id);
                job.state = ExportJobState::Ready;
            }
            if (isRunning(job.state)) {
                stopJob(id, job, ExportStopCause::JobRemoved);
                ++it;
                continue;
            }
            if (job.state == ExportJobState::Stopping) {
                ++it;
                continue;
            }
            {
                std::scoped_lock lock(s.mutex);
                s.logs.erase(id);
            }
            it = s.jobs.erase(it);
        }
    }

    void stopJob(const ExportJobId id, JobRuntime& job, const ExportStopCause cause) {
        if (job.state == ExportJobState::Waiting) {
            std::erase(s.waiting, id);
            job.state = ExportJobState::Ready;
            return;
        }
        if (!isRunning(job.state) || job.state == ExportJobState::Stopping)
            return;
        job.pendingStopCause = cause;
        job.state = ExportJobState::Stopping;
        for (auto& [output, sample] : job.preparingSamples)
            sample.cancel();
        if (job.runner)
            job.runner->stop();
        if (job.group)
            job.group->cancel();
    }

    void collectSamples() {
        for (auto it = s.pendingSamples.begin(); it != s.pendingSamples.end();) {
            auto result = it->handle.tryTakeResult();
            if (!result) {
                ++it;
                continue;
            }
            if (result->state() == runtime::TaskState::Succeeded && result->value() &&
                *result->value()) {
                auto sample = **result->value();
                const auto confirmation = s.confirmations.find(it->output);
                if (confirmation != s.confirmations.end() &&
                    sample.fingerprint != confirmation->second)
                    s.confirmations.erase(confirmation);
                if (s.resampleAfterPublish.erase(it->output) != 0 && sample.fingerprint)
                    s.ownPublications[it->output] = *sample.fingerprint;
                s.samples[it->output] = std::move(sample);
                s.sampleKeys[it->output] = it->key;
            }
            it = s.pendingSamples.erase(it);
        }
    }

    void scheduleSamples() {
        if (!s.snapshot || s.runner == ExportRunnerState::ShuttingDown)
            return;
        if (std::exchange(s.refreshRequested, false))
            s.sampleKeys.clear();
        for (const auto& record : s.snapshot->project().exportJobs()) {
            const auto& job = s.jobs[record.id];
            if (isRunning(job.state))
                continue;
            const auto context = detail::destinationContext(s, record, &job);
            for (const auto& output : record.outputs) {
                const auto resolved = resolveDestination(record, output, context);
                if (!resolved.issues.empty() || resolved.firstPath.empty())
                    continue;
                const detail::SampleKey key{resolved.firstPath, resolved.lastPath,
                                            resolved.fileCount};
                const auto current = s.sampleKeys.find(output.id);
                if (current != s.sampleKeys.end() && current->second == key)
                    continue;
                if (std::ranges::any_of(s.pendingSamples, [&](const auto& pending) {
                        return pending.output == output.id && pending.key == key;
                    }))
                    continue;
                if (current != s.sampleKeys.end()) {
                    // The destination changed: the old confirmation no longer applies.
                    s.confirmations.erase(output.id);
                    s.sampleKeys.erase(current);
                }
                auto handle = detail::submitDestinationSample(
                    s.dependencies.scheduler, s.dependencies.backend, key, std::nullopt,
                    runtime::TaskOwner{runtime::TaskOwnerKind::Export,
                                       runtime::TaskOwnerId::fromRaw(s.serviceOwner)});
                if (handle.isValid())
                    s.pendingSamples.push_back({output.id, key, std::move(handle)});
            }
        }
    }

    void fail(const ExportJobId id, JobRuntime& job, ExportFailure failure) {
        log(id, job,
            detail::exportLog(runtime::DiagnosticSeverity::Error, failure.diagnosticCode,
                              failure.reason, failure.output, failure.frame));
        if (failure.summary.empty())
            failure.summary = std::string(exportFailureSummary(failure.code));
        job.failure = std::move(failure);
        finishRun(id, job, ExportJobState::Failed);
    }

    void startNext() {
        if (s.active || s.runner == ExportRunnerState::ShuttingDown || !s.snapshot)
            return;
        while (!s.waiting.empty() && !s.active) {
            const auto id = s.waiting.front();
            s.waiting.pop_front();
            const auto* record = detail::findRecord(s, id);
            auto found = s.jobs.find(id);
            if (record == nullptr || found == s.jobs.end())
                continue;
            prepare(id, *record, found->second);
        }
    }

    void prepare(const ExportJobId id, const document::ExportJob& record, JobRuntime& job) {
        s.active = id;
        s.runner = ExportRunnerState::Running;
        const auto run = detail::allocateExportOwnerId();
        const runtime::TaskOwner owner{runtime::TaskOwnerKind::Export,
                                       runtime::TaskOwnerId::fromRaw(run)};
        auto group = s.dependencies.scheduler.createGroup(
            owner, "Export " + (record.name.empty() ? std::string("job") : record.name));
        job.group.reset();
        std::optional<runtime::TaskGroupId> groupId;
        if (group.accepted()) {
            groupId = group.handle.id();
            job.group = std::move(group.handle);
        }
        job.run = ExportRunLink{run, owner.id, groupId.value_or(runtime::TaskGroupId{})};
        {
            std::scoped_lock lock(s.mutex);
            s.owners[run] = id;
            s.logs[id].clear();
        }
        job.state = ExportJobState::Preparing;
        job.failure.reset();
        job.stopCause.reset();
        job.pendingStopCause.reset();
        job.log.clear();
        job.logCount = 0;
        job.started = Clock::now();
        job.excluded = {};
        job.excludedSince.reset();
        job.frameTimes.clear();
        job.lastFrames = 0;
        job.progress = {};
        job.captured = record;
        job.capturedSnapshot = s.snapshot;
        const auto context = detail::destinationContext(s, record, nullptr);
        job.year = context.year;
        job.month = context.month;
        job.day = context.day;
        job.dateFrozen = true;
        job.capturedCompositionName = context.compositionName;
        job.capturedRange = context.range;
        for (auto& [outputId, output] : job.outputs) {
            output.state = ExportOutputState::Pending;
            output.verification.reset();
            output.import = detail::exportImport(outputId, ExportAssetImportStatus::NotRequested);
            output.importFailure.clear();
            output.preparedImport.reset();
            output.importTask.reset();
        }
        log(id, job,
            detail::exportLog(runtime::DiagnosticSeverity::Information, "export.job.preparing",
                              "Preparing the export"));
        if (!s.snapshot->project().findComposition(record.composition))
            return fail(id, job,
                        detail::exportFailure(ExportFailureCode::CompositionMissing,
                                              "export.composition-missing",
                                              "The job's composition no longer exists"));
        if (!context.range)
            return fail(id, job,
                        detail::exportFailure(ExportFailureCode::PreflightRefused,
                                              "export.range.outside",
                                              "The range lies outside the composition"));
        job.preparingSamples.clear();
        job.preparedSamples.clear();
        for (const auto& output : record.outputs) {
            if (!output.enabled) {
                job.outputs[output.id].state = ExportOutputState::Skipped;
                continue;
            }
            const auto resolved = resolveDestination(record, output, context);
            if (!resolved.issues.empty())
                return fail(id, job,
                            detail::exportFailure(ExportFailureCode::PreflightRefused,
                                                  "export.preflight.naming",
                                                  resolved.issues.front().reason, output.id));
            job.preparingSamples.emplace(
                output.id, detail::submitDestinationSample(
                               s.dependencies.scheduler, s.dependencies.backend,
                               {resolved.firstPath, resolved.lastPath, resolved.fileCount}, groupId,
                               owner, true));
        }
    }

    void startRunner(const ExportJobId id, JobRuntime& job) {
        const auto& record = *job.captured;
        const auto context = detail::destinationContext(s, record, &job);
        auto runContext = context;
        runContext.range = job.capturedRange;
        runContext.compositionName = job.capturedCompositionName;
        ExportJobRunRequest request{
            .snapshot = *job.capturedSnapshot,
            .job = record,
            .compositionName = job.capturedCompositionName,
            .range = *job.capturedRange,
            .colorSettings = s.colorSettings.value_or(document::ColorSettings{}),
            .assetBaseDirectory = s.projectFolder.value_or(std::filesystem::path{}),
            .outputs = {},
            .encoders = s.dependencies.presets->snapshot()->encoders,
            .owner = {runtime::TaskOwnerKind::Export, job.run->owner},
            .group = job.run->group.isValid() ? std::optional(job.run->group) : std::nullopt};
        const auto* composition =
            job.capturedSnapshot->project().findComposition(record.composition);
        for (const auto& output : record.outputs) {
            if (!output.enabled)
                continue;
            ExportRunOutput run;
            run.record = output;
            run.context = runContext;
            run.destination = resolveDestination(record, output, runContext);
            const auto& sample = job.preparedSamples[output.id];
            const auto confirmation = s.confirmations.find(output.id);
            const auto own = s.ownPublications.find(output.id);
            if (!sample.folderWritable)
                return fail(id, job,
                            detail::exportFailure(ExportFailureCode::PreflightRefused,
                                                  "export.preflight.not-writable",
                                                  "The destination folder cannot be written",
                                                  output.id));
            if (sample.existing > 0 && sample.fingerprint) {
                const bool confirmed = confirmation != s.confirmations.end() &&
                                       confirmation->second == *sample.fingerprint;
                const bool ownFile =
                    own != s.ownPublications.end() && own->second == *sample.fingerprint;
                switch (output.destination.overwrite) {
                case document::ExportOverwritePolicy::Refuse:
                    return fail(id, job,
                                detail::exportFailure(
                                    ExportFailureCode::PreflightRefused, "export.preflight.exists",
                                    "The destination exists and this output refuses to "
                                    "replace it",
                                    output.id));
                case document::ExportOverwritePolicy::AskBeforeReplacing:
                    if (!confirmed && !ownFile)
                        return fail(
                            id, job,
                            detail::exportFailure(ExportFailureCode::PublicationConflict,
                                                  "export.preflight.changed",
                                                  "The destination changed after it was checked or "
                                                  "confirmed; it was not replaced",
                                                  output.id));
                    run.confirmedSample = sample.fingerprint;
                    break;
                case document::ExportOverwritePolicy::Replace:
                    break;
                }
            }
            run.refuseExisting =
                output.destination.overwrite == document::ExportOverwritePolicy::Refuse;
            const auto estimate = composition ? detail::estimateBytes(output.settings, *composition,
                                                                      *job.capturedRange)
                                              : std::nullopt;
            if (sample.freeBytes) {
                const auto margin = platform::destinationSafetyMargin(estimate.value_or(0));
                if (estimate && *estimate + margin > *sample.freeBytes)
                    return fail(id, job,
                                detail::exportFailure(
                                    ExportFailureCode::DestinationFull, "export.preflight.space",
                                    "Not enough free space at the destination", output.id));
                run.artifactByteLimit = *sample.freeBytes > margin ? *sample.freeBytes - margin : 1;
            }
            request.outputs.push_back(std::move(run));
            // The run consumes the confirmation.
            s.confirmations.erase(output.id);
        }
        ExportJobRunnerDependencies dependencies{
            .scheduler = s.dependencies.scheduler,
            .compiler = s.dependencies.compiler,
            .publications = s.dependencies.publications,
            .artifacts = s.dependencies.artifacts,
            .ledger = s.dependencies.ledger,
            .gpu = s.dependencies.gpu,
            .retirement = s.dependencies.retirement,
            .displayProcessor = s.dependencies.displayProcessor,
            .scratchDirectory = s.dependencies.scratchDirectory};
        job.runner = s.dependencies.backend->start(std::move(dependencies), std::move(request));
        if (!job.runner)
            return fail(id, job,
                        detail::exportFailure(ExportFailureCode::Internal, "export.runner.refused",
                                              "The export runner could not start"));
        job.state = ExportJobState::Rendering;
    }

    void updateProgress(JobRuntime& job) {
        auto& runner = *job.runner;
        auto& progress = job.progress;
        const auto now = Clock::now();
        progress.framesCompleted = runner.framesCompleted();
        progress.framesTotal = runner.framesTotal();
        progress.outputIndex = runner.outputIndex();
        progress.outputCount = static_cast<std::uint32_t>(runner.outputs().size());
        progress.currentFrame = runner.currentFrame();
        progress.route = runner.route();
        progress.fraction = progress.framesTotal == 0
                                ? std::nullopt
                                : std::optional(static_cast<double>(progress.framesCompleted) /
                                                static_cast<double>(progress.framesTotal));
        const bool excluded = job.state == ExportJobState::Paused ||
                              progress.route == ExportDeviceRoute::WaitingForViewer;
        if (excluded && !job.excludedSince)
            job.excludedSince = now;
        if (!excluded && job.excludedSince) {
            job.excluded += now - *job.excludedSince;
            job.excludedSince.reset();
        }
        const auto excludedNow =
            job.excluded + (job.excludedSince ? now - *job.excludedSince : Clock::duration{});
        progress.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - job.started - excludedNow);
        while (job.lastFrames < progress.framesCompleted) {
            ++job.lastFrames;
            job.frameTimes.push_back(now);
            while (job.frameTimes.size() > 16)
                job.frameTimes.pop_front();
        }
        progress.remaining.reset();
        if (!excluded &&
            job.frameTimes.size() >=
                std::max<std::size_t>(2, s.dependencies.limits.etaMinimumFrames) &&
            progress.framesTotal > progress.framesCompleted) {
            const auto span = job.frameTimes.back() - job.frameTimes.front();
            const auto perFrame = span / static_cast<long>(job.frameTimes.size() - 1);
            progress.remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                perFrame * static_cast<long>(progress.framesTotal - progress.framesCompleted));
        }
    }

    void advance() {
        if (!s.active)
            return;
        const auto id = *s.active;
        auto found = s.jobs.find(id);
        if (found == s.jobs.end()) {
            s.active.reset();
            return;
        }
        auto& job = found->second;
        if (job.state == ExportJobState::Preparing ||
            (job.state == ExportJobState::Stopping && !job.runner &&
             !job.preparingSamples.empty())) {
            for (auto it = job.preparingSamples.begin(); it != job.preparingSamples.end();) {
                auto result = it->second.tryTakeResult();
                if (!result)
                    return;
                if (result->state() == runtime::TaskState::Succeeded && result->value() &&
                    *result->value())
                    job.preparedSamples[it->first] = **result->value();
                it = job.preparingSamples.erase(it);
            }
            if (job.state == ExportJobState::Stopping)
                return finishRun(id, job, ExportJobState::Stopped);
            if (job.preparedSamples.size() <
                static_cast<std::size_t>(std::ranges::count_if(
                    job.captured->outputs, [](const auto& o) { return o.enabled; })))
                return fail(id, job,
                            detail::exportFailure(ExportFailureCode::PreflightRefused,
                                                  "export.preflight.sample",
                                                  "The destination could not be checked"));
            startRunner(id, job);
            if (!job.runner)
                return;
        }
        if (job.state == ExportJobState::Stopping && !job.runner)
            return finishRun(id, job, ExportJobState::Stopped);
        if (!job.runner)
            return;
        job.runner->poll();
        for (auto& entry : job.runner->takeLog())
            log(id, job, std::move(entry));
        if (job.state == ExportJobState::Pausing && job.runner->paused())
            job.state = ExportJobState::Paused;
        updateProgress(job);
        for (const auto& output : job.runner->outputs()) {
            auto& runtime = job.outputs[output.output];
            runtime.state = output.state;
        }
        if (job.runner->phase() == ExportJobRunPhase::Verifying &&
            job.state == ExportJobState::Rendering)
            job.state = ExportJobState::Verifying;
        if (!job.runner->finished())
            return;
        for (const auto& output : job.runner->outputs()) {
            auto& runtime = job.outputs[output.output];
            runtime.state = output.state;
            runtime.verification = output.verification;
            runtime.published = output.published;
            if (output.state == ExportOutputState::Published ||
                output.state == ExportOutputState::PublishedWithWarnings)
                s.resampleAfterPublish.insert(output.output);
        }
        if (job.runner->stopped() || job.state == ExportJobState::Stopping) {
            job.stopCause = job.pendingStopCause.value_or(ExportStopCause::User);
            return finishRun(id, job, ExportJobState::Stopped);
        }
        if (const auto& failure = job.runner->failure()) {
            job.failure = failure;
            if (job.failure->summary.empty())
                job.failure->summary = std::string(exportFailureSummary(job.failure->code));
            if (job.failure->reason.empty())
                job.failure->reason = job.failure->summary;
            return finishRun(id, job, ExportJobState::Failed);
        }
        const bool warnings = std::ranges::any_of(job.runner->outputs(), [](const auto& output) {
            return output.state == ExportOutputState::PublishedWithWarnings;
        });
        finishRun(id, job, warnings ? ExportJobState::DoneWithWarnings : ExportJobState::Done);
    }

    // Moves the job towards its terminal state: asset imports first (section 8).
    void finishRun(const ExportJobId id, JobRuntime& job, const ExportJobState terminal) {
        if (terminal == ExportJobState::Stopped && !job.stopCause)
            job.stopCause = job.pendingStopCause.value_or(ExportStopCause::User);
        job.runner.reset();
        job.preparingSamples.clear();
        job.capturedSnapshot.reset();
        job.excludedSince.reset();
        job.progress.remaining.reset();
        if (job.group) {
            job.group.reset();
        }
        const auto* record = job.captured ? &*job.captured : nullptr;
        bool importing = false;
        if (record)
            for (const auto& output : record->outputs) {
                auto& runtime = job.outputs[output.id];
                if (!output.enabled && runtime.state == ExportOutputState::Pending)
                    runtime.state = ExportOutputState::Skipped;
                if (!output.importIntoAssets) {
                    runtime.import =
                        detail::exportImport(output.id, ExportAssetImportStatus::NotRequested);
                    continue;
                }
                const bool published = runtime.state == ExportOutputState::Published ||
                                       runtime.state == ExportOutputState::PublishedWithWarnings;
                if (!published) {
                    runtime.import = detail::exportImport(
                        output.id, ExportAssetImportStatus::Skipped, "export.import.not-published",
                        "The output did not publish");
                    continue;
                }
                beginImport(output, runtime);
                importing = true;
            }
        job.state = importing ? ExportJobState::Verifying : terminal;
        job.terminalEventPending = true;
        job.pendingTerminal = terminal;
        if (!importing)
            completeTerminal(id, job);
        s.active.reset();
    }

    void beginImport(const document::ExportOutput& output, detail::OutputRuntime& runtime) {
        runtime.import = detail::exportImport(output.id, ExportAssetImportStatus::Pending);
        if (!s.editable || !s.snapshot) {
            runtime.import.status = ExportAssetImportStatus::Failed;
            runtime.import.code = "export.import.read-only-project";
            runtime.import.reason = "The project is read-only or closed";
            return;
        }
        const auto path =
            runtime.published.empty() ? std::filesystem::path{} : runtime.published.front();
        std::optional<document::AssetId> existing;
        for (const auto& asset : s.snapshot->project().assets())
            if (const auto located = assetPath(asset.locator, s.projectFolder);
                located && *located == path.lexically_normal())
                existing = asset.id;
        runtime.refreshAsset = existing;
        const auto folder = s.projectFolder.value_or(path.parent_path());
        auto submission = s.dependencies.scheduler.submit<std::shared_ptr<commands::Operation>>(
            runtime::TaskRequest(
                "Prepare asset import",
                {runtime::TaskOwnerKind::Export, runtime::TaskOwnerId::fromRaw(s.serviceOwner)},
                runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo),
            [path, folder, existing](runtime::TaskContext& context)
                -> runtime::TaskResult<std::shared_ptr<commands::Operation>> {
                using Result = runtime::TaskResult<std::shared_ptr<commands::Operation>>;
                const auto cancelled = [&context] { return context.isCancellationRequested(); };
                if (existing)
                    return Result::succeeded(std::make_shared<commands::RelinkAsset>(
                        *existing, path, folder, cancelled));
                auto import = std::make_shared<commands::ImportAssets>(
                    std::vector<std::filesystem::path>{path}, folder, cancelled);
                if (!import->diagnostic().empty())
                    return Result::failed(
                        runtime::TaskDiagnostic{.code = "export.import.probe",
                                                .severity = runtime::DiagnosticSeverity::Error,
                                                .summary = import->diagnostic(),
                                                .detail = {},
                                                .suggestedAction = {}});
                return Result::succeeded(std::move(import));
            });
        if (!submission.accepted()) {
            runtime.import.status = ExportAssetImportStatus::Failed;
            runtime.import.code = "export.import.refused";
            runtime.import.reason = "The import could not be scheduled";
            return;
        }
        runtime.importTask = std::move(submission.handle);
    }

    void advanceImports() {
        for (auto& [id, job] : s.jobs) {
            if (!job.terminalEventPending || job.state != ExportJobState::Verifying ||
                s.active == id)
                continue;
            bool pending = false;
            for (auto& [outputId, output] : job.outputs) {
                if (output.import.status != ExportAssetImportStatus::Pending)
                    continue;
                if (output.importTask) {
                    auto result = output.importTask->tryTakeResult();
                    if (!result) {
                        pending = true;
                        continue;
                    }
                    output.importTask.reset();
                    if (result->state() != runtime::TaskState::Succeeded || !result->value() ||
                        !*result->value()) {
                        output.import.status = ExportAssetImportStatus::Failed;
                        output.import.code = "export.import.probe";
                        output.import.reason = result->diagnostics().empty()
                                                   ? std::string("The output could not be imported")
                                                   : result->diagnostics().front().summary;
                        continue;
                    }
                    output.preparedImport = *result->value();
                }
                if (s.gestureActive) {
                    pending = true;
                    continue;
                }
                applyImport(outputId, output);
            }
            if (!pending)
                completeTerminal(id, job);
        }
    }

    void applyImport(const ExportOutputId, detail::OutputRuntime& output) {
        auto prepared = std::exchange(output.preparedImport, nullptr);
        if (!prepared || !s.editable || !s.snapshot) {
            output.import.status = ExportAssetImportStatus::Failed;
            output.import.code = "export.import.project-closed";
            output.import.reason = "The project closed or became read-only before the import";
            return;
        }
        std::vector<document::AssetId> before;
        for (const auto& asset : s.snapshot->project().assets())
            before.push_back(asset.id);
        commands::Transaction transaction("Import exported output");
        if (!transaction.add(std::make_unique<PreparedImportOperation>(std::move(prepared)))) {
            output.import.status = ExportAssetImportStatus::Failed;
            output.import.code = "export.import.refused";
            output.import.reason = "The import could not be queued";
            return;
        }
        const auto result = s.dependencies.session.execute(std::move(transaction));
        if (!result.completed() || !result.command->succeeded()) {
            output.import.status = ExportAssetImportStatus::Failed;
            output.import.code = "export.import.rejected";
            output.import.reason = result.command && !result.command->operationFailures.empty()
                                       ? result.command->operationFailures.front().issue.message
                                       : std::string("The project refused the import");
            return;
        }
        readSession();
        if (output.refreshAsset) {
            output.import.status = ExportAssetImportStatus::Refreshed;
            output.import.asset = output.refreshAsset;
            return;
        }
        output.import.status = ExportAssetImportStatus::Imported;
        if (s.snapshot)
            for (const auto& asset : s.snapshot->project().assets())
                if (std::ranges::find(before, asset.id) == before.end())
                    output.import.asset = asset.id;
    }

    void completeTerminal(const ExportJobId id, JobRuntime& job) {
        const auto terminal = job.pendingTerminal.value_or(ExportJobState::Done);
        job.pendingTerminal.reset();
        job.state = terminal;
        job.terminalEventPending = false;
        std::vector<ExportOutputVerification> verifications;
        std::vector<ExportAssetImportResult> imports;
        std::uint32_t importFailures = 0;
        if (job.captured)
            for (const auto& output : job.captured->outputs) {
                const auto& runtime = job.outputs[output.id];
                verifications.push_back(
                    runtime.verification.value_or(detail::notRunVerification(output.id)));
                imports.push_back(runtime.import);
                if (runtime.import.status == ExportAssetImportStatus::Failed)
                    ++importFailures;
            }
        s.drained.importFailures += importFailures;
        const auto run = job.run ? job.run->run : 0;
        switch (terminal) {
        case ExportJobState::Done:
        case ExportJobState::DoneWithWarnings:
            terminal == ExportJobState::Done ? ++s.drained.succeeded
                                             : ++s.drained.succeededWithWarnings;
            produced.push_back(ExportJobSucceeded{id, run,
                                                  terminal == ExportJobState::DoneWithWarnings,
                                                  std::move(verifications), std::move(imports)});
            log(id, job,
                detail::exportLog(runtime::DiagnosticSeverity::Information, "export.job.done",
                                  "Export finished"));
            break;
        case ExportJobState::Failed:
            ++s.drained.failed;
            produced.push_back(ExportJobFailed{id, run, job.failure.value_or(ExportFailure{}),
                                               std::move(verifications), std::move(imports)});
            break;
        default:
            ++s.drained.cancelled;
            produced.push_back(ExportJobCancelled{id, run,
                                                  job.stopCause.value_or(ExportStopCause::User),
                                                  std::move(verifications), std::move(imports)});
            log(id, job,
                detail::exportLog(runtime::DiagnosticSeverity::Information, "export.job.stopped",
                                  "Export stopped"));
            break;
        }
    }

    void drainCheck() {
        const bool busy = s.active.has_value() || !s.waiting.empty() ||
                          std::ranges::any_of(s.jobs, [](const auto& entry) {
                              return entry.second.terminalEventPending;
                          });
        if (!busy && s.runner == ExportRunnerState::Running) {
            s.runner = ExportRunnerState::Idle;
            produced.push_back(std::exchange(s.drained, {}));
        }
    }

    void publish() {
        auto snapshot = detail::buildSnapshot(s);
        std::ostringstream key;
        key << static_cast<int>(snapshot->runner) << '|'
            << (snapshot->activeJob ? snapshot->activeJob->value() : 0) << '|'
            << snapshot->catalogueRevision << '|' << snapshot->documentRevision.value_or(0);
        for (const auto& job : snapshot->jobs)
            key << '|' << job.id.value() << ':' << job.revision;
        for (const auto& entry : snapshot->commands.entries)
            key << (entry.available ? '1' : '0');
        auto text = key.str();
        if (text == s.queueViewKey && produced.empty())
            return;
        s.queueViewKey = std::move(text);
        snapshot->revision = ++s.revision;
        std::vector<std::shared_ptr<ExportQueueSubscription::State>> wake;
        {
            std::scoped_lock lock(s.mutex);
            s.published = snapshot;
            for (auto& payload : produced) {
                s.events.push_back({s.nextEventSequence++, snapshot->revision, std::move(payload)});
                while (s.events.size() > s.dependencies.limits.retainedEvents)
                    s.events.pop_front();
            }
            wake = s.subscriptions;
        }
        produced.clear();
        for (const auto& subscription : wake) {
            {
                std::scoped_lock lock(subscription->mutex);
                subscription->serial = snapshot->revision;
            }
            subscription->changed.notify_all();
        }
    }

    void run() {
        readSession();
        collectSamples();
        syncJobs();
        advance();
        startNext();
        advance();
        advanceImports();
        drainCheck();
        scheduleSamples();
        publish();
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------
// Service

std::uint64_t detail::allocateExportOwnerId() noexcept {
    static std::atomic<std::uint64_t> next{2};
    return next.fetch_add(1, std::memory_order_relaxed);
}

std::string_view exportFailureSummary(const ExportFailureCode code) noexcept {
    switch (code) {
    case ExportFailureCode::CompositionMissing:
        return "The composition no longer exists.";
    case ExportFailureCode::CompileFailed:
        return "Bloom couldn't prepare the composition for export.";
    case ExportFailureCode::PreflightRefused:
        return "A check before rendering stopped the export.";
    case ExportFailureCode::AnalysisRefused:
        return "A frame couldn't be exported in this format.";
    case ExportFailureCode::EvaluationFailed:
        return "Bloom couldn't render a frame.";
    case ExportFailureCode::ResourceRefused:
        return "There wasn't enough memory or GPU capacity for the export.";
    case ExportFailureCode::EncoderUnavailable:
        return "The encoder for this format isn't available.";
    case ExportFailureCode::EncoderFailed:
        return "The encoder failed.";
    case ExportFailureCode::WriteFailed:
        return "Bloom couldn't write the file.";
    case ExportFailureCode::DestinationFull:
        return "The destination ran out of space.";
    case ExportFailureCode::VerificationFailed:
        return "The exported file didn't pass verification.";
    case ExportFailureCode::PublicationConflict:
        return "The destination changed outside Bloom, so it wasn't replaced.";
    case ExportFailureCode::DeadlineExceeded:
        return "The export stopped making progress and timed out.";
    case ExportFailureCode::Internal:
        break;
    }
    return "The export stopped because of a problem in Bloom.";
}

ExportQueueService::ExportQueueService(ExportQueueServiceDependencies dependencies)
    : state_(std::make_unique<State>(std::move(dependencies))) {
    pump();
}

ExportQueueService::~ExportQueueService() {
    for (auto& [id, job] : state_->jobs)
        if (job.runner)
            job.runner->stop();
    std::vector<std::shared_ptr<ExportQueueSubscription::State>> wake;
    {
        std::scoped_lock lock(state_->mutex);
        state_->closed = true;
        wake = state_->subscriptions;
    }
    for (const auto& subscription : wake) {
        {
            std::scoped_lock lock(subscription->mutex);
            subscription->closed = true;
        }
        subscription->changed.notify_all();
    }
}

std::shared_ptr<const ExportQueueSnapshot> ExportQueueService::snapshot() const {
    std::scoped_lock lock(state_->mutex);
    return state_->published;
}

ExportQueueEventBatch ExportQueueService::events(const std::uint64_t afterSequence) const {
    std::scoped_lock lock(state_->mutex);
    ExportQueueEventBatch batch;
    batch.lastSequence = state_->nextEventSequence - 1;
    if (!state_->events.empty() && state_->events.front().sequence > afterSequence + 1)
        batch.gap = true;
    for (const auto& event : state_->events)
        if (event.sequence > afterSequence)
            batch.events.push_back(event);
    return batch;
}

ExportLogBatch ExportQueueService::log(const ExportJobId job,
                                       const std::uint64_t afterSequence) const {
    std::scoped_lock lock(state_->mutex);
    ExportLogBatch batch;
    const auto found = state_->logs.find(job);
    if (found == state_->logs.end())
        return batch;
    const auto& entries = found->second;
    if (!entries.empty()) {
        batch.lastSequence = entries.back().sequence;
        // Entries of this run older than the retained window were dropped.
        if (afterSequence + 1 < entries.front().sequence &&
            entries.size() >= state_->dependencies.limits.logEntriesPerRun)
            batch.gap = true;
    }
    for (const auto& entry : entries)
        if (entry.sequence > afterSequence)
            batch.entries.push_back(entry);
    return batch;
}

ExportQueueSubscribeResult ExportQueueService::subscribe() {
    std::scoped_lock lock(state_->mutex);
    ExportQueueSubscribeResult result;
    if (state_->closed)
        return result;
    std::erase_if(state_->subscriptions, [](const auto& subscription) {
        std::scoped_lock inner(subscription->mutex);
        return subscription->closed;
    });
    if (state_->subscriptions.size() >= kExportQueueSubscriptionCapacity) {
        result.status = ExportQueueSubscribeStatus::CapacityReached;
        return result;
    }
    auto subscription = std::make_shared<ExportQueueSubscription::State>();
    subscription->serial = state_->published->revision;
    state_->subscriptions.push_back(subscription);
    result.status = ExportQueueSubscribeStatus::Accepted;
    result.subscription = ExportQueueSubscription(std::move(subscription));
    return result;
}

std::optional<ExportJobId>
ExportQueueService::jobForTaskOwner(const runtime::TaskOwnerId owner) const {
    std::scoped_lock lock(state_->mutex);
    const auto found = state_->owners.find(owner.value());
    if (found == state_->owners.end())
        return std::nullopt;
    return found->second;
}

ExportPresetCatalogue& ExportQueueService::presets() const noexcept {
    return *state_->dependencies.presets;
}

void ExportQueueService::pump() {
    auto& s = *state_;
    if (s.pumping)
        return;
    s.pumping = true;
    Pump{s, {}}.run();
    s.pumping = false;
}

ExportCommandAvailability ExportQueueService::availability(const ExportCommand command) const {
    const auto snapshot = this->snapshot();
    return snapshot->commands[command];
}

ExportCommandAvailability ExportQueueService::availability(const ExportJobId job,
                                                           const ExportCommand command) const {
    const auto snapshot = this->snapshot();
    for (const auto& view : snapshot->jobs)
        if (view.id == job)
            return view.commands[command];
    return unknownJob();
}

ExportCommandAvailability ExportQueueService::availability(const ExportJobId job,
                                                           const ExportOutputId output,
                                                           const ExportCommand command) const {
    const auto snapshot = this->snapshot();
    for (const auto& view : snapshot->jobs)
        if (view.id == job) {
            for (const auto& outputView : view.outputs)
                if (outputView.id == output)
                    return outputView.commands[command];
            return {false, ExportUnavailableCode::UnknownOutput, "The output does not exist"};
        }
    return unknownJob();
}

namespace {
ExportCommandResult refused(ExportCommandAvailability availability, const std::uint64_t revision) {
    return {false, std::move(availability), revision};
}
} // namespace

ExportCommandResult ExportQueueService::start(const std::span<const ExportJobId> jobs) {
    auto& s = *state_;
    std::optional<ExportCommandAvailability> refusal;
    bool accepted = false;
    for (const auto id : jobs) {
        const auto allowed = availability(id, ExportCommand::Start);
        if (!allowed) {
            if (!refusal)
                refusal = allowed;
            continue;
        }
        auto& job = s.jobs[id];
        job.state = ExportJobState::Waiting;
        job.failure.reset();
        job.stopCause.reset();
        s.waiting.push_back(id);
        s.runner = ExportRunnerState::Running;
        accepted = true;
    }
    pump();
    if (!accepted)
        return refused(refusal.value_or(unknownJob()), snapshot()->revision);
    return {true, {true, ExportUnavailableCode::None, {}}, snapshot()->revision};
}

ExportCommandResult ExportQueueService::startAll() {
    const auto allowed = availability(ExportCommand::StartAll);
    if (!allowed)
        return refused(allowed, snapshot()->revision);
    std::vector<ExportJobId> jobs;
    for (const auto& view : snapshot()->jobs)
        if (view.record.enabled && view.commands[ExportCommand::Start])
            jobs.push_back(view.id);
    return start(jobs);
}

namespace {
template <typename Apply>
ExportCommandResult jobCommand(ExportQueueService& service, ExportQueueService::State& state,
                               const ExportJobId id, const ExportCommand command, Apply&& apply) {
    const auto allowed = service.availability(id, command);
    if (!allowed)
        return refused(allowed, service.snapshot()->revision);
    apply(state.jobs[id]);
    service.pump();
    return {true, {true, ExportUnavailableCode::None, {}}, service.snapshot()->revision};
}
} // namespace

ExportCommandResult ExportQueueService::pause(const ExportJobId job) {
    return jobCommand(*this, *state_, job, ExportCommand::Pause, [](detail::JobRuntime& runtime) {
        runtime.state = ExportJobState::Pausing;
        if (runtime.runner)
            runtime.runner->setPaused(true);
    });
}

ExportCommandResult ExportQueueService::resume(const ExportJobId job) {
    return jobCommand(*this, *state_, job, ExportCommand::Resume, [](detail::JobRuntime& runtime) {
        runtime.state = ExportJobState::Rendering;
        if (runtime.runner)
            runtime.runner->setPaused(false);
    });
}

ExportCommandResult ExportQueueService::stop(const ExportJobId job) {
    auto& s = *state_;
    return jobCommand(*this, s, job, ExportCommand::Stop, [&](detail::JobRuntime& runtime) {
        if (runtime.state == ExportJobState::Waiting) {
            std::erase(s.waiting, job);
            runtime.state = ExportJobState::Ready;
            return;
        }
        runtime.pendingStopCause = ExportStopCause::User;
        runtime.state = ExportJobState::Stopping;
        for (auto& [id, sample] : runtime.preparingSamples)
            sample.cancel();
        if (runtime.runner)
            runtime.runner->stop();
    });
}

ExportCommandResult ExportQueueService::stopAll() {
    const auto allowed = availability(ExportCommand::StopAll);
    if (!allowed)
        return refused(allowed, snapshot()->revision);
    for (const auto& view : snapshot()->jobs)
        if (view.commands[ExportCommand::Stop])
            (void)stop(view.id);
    return {true, {true, ExportUnavailableCode::None, {}}, snapshot()->revision};
}

ExportCommandResult ExportQueueService::retry(const ExportJobId job) {
    const auto allowed = availability(job, ExportCommand::Retry);
    if (!allowed)
        return refused(allowed, snapshot()->revision);
    const ExportJobId jobs[] = {job};
    auto& runtime = state_->jobs[job];
    runtime.state = ExportJobState::Ready;
    runtime.viewKey.clear();
    // Start re-checks every condition against the snapshot this pump publishes.
    pump();
    return start(jobs);
}

ExportCommandResult ExportQueueService::confirmReplace(const ExportJobId job,
                                                       const ExportOutputId output) {
    auto& s = *state_;
    const auto allowed = availability(job, output, ExportCommand::ConfirmReplace);
    if (!allowed)
        return refused(allowed, snapshot()->revision);
    if (const auto sample = s.samples.find(output);
        sample != s.samples.end() && sample->second.fingerprint)
        s.confirmations[output] = *sample->second.fingerprint;
    pump();
    return {true, {true, ExportUnavailableCode::None, {}}, snapshot()->revision};
}

ExportTransientSubmitResult ExportQueueService::submitTransient(ExportTransientJobRequest request) {
    auto& s = *state_;
    pump();
    ExportTransientSubmitResult result;
    const auto refuse = [&](const ExportUnavailableCode code, std::string reason) {
        result.availability = {false, code, std::move(reason)};
        result.revision = snapshot()->revision;
        return result;
    };
    const auto catalogue = s.dependencies.presets->snapshot();
    if (!catalogue->encoders.exportPlatform)
        return refuse(ExportUnavailableCode::PlatformNotSupported,
                      "Export runs on Linux in this version");
    if (s.runner == ExportRunnerState::ShuttingDown)
        return refuse(ExportUnavailableCode::ShuttingDown, "Bloom is shutting down");
    if (!s.snapshot)
        return refuse(ExportUnavailableCode::NoProject, "No project is open");
    if (!s.snapshot->project().findComposition(request.composition))
        return refuse(ExportUnavailableCode::CompositionMissing, "The composition does not exist");
    document::ExportJob job;
    job.id = ExportJobId::fromRaw(s.nextTransient++);
    job.composition = request.composition;
    job.name = std::move(request.name);
    job.render.range = request.range;
    document::ExportOutput output;
    output.id = ExportOutputId::fromRaw(s.nextTransient++);
    output.presetId = request.presetId.empty() ? std::string("transient") : request.presetId;
    output.settings = std::move(request.settings);
    output.destination = std::move(request.destination);
    job.outputs.push_back(output);
    if (const auto validation = document::validateExportJob(job); !validation.ok())
        return refuse(ExportUnavailableCode::PreflightBlocked, validation.issues().front().message);
    if (!resolveExportRange(job, s.snapshot->project()))
        return refuse(ExportUnavailableCode::PreflightBlocked,
                      "The range lies outside the composition");
    auto availability = exportPlatformAvailability(output.settings, catalogue->encoders);
    if (availability && s.colorSettings)
        availability = exportProjectAvailability(output.settings, *s.colorSettings);
    if (!availability)
        return refuse(availability.code == ExportAvailabilityCode::PlatformNotSupported
                          ? ExportUnavailableCode::PlatformNotSupported
                          : ExportUnavailableCode::PreflightBlocked,
                      availability.reason);
    const auto resolved =
        resolveDestination(job, output, detail::destinationContext(s, job, nullptr));
    if (!resolved.issues.empty())
        return refuse(ExportUnavailableCode::PreflightBlocked, resolved.issues.front().reason);
    const auto id = job.id;
    s.transients.emplace(id, std::move(job));
    auto& runtime = s.jobs[id];
    runtime.transient = true;
    runtime.state = ExportJobState::Waiting;
    s.waiting.push_back(id);
    s.runner = ExportRunnerState::Running;
    pump();
    result.accepted = true;
    result.availability = {true, ExportUnavailableCode::None, {}};
    result.job = id;
    result.output = output.id;
    result.revision = snapshot()->revision;
    return result;
}

bool ExportQueueService::acknowledgeTransient(const ExportJobId job) {
    auto& s = *state_;
    const auto found = s.jobs.find(job);
    if (!s.transients.contains(job) || found == s.jobs.end() || !isTerminal(found->second.state) ||
        found->second.terminalEventPending)
        return false;
    s.acknowledged.insert(job);
    pump();
    return true;
}

void ExportQueueService::refreshPreflight() {
    state_->refreshRequested = true;
    pump();
}

void ExportQueueService::setInteractiveGestureActive(const bool active) {
    state_->gestureActive = active;
    if (!active)
        pump();
}

void ExportQueueService::beginShutdown(const ExportStopCause cause) {
    auto& s = *state_;
    s.runner = ExportRunnerState::ShuttingDown;
    s.shutdownCause = cause;
    for (const auto id : std::exchange(s.waiting, {}))
        s.jobs[id].state = ExportJobState::Ready;
    for (auto& [id, job] : s.jobs)
        if (isRunning(job.state) && job.state != ExportJobState::Stopping) {
            job.pendingStopCause = cause;
            job.state = ExportJobState::Stopping;
            for (auto& [output, sample] : job.preparingSamples)
                sample.cancel();
            if (job.runner)
                job.runner->stop();
        }
    // Preflight samples stop too; quiescent() waits for their workers to answer, so nothing this
    // service submitted outlives it.
    for (auto& pending : s.pendingSamples)
        pending.handle.cancel();
    pump();
}

bool ExportQueueService::quiescent() const noexcept {
    const auto& s = *state_;
    if (s.active || !s.pendingSamples.empty())
        return false;
    for (const auto& [id, job] : s.jobs)
        if (job.runner || job.terminalEventPending)
            return false;
    return true;
}

std::size_t ExportQueueService::activeJobCount() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        state_->jobs, [](const auto& entry) { return isActive(entry.second.state); }));
}

} // namespace bloom::host
