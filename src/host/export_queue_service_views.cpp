// ExportQueueService: destination samples, preflight, availability and the published snapshot
// (ADR 0033 sections 7, 11 and 12).
#include "export_output_drivers.hpp"
#include "export_queue_service_state.hpp"

#include <bloom/document/project.hpp>
#include <bloom/platform/destination_facts.hpp>

#include <algorithm>
#include <ctime>
#include <sstream>

namespace bloom::host::detail {
namespace {

using document::ExportFormat;

constexpr std::string_view kLockedReason =
    "Stop the job to change it; it keeps the settings it started with";

ExportCommandAvailability yes() { return {true, ExportUnavailableCode::None, {}}; }
ExportCommandAvailability no(const ExportUnavailableCode code, std::string reason) {
    return {false, code, std::move(reason)};
}
ExportCommandAvailability notApplicable() {
    return no(ExportUnavailableCode::NotApplicable, "Not available here");
}

ExportCommandAvailabilitySet allNotApplicable() {
    ExportCommandAvailabilitySet set;
    for (auto& entry : set.entries)
        entry = notApplicable();
    return set;
}

void put(ExportCommandAvailabilitySet& set, const ExportCommand command,
         ExportCommandAvailability value) {
    set.entries[static_cast<std::size_t>(command)] = std::move(value);
}

const document::Composition* compositionOf(const ExportQueueService::State& state,
                                           const document::ExportJob& job) {
    return state.snapshot ? state.snapshot->project().findComposition(job.composition) : nullptr;
}

// Bits per pixel per frame of the intra codecs at their published target rates; used only for
// the size estimate shown before Start.
double intraBitsPerPixel(const document::ExportOutputSettings& settings) {
    const auto& profile = settings.codecProfile;
    if (settings.format == ExportFormat::ProResMov)
        return profile == "proxy"    ? 0.91
               : profile == "lt"     ? 2.05
               : profile == "422"    ? 2.95
               : profile == "4444"   ? 5.31
               : profile == "4444xq" ? 7.96
                                     : 3.54;
    return profile == "lb" ? 0.73 : profile == "sq" ? 2.33 : profile == "444" ? 7.08 : 3.54;
}

} // namespace

std::vector<const document::ExportJob*> allRecords(const ExportQueueService::State& state) {
    std::vector<const document::ExportJob*> records;
    if (state.snapshot)
        for (const auto& job : state.snapshot->project().exportJobs())
            records.push_back(&job);
    for (const auto& [id, job] : state.transients)
        records.push_back(&job);
    return records;
}

const document::ExportJob* findRecord(const ExportQueueService::State& state,
                                      const ExportJobId id) {
    if (const auto found = state.transients.find(id); found != state.transients.end())
        return &found->second;
    return state.snapshot ? state.snapshot->project().findExportJob(id) : nullptr;
}

ExportDestinationContext destinationContext(const ExportQueueService::State& state,
                                            const document::ExportJob& job,
                                            const JobRuntime* runtime) {
    ExportDestinationContext context;
    context.projectFolder = state.projectFolder;
    if (const auto* composition = compositionOf(state, job))
        context.compositionName = composition->name();
    if (state.snapshot)
        context.range = resolveExportRange(job, state.snapshot->project());
    if (runtime && runtime->dateFrozen) {
        context.year = runtime->year;
        context.month = runtime->month;
        context.day = runtime->day;
    } else {
        const auto now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        context.year = static_cast<std::uint16_t>(local.tm_year + 1900);
        context.month = static_cast<std::uint8_t>(local.tm_mon + 1);
        context.day = static_cast<std::uint8_t>(local.tm_mday);
    }
    return context;
}

runtime::TaskHandle<std::shared_ptr<const DestinationSample>>
submitDestinationSample(runtime::TaskScheduler& scheduler,
                        const std::shared_ptr<ExportRunBackend>& backend, const SampleKey& key,
                        const std::optional<runtime::TaskGroupId> group,
                        const std::optional<runtime::TaskOwner> owner, const bool forRun) {
    runtime::TaskRequest request(
        "Check export destination",
        owner.value_or(
            runtime::TaskOwner{runtime::TaskOwnerKind::Export, runtime::TaskOwnerId::fromRaw(1)}),
        runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo);
    request.groupId = group;
    auto submission = scheduler.submit<std::shared_ptr<const DestinationSample>>(
        std::move(request), [backend, key, forRun](runtime::TaskContext& context) {
            using Result = runtime::TaskResult<std::shared_ptr<const DestinationSample>>;
            if (context.isCancellationRequested())
                return Result::cancelled();
            auto sample =
                backend->sampleDestination({key.firstPath, key.lastPath, key.fileCount, forRun});
            return Result::succeeded(std::make_shared<const DestinationSample>(std::move(sample)));
        });
    return std::move(submission.handle);
}

std::optional<std::uint64_t> estimateBytes(const document::ExportOutputSettings& settings,
                                           const document::Composition& composition,
                                           const ExportResolvedRange& range) {
    const auto frames = range.last - range.first + 1;
    const auto rate = composition.format().frameRate();
    if (rate.numerator() == 0)
        return std::nullopt;
    const double seconds = static_cast<double>(frames) * rate.denominator() / rate.numerator();
    const double pixels =
        static_cast<double>(composition.format().width()) * composition.format().height();
    const double audio = settings.includeAudio ? seconds * settings.audioSampleRate * 2 * 2 : 0.0;
    double bytes = 0;
    switch (settings.format) {
    case ExportFormat::H264Mov:
    case ExportFormat::H264Mp4:
    case ExportFormat::HevcMov:
    case ExportFormat::HevcMp4: {
        const auto kbps = settings.videoBitrateKbps.value_or(
            settings.format == ExportFormat::HevcMov || settings.format == ExportFormat::HevcMp4
                ? 20000
                : 30000);
        bytes = seconds * kbps * 125.0 + audio;
        break;
    }
    case ExportFormat::ProResMov:
    case ExportFormat::DnxhrMxf:
        bytes = pixels * static_cast<double>(frames) * intraBitsPerPixel(settings) / 8.0 + audio;
        break;
    case ExportFormat::Tiff:
        bytes = (pixels * 8.0 + 1024.0) * static_cast<double>(frames);
        break;
    case ExportFormat::Dpx:
        bytes = (pixels * (settings.codecProfile == "16bit" ? 6.0 : 4.0) + 2048.0) *
                static_cast<double>(frames);
        break;
    case ExportFormat::Wav:
        bytes = seconds * settings.audioSampleRate * 2 * (settings.codecProfile == "s16" ? 2 : 3);
        break;
    case ExportFormat::Png:
    case ExportFormat::OpenExr:
        // Compressed image sizes depend on content: no honest estimate exists.
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(bytes);
}

std::vector<ExportPreflightIssue>
preflightFor(const ExportQueueService::State& state, const document::ExportJob& job,
             const document::ExportOutput& output, const ExportResolvedDestination& destination,
             const std::optional<std::uint64_t> estimate, const ExportAvailability& availability) {
    std::vector<ExportPreflightIssue> issues;
    const auto blocking = [&](ExportPreflightCode code, ExportPreflightTarget target,
                              std::string reason) {
        issues.push_back({code, target, ExportPreflightSeverity::Blocking, std::move(reason)});
    };
    // Job, Composition and Range issues live on the job view (jobPreflightFor).
    if (!compositionOf(state, job))
        return issues;
    for (const auto& issue : destination.issues)
        issues.push_back(issue);
    if (!availability.available)
        blocking(ExportPreflightCode::PresetUnavailable,
                 availability.code == ExportAvailabilityCode::RequiresAcesConfig ||
                         availability.code == ExportAvailabilityCode::ConfigRevisionUnpinned ||
                         availability.code == ExportAvailabilityCode::ColourNameMissing
                     ? ExportPreflightTarget::Colour
                     : ExportPreflightTarget::Encoder,
                 availability.reason);
    if (output.enabled)
        for (const auto& other : job.outputs)
            if (other.id != output.id && other.enabled && other.destination == output.destination &&
                other.settings == output.settings) {
                blocking(ExportPreflightCode::DuplicateTarget, ExportPreflightTarget::Destination,
                         "Another output of this job writes the same file");
                break;
            }
    if (!destination.issues.empty() || destination.firstPath.empty())
        return issues;
    const SampleKey key{destination.firstPath, destination.lastPath, destination.fileCount};
    const auto keyed = state.sampleKeys.find(output.id);
    const auto sample = state.samples.find(output.id);
    if (keyed == state.sampleKeys.end() || !(keyed->second == key) ||
        sample == state.samples.end() || sample->second.firstPath != key.firstPath ||
        sample->second.lastPath != key.lastPath) {
        blocking(ExportPreflightCode::DestinationCheckPending, ExportPreflightTarget::Destination,
                 "Checking destination…");
        return issues;
    }
    const auto& facts = sample->second;
    if (!facts.folderWritable)
        blocking(ExportPreflightCode::DestinationNotWritable, ExportPreflightTarget::Destination,
                 "The destination folder cannot be written");
    if (facts.existing > 0 && facts.fingerprint) {
        const auto own = state.ownPublications.find(output.id);
        const bool ownFile =
            own != state.ownPublications.end() && own->second == *facts.fingerprint;
        const auto noun = facts.existing > 1 ? std::to_string(facts.existing) + " files exist"
                                             : std::string("The file exists");
        switch (output.destination.overwrite) {
        case document::ExportOverwritePolicy::AskBeforeReplacing:
            if (!ownFile)
                issues.push_back({ExportPreflightCode::DestinationExists,
                                  ExportPreflightTarget::Destination,
                                  ExportPreflightSeverity::NeedsConfirmation,
                                  noun + " at the destination; confirm to replace"});
            break;
        case document::ExportOverwritePolicy::Replace:
            issues.push_back({ExportPreflightCode::DestinationExists,
                              ExportPreflightTarget::Destination, ExportPreflightSeverity::Warning,
                              noun + " at the destination and will be replaced"});
            break;
        case document::ExportOverwritePolicy::Refuse:
            blocking(ExportPreflightCode::DestinationExists, ExportPreflightTarget::Destination,
                     noun + " at the destination; this output refuses to replace it");
            break;
        }
    }
    if (estimate && facts.freeBytes &&
        *estimate + platform::destinationSafetyMargin(*estimate) > *facts.freeBytes)
        blocking(ExportPreflightCode::InsufficientSpace, ExportPreflightTarget::Destination,
                 "Not enough free space at the destination for the estimated size");
    return issues;
}

std::vector<ExportPreflightIssue> jobPreflightFor(const ExportQueueService::State& state,
                                                  const document::ExportJob& job) {
    std::vector<ExportPreflightIssue> issues;
    const auto blocking = [&](ExportPreflightCode code, ExportPreflightTarget target,
                              std::string reason) {
        issues.push_back({code, target, ExportPreflightSeverity::Blocking, std::move(reason)});
    };
    if (!compositionOf(state, job))
        blocking(ExportPreflightCode::CompositionMissing, ExportPreflightTarget::Composition,
                 "The job's composition no longer exists");
    else if (!state.snapshot || !resolveExportRange(job, state.snapshot->project()))
        blocking(ExportPreflightCode::RangeOutsideComposition, ExportPreflightTarget::Range,
                 "The range lies outside the composition");
    if (std::ranges::none_of(job.outputs, [](const auto& output) { return output.enabled; }))
        blocking(ExportPreflightCode::NoEnabledOutputs, ExportPreflightTarget::Job,
                 "Enable at least one output");
    return issues;
}

std::shared_ptr<ExportQueueSnapshot> buildSnapshot(ExportQueueService::State& state) {
    auto snapshot = std::make_shared<ExportQueueSnapshot>();
    snapshot->documentRevision =
        state.snapshot ? std::optional(state.snapshot->revision().value()) : std::nullopt;
    const auto catalogue = state.dependencies.presets->snapshot();
    snapshot->catalogueRevision = catalogue->revision;
    snapshot->runner = state.runner;
    snapshot->activeJob = state.active;
    const bool shuttingDown = state.runner == ExportRunnerState::ShuttingDown;
    const auto noProject = no(ExportUnavailableCode::NoProject, "No project is open");
    const auto readOnly = no(ExportUnavailableCode::ReadOnlyProject, "The project is read-only");
    const auto jobsSpan = state.snapshot ? state.snapshot->project().exportJobs()
                                         : std::span<const document::ExportJob>{};
    const auto records = allRecords(state);
    const auto defaultColour = document::ColorSettings{};
    const auto& colour = state.colorSettings ? *state.colorSettings : defaultColour;

    bool anyStartable = false;
    bool anyActive = false;
    bool anyFinished = false;
    std::string firstStartRefusal;
    for (const auto* recordPointer : records) {
        const auto& record = *recordPointer;
        auto& runtime = state.jobs[record.id];
        ExportJobView view;
        view.id = record.id;
        view.transient = runtime.transient;
        view.record = record;
        const auto* composition = compositionOf(state, record);
        view.displayName = !record.name.empty()
                               ? record.name
                               : (composition ? composition->name() : std::string());
        view.state = runtime.state;
        const bool running = isRunning(runtime.state);
        view.resolvedRange = running && runtime.capturedRange
                                 ? runtime.capturedRange
                                 : resolveExportRange(record, state.snapshot->project());
        view.progress = runtime.progress;
        view.run = runtime.run;
        view.failure = runtime.failure;
        view.stopCause = runtime.stopCause;
        view.recordChangedSinceStart =
            running && runtime.captured && !(*runtime.captured == record);
        view.logEntryCount = runtime.logCount;
        view.lastLogSequence = runtime.lastLogSequence;
        if (!runtime.log.empty())
            view.latestLogEntry = runtime.log.back();
        const auto context = destinationContext(state, record, &runtime);
        bool startable = true;
        ExportCommandAvailability startRefusal = yes();
        const auto refuseStart = [&](ExportCommandAvailability value) {
            if (startable) {
                startable = false;
                startRefusal = std::move(value);
            }
        };
        if (shuttingDown)
            refuseStart(no(ExportUnavailableCode::ShuttingDown, "Bloom is shutting down"));
        if (isActive(runtime.state))
            refuseStart(no(ExportUnavailableCode::AlreadyActive, "The job is already running"));
        if (!composition)
            refuseStart(no(ExportUnavailableCode::CompositionMissing,
                           "The job's composition no longer exists"));
        view.preflight = jobPreflightFor(state, record);
        if (!view.resolvedRange)
            refuseStart(no(ExportUnavailableCode::PreflightBlocked,
                           "The range lies outside the composition"));
        if (std::ranges::none_of(record.outputs, [](const auto& o) { return o.enabled; }))
            refuseStart(no(ExportUnavailableCode::NoEnabledOutputs, "Enable at least one output"));
        for (const auto& output : record.outputs) {
            ExportOutputView outputView;
            outputView.id = output.id;
            outputView.record = output;
            const auto& outputRuntime = runtime.outputs[output.id];
            outputView.state = outputRuntime.state;
            outputView.verification = outputRuntime.verification;
            outputView.import = outputRuntime.import;
            outputView.import.output = output.id;
            if (!output.importIntoAssets &&
                outputView.import.status == ExportAssetImportStatus::Pending)
                outputView.import.status = ExportAssetImportStatus::NotRequested;
            outputView.destination = resolveDestination(record, output, context);
            const auto preset =
                std::ranges::find(catalogue->presets, output.presetId, &ExportPreset::id);
            outputView.presetModified =
                preset == catalogue->presets.end() || !(preset->settings == output.settings);
            outputView.availability =
                exportPlatformAvailability(output.settings, catalogue->encoders);
            if (outputView.availability)
                outputView.availability = exportProjectAvailability(output.settings, colour);
            if (composition && view.resolvedRange)
                outputView.estimatedBytes =
                    estimateBytes(output.settings, *composition, *view.resolvedRange);
            if (const auto sample = state.samples.find(output.id); sample != state.samples.end())
                outputView.freeBytesAtDestination = sample->second.freeBytes;
            outputView.preflight = preflightFor(state, record, output, outputView.destination,
                                                outputView.estimatedBytes, outputView.availability);
            const auto confirmation = state.confirmations.find(output.id);
            const auto sample = state.samples.find(output.id);
            outputView.replaceConfirmed = confirmation != state.confirmations.end() &&
                                          sample != state.samples.end() &&
                                          sample->second.fingerprint == confirmation->second;
            if (output.enabled && !isActive(runtime.state)) {
                for (const auto& issue : outputView.preflight) {
                    if (issue.code == ExportPreflightCode::DestinationCheckPending)
                        refuseStart(
                            no(ExportUnavailableCode::DestinationCheckPending, issue.reason));
                    else if (issue.severity == ExportPreflightSeverity::Blocking)
                        refuseStart(no(ExportUnavailableCode::PreflightBlocked, issue.reason));
                    else if (issue.severity == ExportPreflightSeverity::NeedsConfirmation &&
                             !outputView.replaceConfirmed)
                        refuseStart(no(ExportUnavailableCode::NeedsReplaceConfirmation,
                                       "Confirm replacing the existing destination first"));
                }
            }
            // Output-level commands.
            auto& commands = outputView.commands;
            commands = allNotApplicable();
            const auto editable = [&]() -> ExportCommandAvailability {
                if (!state.snapshot)
                    return noProject;
                if (!state.editable)
                    return readOnly;
                if (running)
                    return no(ExportUnavailableCode::LockedWhileRunning,
                              std::string(kLockedReason));
                return yes();
            };
            put(commands, ExportCommand::EditOutput, editable());
            put(commands, ExportCommand::ApplyPreset, editable());
            auto remove = editable();
            if (remove && record.outputs.size() <= 1)
                remove = no(ExportUnavailableCode::LastOutput, "A job keeps at least one output");
            put(commands, ExportCommand::RemoveOutput, remove);
            put(commands, ExportCommand::RevealOutput,
                outputRuntime.published.empty()
                    ? no(ExportUnavailableCode::NotPublished,
                         "Nothing has been published at this destination yet")
                    : yes());
            ExportCommandAvailability confirm = yes();
            const bool needs = std::ranges::any_of(outputView.preflight, [](const auto& issue) {
                return issue.severity == ExportPreflightSeverity::NeedsConfirmation;
            });
            if (running)
                confirm = no(ExportUnavailableCode::LockedWhileRunning, std::string(kLockedReason));
            else if (std::ranges::any_of(outputView.preflight, [](const auto& issue) {
                         return issue.code == ExportPreflightCode::DestinationCheckPending;
                     }))
                confirm =
                    no(ExportUnavailableCode::DestinationCheckPending, "Checking destination…");
            else if (!needs || outputView.replaceConfirmed)
                confirm = no(ExportUnavailableCode::NothingToConfirm,
                             "There is no existing destination to confirm");
            put(commands, ExportCommand::ConfirmReplace, confirm);
            view.outputs.push_back(std::move(outputView));
        }
        // Job-level commands.
        auto& commands = view.commands;
        commands = allNotApplicable();
        put(commands, ExportCommand::Start, startable ? yes() : startRefusal);
        put(commands, ExportCommand::Pause,
            runtime.state == ExportJobState::Rendering
                ? yes()
                : no(ExportUnavailableCode::NotRendering, "Only a rendering job can pause"));
        put(commands, ExportCommand::Resume,
            runtime.state == ExportJobState::Paused
                ? yes()
                : no(ExportUnavailableCode::NotPaused, "The job is not paused"));
        put(commands, ExportCommand::Stop,
            isActive(runtime.state) && runtime.state != ExportJobState::Stopping
                ? yes()
                : no(ExportUnavailableCode::NotActive, "The job is not running"));
        ExportCommandAvailability retry = startable ? yes() : startRefusal;
        if (!isTerminal(runtime.state))
            retry = isActive(runtime.state)
                        ? no(ExportUnavailableCode::AlreadyActive, "The job is already running")
                        : no(ExportUnavailableCode::NotFinished, "Only a finished job can retry");
        put(commands, ExportCommand::Retry, retry);
        const auto edit = [&]() -> ExportCommandAvailability {
            if (!state.snapshot)
                return noProject;
            if (!state.editable)
                return readOnly;
            if (running)
                return no(ExportUnavailableCode::LockedWhileRunning, std::string(kLockedReason));
            return yes();
        };
        for (const auto command :
             {ExportCommand::EditJob, ExportCommand::RemoveJob, ExportCommand::MoveJob})
            put(commands, command, edit());
        auto add = edit();
        if (add && record.outputs.size() >= document::kExportJobMaximumOutputs)
            add = no(ExportUnavailableCode::OutputLimitReached, "A job has at most 16 outputs");
        put(commands, ExportCommand::AddOutput, add);
        auto duplicate = state.editable ? yes() : readOnly;
        if (duplicate && jobsSpan.size() >= document::kExportQueueMaximumJobs)
            duplicate = no(ExportUnavailableCode::QueueFull, "The export queue is full");
        put(commands, ExportCommand::DuplicateJob, duplicate);

        anyStartable = anyStartable || (startable && record.enabled);
        if (!startable && record.enabled && firstStartRefusal.empty())
            firstStartRefusal = startRefusal.reason;
        anyActive = anyActive || isActive(runtime.state);
        anyFinished = anyFinished || runtime.state == ExportJobState::Done ||
                      runtime.state == ExportJobState::DoneWithWarnings;

        // Per-job revision: bump when anything in the view changed.
        std::ostringstream key;
        key << static_cast<int>(view.state) << '|' << view.progress.framesCompleted << '|'
            << view.progress.framesTotal << '|' << static_cast<int>(view.progress.route) << '|'
            << view.progress.outputIndex << '|' << view.logEntryCount << '|'
            << view.recordChangedSinceStart << '|' << (view.failure ? view.failure->reason : "")
            << '|' << (view.resolvedRange ? view.resolvedRange->last : 0) << '|'
            << view.progress.remaining.value_or(std::chrono::milliseconds(-1)).count() << '|'
            << snapshot->catalogueRevision;
        for (const auto& issue : view.preflight)
            key << ',' << static_cast<int>(issue.code) << static_cast<int>(issue.severity);
        for (const auto& output : view.outputs) {
            key << '#' << static_cast<int>(output.state) << ':' << output.destination.firstPath
                << ':' << output.preflight.size() << ':' << output.replaceConfirmed << ':'
                << static_cast<int>(output.import.status) << ':'
                << (output.verification ? static_cast<int>(output.verification->overall) : 0) << ':'
                << output.freeBytesAtDestination.value_or(0) << ':'
                << static_cast<int>(output.availability.code);
            for (const auto& issue : output.preflight)
                key << ',' << static_cast<int>(issue.code) << static_cast<int>(issue.severity);
        }
        for (const auto& entry : commands.entries)
            key << (entry.available ? '1' : '0');
        auto keyText = key.str();
        const bool recordChanged = runtime.viewKey.empty() || keyText != runtime.viewKey;
        if (recordChanged) {
            ++runtime.revision;
            runtime.viewKey = std::move(keyText);
        }
        view.revision = runtime.revision;
        snapshot->jobs.push_back(std::move(view));
    }
    // Queue-level commands.
    auto& commands = snapshot->commands;
    commands = allNotApplicable();
    ExportCommandAvailability add = yes();
    if (!state.snapshot)
        add = noProject;
    else if (!state.editable)
        add = readOnly;
    else if (jobsSpan.size() >= document::kExportQueueMaximumJobs)
        add = no(ExportUnavailableCode::QueueFull, "The export queue is full");
    put(commands, ExportCommand::AddJob, add);
    ExportCommandAvailability startAll =
        anyStartable
            ? yes()
            : no(ExportUnavailableCode::NothingToStart,
                 firstStartRefusal.empty() ? "No enabled job can start" : firstStartRefusal);
    if (shuttingDown)
        startAll = no(ExportUnavailableCode::ShuttingDown, "Bloom is shutting down");
    put(commands, ExportCommand::StartAll, startAll);
    put(commands, ExportCommand::StopAll,
        anyActive ? yes() : no(ExportUnavailableCode::NothingToStop, "No job is running"));
    ExportCommandAvailability removeFinished =
        anyFinished ? yes() : no(ExportUnavailableCode::NothingFinished, "No job has finished");
    if (removeFinished && !state.editable)
        removeFinished = readOnly;
    put(commands, ExportCommand::RemoveFinished, removeFinished);
    return snapshot;
}

} // namespace bloom::host::detail
