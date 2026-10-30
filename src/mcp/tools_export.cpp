// The export.* session tools and the transient-job route of render, export and capture (ADR 0034,
// section 5; ADR 0033 section 13). Every export queue call runs on the queue's owner thread through
// McpExportAccess; the request worker only waits.
#include "core_internal.hpp"

#include <bloom/core/file_uri.hpp>
#include <bloom/host/export_presets.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace bloom::mcp {
namespace {

using namespace std::chrono_literals;

std::string_view jobStateName(const host::ExportJobState state) {
    constexpr std::array<std::string_view, 13> names{
        "",          "ready",    "waiting", "preparing",          "rendering", "pausing", "paused",
        "verifying", "stopping", "done",    "done-with-warnings", "failed",    "stopped"};
    const auto index = static_cast<std::size_t>(state);
    return index < names.size() ? names[index] : "unknown";
}

std::string_view outputStateName(const host::ExportOutputState state) {
    constexpr std::array<std::string_view, 10> names{"",
                                                     "pending",
                                                     "preparing",
                                                     "rendering",
                                                     "verifying",
                                                     "published",
                                                     "published-with-warnings",
                                                     "failed",
                                                     "stopped",
                                                     "skipped"};
    const auto index = static_cast<std::size_t>(state);
    return index < names.size() ? names[index] : "unknown";
}

// By enumerator, never by position: the numeric values keep gaps (23 was retired by ADR 0035).
std::string_view unavailableName(const host::ExportUnavailableCode code) {
    using Code = host::ExportUnavailableCode;
    switch (code) {
    case Code::None:
        return "None";
    case Code::NotApplicable:
        return "NotApplicable";
    case Code::ShuttingDown:
        return "ShuttingDown";
    case Code::NoProject:
        return "NoProject";
    case Code::ReadOnlyProject:
        return "ReadOnlyProject";
    case Code::UnknownJob:
        return "UnknownJob";
    case Code::UnknownOutput:
        return "UnknownOutput";
    case Code::AlreadyActive:
        return "AlreadyActive";
    case Code::NotActive:
        return "NotActive";
    case Code::NotRendering:
        return "NotRendering";
    case Code::NotPaused:
        return "NotPaused";
    case Code::NotFinished:
        return "NotFinished";
    case Code::NoEnabledOutputs:
        return "NoEnabledOutputs";
    case Code::PreflightBlocked:
        return "PreflightBlocked";
    case Code::LockedWhileRunning:
        return "LockedWhileRunning";
    case Code::LastOutput:
        return "LastOutput";
    case Code::QueueFull:
        return "QueueFull";
    case Code::OutputLimitReached:
        return "OutputLimitReached";
    case Code::NothingToStart:
        return "NothingToStart";
    case Code::NothingToStop:
        return "NothingToStop";
    case Code::NothingFinished:
        return "NothingFinished";
    case Code::NotPublished:
        return "NotPublished";
    case Code::CompositionMissing:
        return "CompositionMissing";
    case Code::NeedsReplaceConfirmation:
        return "NeedsReplaceConfirmation";
    case Code::NothingToConfirm:
        return "NothingToConfirm";
    case Code::DestinationCheckPending:
        return "DestinationCheckPending";
    }
    return "Unknown";
}

std::string_view routeName(const host::ExportDeviceRoute route) {
    switch (route) {
    case host::ExportDeviceRoute::None:
        return "none";
    case host::ExportDeviceRoute::Gpu:
        return "gpu";
    case host::ExportDeviceRoute::WaitingForViewer:
        return "waiting-for-viewer";
    case host::ExportDeviceRoute::RecoveringGpu:
        return "recovering-gpu";
    case host::ExportDeviceRoute::CpuReference:
        return "cpu-reference";
    case host::ExportDeviceRoute::WaitingForMemory:
        return "waiting-for-memory";
    }
    return "none";
}

std::string hex(const core::Sha256Digest& digest) {
    const auto text = digest.toLowercaseHex();
    return "sha256:" + std::string(text.data(), text.size());
}

// Runs `work` on the queue's owner thread; a refusal to run is a cancellation.
void onOwner(McpServerCore::State& core, const McpExportAccess::Work& work,
             const std::stop_token& stop) {
    const auto& access = core.dependencies.exports;
    if (!access.run)
        throw ToolFailure(McpToolErrorCode::ExportUnavailable,
                          "Export is not available in this window yet.");
    if (!access.run(work, stop))
        throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
}

[[noreturn]] void noQueue() {
    throw ToolFailure(McpToolErrorCode::ExportUnavailable,
                      "Export isn't available in this window yet.");
}

yyjson_mut_val* availabilityValue(Json& out, const host::ExportCommandAvailability& value) {
    auto* result = out.object();
    out.set(result, "available", out.boolean(value.available));
    out.set(result, "code", out.text(unavailableName(value.code)));
    out.set(result, "reason", out.text(value.reason));
    return result;
}

yyjson_mut_val* commandValue(Json& out, const host::ExportCommandResult& result) {
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(result.accepted));
    out.set(value, "accepted", out.boolean(result.accepted));
    out.set(value, "revision", out.number(result.revision));
    out.set(value, "availability", availabilityValue(out, result.availability));
    return value;
}

yyjson_mut_val* verificationValue(Json& out, const host::ExportOutputVerification& verification) {
    auto* value = out.object();
    constexpr std::array<std::string_view, 6> outcomes{"",       "passed",         "warning",
                                                       "failed", "not-applicable", "not-run"};
    out.set(value, "overall",
            out.text(outcomes.at(static_cast<std::size_t>(verification.overall))));
    const auto& artifacts = verification.artifacts;
    out.set(value, "publishedArtifacts", out.number(artifacts.publishedArtifacts));
    out.set(value, "firstPath", out.text(artifacts.firstPath.string()));
    out.set(value, "lastPath", out.text(artifacts.lastPath.string()));
    out.set(value, "totalBytes", out.number(artifacts.totalBytes));
    out.set(value, "digest", artifacts.digest ? out.text(hex(*artifacts.digest)) : out.null());
    out.set(value, "encoder", out.text(verification.provenance.encoder));
    out.set(value, "gpuFrames", out.number(verification.provenance.gpuFrames));
    out.set(value, "cpuReferenceFrames", out.number(verification.provenance.cpuReferenceFrames));
    return value;
}

yyjson_mut_val* jobValue(Json& out, const host::ExportJobView& job) {
    auto* value = out.object();
    out.set(value, "id", out.number(job.id.value()));
    out.set(value, "transient", out.boolean(job.transient));
    out.set(value, "name", out.text(job.displayName));
    out.set(value, "composition", out.number(job.record.composition.value()));
    out.set(value, "state", out.text(jobStateName(job.state)));
    if (job.resolvedRange) {
        auto* range = out.object();
        out.set(range, "first", out.number(job.resolvedRange->first));
        out.set(range, "last", out.number(job.resolvedRange->last));
        out.set(value, "range", range);
    }
    auto* progress = out.object();
    out.set(progress, "framesCompleted", out.number(job.progress.framesCompleted));
    out.set(progress, "framesTotal", out.number(job.progress.framesTotal));
    out.set(progress, "fraction",
            job.progress.fraction ? out.real(*job.progress.fraction) : out.null());
    out.set(progress, "route", out.text(routeName(job.progress.route)));
    out.set(progress, "elapsedMs",
            out.number(static_cast<std::uint64_t>(job.progress.elapsed.count())));
    out.set(progress, "remainingMs",
            job.progress.remaining
                ? out.number(static_cast<std::uint64_t>(job.progress.remaining->count()))
                : out.null());
    out.set(value, "progress", progress);
    if (job.failure) {
        auto* failure = out.object();
        out.set(failure, "code", out.number(static_cast<unsigned>(job.failure->code)));
        out.set(failure, "diagnosticCode", out.text(job.failure->diagnosticCode));
        out.set(failure, "reason", out.text(job.failure->reason));
        out.set(value, "failure", failure);
    }
    out.set(value, "lastLogSequence", out.number(job.lastLogSequence));
    auto* outputs = out.array();
    for (const auto& output : job.outputs) {
        auto* row = out.object();
        out.set(row, "id", out.number(output.id.value()));
        out.set(row, "state", out.text(outputStateName(output.state)));
        out.set(row, "preset", out.text(output.record.presetId));
        out.set(row, "format",
                out.text(document::exportFormatToken(output.record.settings.format)));
        out.set(row, "firstPath", out.text(output.destination.firstPath.string()));
        out.set(row, "lastPath", out.text(output.destination.lastPath.string()));
        out.set(row, "fileCount", out.number(output.destination.fileCount));
        out.set(row, "replaceConfirmed", out.boolean(output.replaceConfirmed));
        auto* preflight = out.array();
        for (const auto& issue : output.preflight)
            out.append(preflight, out.text(issue.reason));
        out.set(row, "preflight", preflight);
        if (output.verification)
            out.set(row, "verification", verificationValue(out, *output.verification));
        out.append(outputs, row);
    }
    out.set(value, "outputs", outputs);
    auto* commands = out.object();
    constexpr std::array<std::pair<std::string_view, host::ExportCommand>, 5> shown{{
        {"start", host::ExportCommand::Start},
        {"pause", host::ExportCommand::Pause},
        {"resume", host::ExportCommand::Resume},
        {"stop", host::ExportCommand::Stop},
        {"retry", host::ExportCommand::Retry},
    }};
    for (const auto& [name, command] : shown)
        out.set(commands, name, availabilityValue(out, job.commands[command]));
    out.set(value, "commands", commands);
    return value;
}

std::optional<host::ExportJobView> findJob(const host::ExportQueueSnapshot& snapshot,
                                           const host::ExportJobId id) {
    const auto found = std::ranges::find(snapshot.jobs, id, &host::ExportJobView::id);
    return found == snapshot.jobs.end() ? std::nullopt : std::optional(*found);
}

McpProgress progressOf(const host::ExportJobView& job) {
    McpProgress progress;
    progress.progress = static_cast<double>(job.progress.framesCompleted);
    if (job.progress.framesTotal > 0)
        progress.total = static_cast<double>(job.progress.framesTotal);
    if (job.progress.route == host::ExportDeviceRoute::WaitingForViewer)
        progress.message = "Waiting for the Viewer";
    else if (job.progress.route == host::ExportDeviceRoute::WaitingForMemory)
        progress.message =
            job.progress.memoryNeededBytes
                ? "Waiting for memory: " +
                      std::to_string((*job.progress.memoryNeededBytes + (1ULL << 20) - 1) >> 20) +
                      " MiB needed"
                : std::string("Waiting for memory");
    else if (job.state == host::ExportJobState::Rendering && job.progress.currentFrame)
        progress.message = "Rendering frame " + std::to_string(job.progress.framesCompleted + 1) +
                           " of " + std::to_string(job.progress.framesTotal);
    else
        progress.message = std::string(jobStateName(job.state));
    return progress;
}

// The files a published output wrote: one file, or the sequence between its first and last names
// (fixed-width, ascending; the same rule the destination sampler uses).
std::vector<std::filesystem::path> publishedFiles(const host::ExportArtifactSummary& artifacts) {
    if (artifacts.firstPath.empty())
        return {};
    if (artifacts.firstPath == artifacts.lastPath || artifacts.publishedArtifacts <= 1)
        return {artifacts.firstPath};
    std::vector<std::filesystem::path> files;
    const auto first = artifacts.firstPath.filename().string();
    const auto last = artifacts.lastPath.filename().string();
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(artifacts.firstPath.parent_path(), error)) {
        const auto name = entry.path().filename().string();
        if (name.size() == first.size() && name >= first && name <= last &&
            entry.path().extension() == artifacts.firstPath.extension())
            files.push_back(entry.path());
    }
    std::ranges::sort(files);
    return files;
}

// Waits on the request worker until the queue may have changed: the headless owner pumps the
// queue itself; the desktop owner pumps it on the UI thread and wakes the subscription.
void waitForQueue(McpServerCore::State& core, std::optional<host::ExportQueueSubscription>& sub,
                  std::uint64_t& observed, const std::stop_token& stop) {
    if (core.dependencies.exports.callerIsOwner || !sub) {
        std::this_thread::sleep_for(core.dependencies.exports.callerIsOwner ? 5ms : 20ms);
        return;
    }
    if (const auto serial = sub->waitForChange(observed, stop))
        observed = *serial;
    else if (!stop.stop_requested()) {
        // Closed: the queue was replaced or is shutting down. Read again; the generation check
        // reports it.
        sub.reset();
        std::this_thread::sleep_for(20ms);
    }
}

} // namespace

yyjson_mut_val* logEntries(Json& out, const std::vector<host::ExportLogEntry>& entries) {
    auto* rows = out.array();
    constexpr std::array<std::string_view, 4> severities{"information", "warning", "error",
                                                         "fatal"};
    for (const auto& entry : entries) {
        auto* row = out.object();
        out.set(row, "sequence", out.number(entry.sequence));
        out.set(row, "sinceStartMs",
                out.number(static_cast<std::uint64_t>(entry.sinceStart.count())));
        const auto severity = static_cast<std::size_t>(entry.severity);
        out.set(row, "severity",
                out.text(severity < severities.size() ? severities[severity] : "information"));
        out.set(row, "code", out.text(entry.code));
        out.set(row, "frame", entry.frame ? out.number(*entry.frame) : out.null());
        out.set(row, "message", out.text(entry.message));
        out.append(rows, row);
    }
    return rows;
}

document::ExportDestination transientDestination(const std::string& path, yyjson_val* overwrite) {
    document::ExportDestination destination;
    const std::filesystem::path file(path);
    if (file.is_absolute()) {
        const auto uri = core::fileUriFromAbsolutePath(file);
        if (!uri)
            throw InvalidInput("The destination is not a valid absolute path");
        destination.portability = "external";
        destination.path = *uri;
    } else {
        destination.portability = "project-relative";
        destination.path = file.generic_string();
    }
    const auto policy = overwrite ? string(overwrite, 16) : std::string("ask");
    if (policy == "replace")
        destination.overwrite = document::ExportOverwritePolicy::Replace;
    else if (policy == "refuse")
        destination.overwrite = document::ExportOverwritePolicy::Refuse;
    else if (policy == "ask")
        destination.overwrite = document::ExportOverwritePolicy::AskBeforeReplacing;
    else
        throw InvalidInput("overwrite is ask, replace or refuse");
    return destination;
}

void McpServerCore::State::retire(RetiredTransient job) {
    const std::lock_guard lock(retiredMutex);
    std::erase_if(retired, [&](const RetiredTransient& entry) {
        return entry.generation != job.generation || entry.view.id == job.view.id;
    });
    retired.push_back(std::move(job));
    while (retired.size() > kRetiredTransientJobs)
        retired.pop_front();
}

std::vector<McpServerCore::State::RetiredTransient>
McpServerCore::State::retiredJobs(const std::uint64_t generation) {
    const std::lock_guard lock(retiredMutex);
    std::erase_if(retired,
                  [&](const RetiredTransient& entry) { return entry.generation != generation; });
    return {retired.begin(), retired.end()};
}

TransientResult runTransientJob(McpServerCore::State& core, McpCall& call,
                                host::ExportTransientJobRequest request) {
    const auto& stop = call.control.stop;
    host::ExportTransientSubmitResult submitted;
    std::optional<host::ExportQueueSubscription> sub;
    std::uint64_t generation = 0;
    bool missing = false;
    onOwner(
        core,
        [&](host::ExportQueueService* queue, const std::uint64_t queueGeneration) {
            if (queue == nullptr) {
                missing = true;
                return;
            }
            generation = queueGeneration;
            submitted = queue->submitTransient(std::move(request));
            if (submitted.accepted && !core.dependencies.exports.callerIsOwner) {
                auto subscribed = queue->subscribe();
                if (subscribed.status == host::ExportQueueSubscribeStatus::Accepted)
                    sub.emplace(std::move(subscribed.subscription));
            }
        },
        stop);
    if (missing)
        noQueue();
    if (!submitted.accepted || !submitted.job)
        throw ToolFailure(submitted.availability.code ==
                                  host::ExportUnavailableCode::PreflightBlocked
                              ? McpToolErrorCode::PreflightBlocked
                              : McpToolErrorCode::ExportUnavailable,
                          submitted.availability.reason,
                          std::string(unavailableName(submitted.availability.code)));
    const auto job = *submitted.job;
    std::uint64_t observed = sub ? sub->serial() : 0;
    bool stopping = false;
    while (true) {
        std::optional<host::ExportJobView> view;
        bool replaced = false;
        // Reads are never cancelled: a stopped request still follows its job to the end.
        const auto read = [&](host::ExportQueueService* queue, const std::uint64_t current) {
            if (queue == nullptr || current != generation) {
                replaced = true;
                return;
            }
            if (core.dependencies.exports.callerIsOwner)
                queue->pump();
            view = findJob(*queue->snapshot(), job);
            if (!sub && !core.dependencies.exports.callerIsOwner) {
                auto subscribed = queue->subscribe();
                if (subscribed.status == host::ExportQueueSubscribeStatus::Accepted) {
                    sub.emplace(std::move(subscribed.subscription));
                    observed = sub->serial();
                }
            }
        };
        if (!core.dependencies.exports.run(read, {}))
            throw ToolFailure(McpToolErrorCode::Cancelled, "The server stopped; the job stopped.");
        if (replaced || !view)
            throw ToolFailure(McpToolErrorCode::Cancelled,
                              "The export queue was replaced; the job stopped.");
        if (call.control.progress && !host::isTerminal(view->state))
            call.control.progress(progressOf(*view));
        if (host::isTerminal(view->state)) {
            // The run's log is read before the result is acknowledged (which retires the job), so
            // the call's result carries the detail the job's log had -- success or failure.
            std::vector<host::ExportLogEntry> log;
            core.dependencies.exports.run(
                [&](host::ExportQueueService* queue, const std::uint64_t current) {
                    if (queue == nullptr || current != generation)
                        return;
                    auto batch = queue->log(job, 0);
                    // Read after the call returns (export.status, export.log, export.wait).
                    core.retire({generation, *view, batch});
                    log = std::move(batch.entries);
                    if (log.size() > kTransientLogEntries)
                        log.erase(log.begin(),
                                  log.end() - static_cast<std::ptrdiff_t>(kTransientLogEntries));
                    for (auto& entry : log)
                        if (entry.message.size() > 1024)
                            entry.message.resize(1024);
                    static_cast<void>(queue->acknowledgeTransient(job));
                },
                {});
            const auto fail = [&](const McpToolErrorCode code, std::string message,
                                  std::string detail) {
                ToolFailure failure(code, message, std::move(detail));
                failure.job = job.value();
                failure.log = std::make_shared<std::vector<host::ExportLogEntry>>(log);
                return failure;
            };
            if (view->state == host::ExportJobState::Stopped || stopping)
                throw fail(McpToolErrorCode::Cancelled,
                           "The request was cancelled; the job stopped.", {});
            if (view->state == host::ExportJobState::Failed)
                throw fail(McpToolErrorCode::ExportFailed,
                           view->failure ? view->failure->reason : "The export failed.",
                           view->failure ? view->failure->diagnosticCode : std::string{});
            TransientResult result;
            result.job = job.value();
            result.log = std::move(log);
            for (const auto& output : view->outputs) {
                if (!output.verification)
                    continue;
                result.frames += output.verification->artifacts.publishedArtifacts;
                for (auto& file : publishedFiles(output.verification->artifacts))
                    result.files.push_back(std::move(file));
            }
            result.frames = std::max<std::uint64_t>(result.frames, view->progress.framesCompleted);
            return result;
        }
        if (stop.stop_requested() && !stopping) {
            stopping = true;
            core.dependencies.exports.run(
                [&](host::ExportQueueService* queue, const std::uint64_t current) {
                    if (queue != nullptr && current == generation)
                        static_cast<void>(queue->stop(job));
                },
                {});
            continue;
        }
        waitForQueue(core, sub, observed, stopping ? std::stop_token{} : stop);
    }
}

yyjson_mut_val* exportSessionTool(McpServerCore::State& core, Json& out, McpCall& call,
                                  const McpTool tool, yyjson_val* arguments) {
    const auto& stop = call.control.stop;
    const auto jobArgument = [&](const char* name = "job") {
        return host::ExportJobId::fromRaw(integer(member(arguments, name)));
    };
    yyjson_mut_val* result = nullptr;
    bool missing = false;
    const auto command = [&](auto&& issue) {
        onOwner(
            core,
            [&](host::ExportQueueService* queue, std::uint64_t) {
                if (queue == nullptr) {
                    missing = true;
                    return;
                }
                result = commandValue(out, issue(*queue));
            },
            stop);
        if (missing)
            noQueue();
        return result;
    };
    switch (tool) {
    case McpTool::ExportStart: {
        members(arguments, {"jobs"}, {"jobs"});
        auto* list = member(arguments, "jobs");
        if (!yyjson_is_arr(list) || yyjson_arr_size(list) == 0 || yyjson_arr_size(list) > 1024)
            throw InvalidInput("jobs is a list of 1..1024 job ids");
        std::vector<host::ExportJobId> jobs;
        yyjson_arr_iter iterator = yyjson_arr_iter_with(list);
        while (auto* item = yyjson_arr_iter_next(&iterator))
            jobs.push_back(host::ExportJobId::fromRaw(integer(item)));
        call.summary = std::to_string(jobs.size()) + " job(s)";
        return command([&](host::ExportQueueService& queue) { return queue.start(jobs); });
    }
    case McpTool::ExportStartAll:
        if (arguments)
            members(arguments, {});
        return command([](host::ExportQueueService& queue) { return queue.startAll(); });
    case McpTool::ExportStopAll:
        if (arguments)
            members(arguments, {});
        return command([](host::ExportQueueService& queue) { return queue.stopAll(); });
    case McpTool::ExportPause:
    case McpTool::ExportResume:
    case McpTool::ExportStop:
    case McpTool::ExportRetry: {
        members(arguments, {"job"}, {"job"});
        const auto job = jobArgument();
        call.summary = "job " + std::to_string(job.value());
        return command([&](host::ExportQueueService& queue) {
            switch (tool) {
            case McpTool::ExportPause:
                return queue.pause(job);
            case McpTool::ExportResume:
                return queue.resume(job);
            case McpTool::ExportStop:
                return queue.stop(job);
            default:
                return queue.retry(job);
            }
        });
    }
    case McpTool::ExportConfirmReplace: {
        members(arguments, {"job", "output"}, {"job", "output"});
        const auto job = jobArgument();
        const auto output = host::ExportOutputId::fromRaw(integer(member(arguments, "output")));
        return command(
            [&](host::ExportQueueService& queue) { return queue.confirmReplace(job, output); });
    }
    default:
        break;
    }

    // Reads.
    if (tool == McpTool::ExportStatus || tool == McpTool::ExportEvents ||
        tool == McpTool::ExportLog || tool == McpTool::ExportPresets ||
        tool == McpTool::ExportScript) {
        if (arguments == nullptr)
            throw InvalidInput("Expected an argument object");
        switch (tool) {
        case McpTool::ExportStatus:
            members(arguments, {"sinceRevision"});
            break;
        case McpTool::ExportEvents:
            members(arguments, {"afterSequence"});
            break;
        case McpTool::ExportLog:
            members(arguments, {"job", "afterSequence"}, {"job"});
            break;
        case McpTool::ExportScript:
            members(arguments, {"job", "format"}, {"job"});
            break;
        default:
            members(arguments, {});
            break;
        }
        std::optional<std::uint64_t> since;
        if (auto* value = member(arguments, "sinceRevision"))
            since = integer(value);
        const auto after = member(arguments, "afterSequence")
                               ? integer(member(arguments, "afterSequence"))
                               : std::uint64_t{0};
        std::string scriptFormat = "cli-json";
        if (auto* format = member(arguments, "format"))
            scriptFormat = string(format, 16);
        if (tool == McpTool::ExportScript && scriptFormat != "cli-json" && scriptFormat != "python")
            throw InvalidInput("format is cli-json or python");
        onOwner(
            core,
            [&](host::ExportQueueService* queue, const std::uint64_t generation) {
                if (queue == nullptr) {
                    missing = true;
                    return;
                }
                if (core.dependencies.exports.callerIsOwner)
                    queue->pump();
                result = out.object();
                switch (tool) {
                case McpTool::ExportStatus: {
                    const auto snapshot = queue->snapshot();
                    out.set(result, "revision", out.number(snapshot->revision));
                    if (since && *since == snapshot->revision) {
                        out.set(result, "unchanged", out.boolean(true));
                        break;
                    }
                    out.set(result, "documentRevision",
                            snapshot->documentRevision ? out.number(*snapshot->documentRevision)
                                                       : out.null());
                    constexpr std::array<std::string_view, 4> runners{"", "idle", "running",
                                                                      "shutting-down"};
                    out.set(result, "runner",
                            out.text(runners.at(static_cast<std::size_t>(snapshot->runner))));
                    out.set(result, "activeJob",
                            snapshot->activeJob ? out.number(snapshot->activeJob->value())
                                                : out.null());
                    auto* jobs = out.array();
                    for (const auto& job : snapshot->jobs)
                        out.append(jobs, jobValue(out, job));
                    out.set(result, "jobs", jobs);
                    auto* retired = out.array();
                    for (const auto& job : core.retiredJobs(generation))
                        if (!findJob(*snapshot, job.view.id))
                            out.append(retired, jobValue(out, job.view));
                    out.set(result, "recentTransientJobs", retired);
                    auto* commands = out.object();
                    out.set(
                        commands, "startAll",
                        availabilityValue(out, snapshot->commands[host::ExportCommand::StartAll]));
                    out.set(
                        commands, "stopAll",
                        availabilityValue(out, snapshot->commands[host::ExportCommand::StopAll]));
                    out.set(result, "commands", commands);
                    break;
                }
                case McpTool::ExportEvents: {
                    const auto batch = queue->events(after);
                    out.set(result, "gap", out.boolean(batch.gap));
                    out.set(result, "lastSequence", out.number(batch.lastSequence));
                    auto* events = out.array();
                    for (const auto& event : batch.events) {
                        auto* row = out.object();
                        out.set(row, "sequence", out.number(event.sequence));
                        std::visit(
                            [&](const auto& payload) {
                                using T = std::decay_t<decltype(payload)>;
                                if constexpr (std::is_same_v<T, host::ExportJobSucceeded>) {
                                    out.set(row, "kind", out.text("succeeded"));
                                    out.set(row, "job", out.number(payload.job.value()));
                                    out.set(row, "withWarnings", out.boolean(payload.withWarnings));
                                } else if constexpr (std::is_same_v<T, host::ExportJobFailed>) {
                                    out.set(row, "kind", out.text("failed"));
                                    out.set(row, "job", out.number(payload.job.value()));
                                    out.set(row, "reason", out.text(payload.reason.reason));
                                } else if constexpr (std::is_same_v<T, host::ExportJobCancelled>) {
                                    out.set(row, "kind", out.text("cancelled"));
                                    out.set(row, "job", out.number(payload.job.value()));
                                } else {
                                    out.set(row, "kind", out.text("drained"));
                                    out.set(row, "succeeded", out.number(payload.succeeded));
                                    out.set(row, "failed", out.number(payload.failed));
                                    out.set(row, "cancelled", out.number(payload.cancelled));
                                }
                            },
                            event.payload);
                        out.append(events, row);
                    }
                    out.set(result, "events", events);
                    break;
                }
                case McpTool::ExportLog: {
                    const auto job = jobArgument();
                    auto batch = queue->log(job, after);
                    // A finished transient job the queue already retired.
                    if (!findJob(*queue->snapshot(), job))
                        for (auto& retired : core.retiredJobs(generation))
                            if (retired.view.id == job) {
                                batch = std::move(retired.log);
                                batch.gap = batch.gap && !batch.entries.empty() &&
                                            after + 1 < batch.entries.front().sequence;
                                std::erase_if(batch.entries, [&](const auto& entry) {
                                    return entry.sequence <= after;
                                });
                            }
                    out.set(result, "gap", out.boolean(batch.gap));
                    out.set(result, "lastSequence", out.number(batch.lastSequence));
                    auto* entries = logEntries(out, batch.entries);
                    out.set(result, "entries", entries);
                    break;
                }
                case McpTool::ExportPresets: {
                    const auto catalogue = queue->presets().snapshot();
                    out.set(result, "revision", out.number(catalogue->revision));
                    auto* presets = out.array();
                    for (const auto& preset : catalogue->presets) {
                        auto* row = out.object();
                        out.set(row, "id", out.text(preset.id));
                        out.set(row, "name", out.text(preset.displayName));
                        out.set(row, "description", out.text(preset.description));
                        out.set(row, "available",
                                out.boolean(preset.platformAvailability.available));
                        out.set(row, "reason", out.text(preset.platformAvailability.reason));
                        Parsed settings(host::exportSettingsToJson(preset.settings));
                        out.set(row, "settings", out.copy(settings.root()));
                        out.append(presets, row);
                    }
                    out.set(result, "presets", presets);
                    break;
                }
                default: {
                    const auto job = findJob(*queue->snapshot(), jobArgument());
                    if (!job || job->transient)
                        throw InvalidInput("Unknown export job");
                    out.set(result, "format", out.text(scriptFormat));
                    out.set(result, "script",
                            out.text(host::exportJobScript(
                                job->record, scriptFormat == "python"
                                                 ? host::ExportScriptFormat::Python
                                                 : host::ExportScriptFormat::CliJson)));
                    break;
                }
                }
            },
            stop);
        if (missing)
            noQueue();
        return result;
    }

    // export.wait: a task tool.
    if (arguments)
        members(arguments, {"job", "timeoutMs"});
    std::optional<host::ExportJobId> job;
    if (arguments && member(arguments, "job"))
        job = jobArgument();
    const auto timeout = std::chrono::milliseconds(arguments && member(arguments, "timeoutMs")
                                                       ? integer(member(arguments, "timeoutMs"))
                                                       : std::uint64_t{3600000});
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::optional<host::ExportQueueSubscription> sub;
    std::uint64_t observed = 0;
    while (true) {
        bool done = false;
        bool unknown = false;
        std::optional<host::ExportJobView> view;
        std::uint64_t revision = 0;
        onOwner(
            core,
            [&](host::ExportQueueService* queue, const std::uint64_t generation) {
                if (queue == nullptr) {
                    missing = true;
                    return;
                }
                if (core.dependencies.exports.callerIsOwner)
                    queue->pump();
                const auto snapshot = queue->snapshot();
                revision = snapshot->revision;
                if (job) {
                    view = findJob(*snapshot, *job);
                    if (!view)
                        for (const auto& retired : core.retiredJobs(generation))
                            if (retired.view.id == *job)
                                view = retired.view;
                    unknown = !view;
                    done = view && !host::isActive(view->state);
                } else {
                    done = snapshot->runner != host::ExportRunnerState::Running;
                }
                if (!sub && !core.dependencies.exports.callerIsOwner) {
                    auto subscribed = queue->subscribe();
                    if (subscribed.status == host::ExportQueueSubscribeStatus::Accepted) {
                        sub.emplace(std::move(subscribed.subscription));
                        observed = sub->serial();
                    }
                }
            },
            stop);
        if (missing)
            noQueue();
        if (unknown)
            throw InvalidInput("Unknown export job");
        if (view && call.control.progress && !done)
            call.control.progress(progressOf(*view));
        if (done || std::chrono::steady_clock::now() >= deadline) {
            auto* value = out.object();
            out.set(value, "finished", out.boolean(done));
            out.set(value, "timedOut", out.boolean(!done));
            out.set(value, "revision", out.number(revision));
            if (view)
                out.set(value, "job", jobValue(out, *view));
            return value;
        }
        if (stop.stop_requested())
            throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
        // Bounded by the request's own timeout: wake at the deadline even if the queue is idle.
        std::stop_source wake;
        std::jthread timer([&wake, deadline](const std::stop_token& cancelTimer) {
            std::mutex mutex;
            std::condition_variable_any idle;
            std::unique_lock lock(mutex);
            idle.wait_until(lock, cancelTimer, deadline, [] { return false; });
            wake.request_stop();
        });
        const std::stop_callback forward(stop, [&wake] { wake.request_stop(); });
        waitForQueue(core, sub, observed, wake.get_token());
    }
}

} // namespace bloom::mcp
