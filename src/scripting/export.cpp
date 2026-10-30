#include <bloom/scripting/export.hpp>

#include <bloom/host/export_presets.hpp>

#include <array>
#include <cstdio>
#include <thread>

namespace bloom::scripting {
namespace {

using host::ExportCommandAvailability;

class Json final {
  public:
    Json& raw(std::string_view text) {
        separate();
        out_ += text;
        return *this;
    }
    Json& key(std::string_view name) {
        separate();
        quote(name);
        out_ += ':';
        afterKey_ = true;
        return *this;
    }
    Json& string(std::string_view text) {
        separate();
        quote(text);
        return *this;
    }
    Json& number(const std::uint64_t value) { return raw(std::to_string(value)); }
    Json& boolean(const bool value) { return raw(value ? "true" : "false"); }
    Json& begin(const char bracket) {
        separate();
        out_ += bracket;
        first_ = true;
        return *this;
    }
    Json& end(const char bracket) {
        out_ += bracket;
        first_ = false;
        return *this;
    }
    [[nodiscard]] std::string take() { return std::move(out_); }

  private:
    void separate() {
        if (afterKey_) {
            afterKey_ = false;
            return;
        }
        if (!first_)
            out_ += ',';
        first_ = false;
    }
    void quote(std::string_view text) {
        out_ += '"';
        for (const char c : text) {
            if (c == '"' || c == '\\') {
                out_ += '\\';
                out_ += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                std::array<char, 8> buffer{};
                std::snprintf(buffer.data(), buffer.size(), "\\u%04x", static_cast<unsigned>(c));
                out_ += buffer.data();
            } else {
                out_ += c;
            }
        }
        out_ += '"';
    }
    std::string out_;
    bool first_ = true;
    bool afterKey_ = false;
};

std::string_view jobState(const host::ExportJobState state) {
    constexpr std::array<std::string_view, 12> names{
        "ready",     "waiting",  "preparing", "rendering",          "pausing", "paused",
        "verifying", "stopping", "done",      "done-with-warnings", "failed",  "stopped"};
    return names[static_cast<std::size_t>(state) - 1];
}

std::string_view outputState(const host::ExportOutputState state) {
    constexpr std::array<std::string_view, 9> names{
        "pending", "preparing", "rendering", "verifying", "published", "published-with-warnings",
        "failed",  "stopped",   "skipped"};
    return names[static_cast<std::size_t>(state) - 1];
}

std::string_view route(const host::ExportDeviceRoute value) {
    constexpr std::array<std::string_view, 5> names{"none", "gpu", "waiting-for-viewer",
                                                    "recovering-gpu", "cpu-reference"};
    return names[static_cast<std::size_t>(value)];
}

std::string_view outcome(const host::ExportCheckOutcome value) {
    constexpr std::array<std::string_view, 5> names{"passed", "warning", "failed", "not-applicable",
                                                    "not-run"};
    return names[static_cast<std::size_t>(value) - 1];
}

std::string_view checkName(const host::ExportCheck value) {
    constexpr std::array<std::string_view, 11> names{
        "analysis-policy",  "reopen", "frame-count", "timing",          "colour-tags",
        "container",        "audio",  "naming",      "artifact-digest", "publication",
        "device-provenance"};
    return names[static_cast<std::size_t>(value) - 1];
}

std::string_view importStatus(const host::ExportAssetImportStatus value) {
    constexpr std::array<std::string_view, 6> names{"not-requested", "pending", "imported",
                                                    "refreshed",     "failed",  "skipped"};
    return names[static_cast<std::size_t>(value)];
}

std::string_view unavailableCode(const host::ExportUnavailableCode code) {
    constexpr std::array<std::string_view, 27> names{"none",
                                                     "not-applicable",
                                                     "shutting-down",
                                                     "no-project",
                                                     "read-only-project",
                                                     "unknown-job",
                                                     "unknown-output",
                                                     "already-active",
                                                     "not-active",
                                                     "not-rendering",
                                                     "not-paused",
                                                     "not-finished",
                                                     "no-enabled-outputs",
                                                     "preflight-blocked",
                                                     "locked-while-running",
                                                     "last-output",
                                                     "queue-full",
                                                     "output-limit-reached",
                                                     "nothing-to-start",
                                                     "nothing-to-stop",
                                                     "nothing-finished",
                                                     "not-published",
                                                     "composition-missing",
                                                     "platform-not-supported",
                                                     "needs-replace-confirmation",
                                                     "nothing-to-confirm",
                                                     "destination-check-pending"};
    return names[static_cast<std::size_t>(code)];
}

void writeCheckValue(Json& json, const host::ExportCheckValue& value) {
    std::visit(
        [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                json.raw("null");
            else if constexpr (std::is_same_v<T, std::uint64_t>)
                json.number(item);
            else if constexpr (std::is_same_v<T, std::string>)
                json.string(item);
            else if constexpr (std::is_same_v<T, host::ExportRationalValue>)
                json.string(std::to_string(item.numerator) + "/" +
                            std::to_string(item.denominator));
            else
                json.begin('[')
                    .number(item.primaries)
                    .number(item.transfer)
                    .number(item.matrix)
                    .boolean(item.fullRange)
                    .end(']');
        },
        value);
}

void writeVerification(Json& json, const host::ExportOutputVerification& output) {
    json.begin('{')
        .key("output")
        .number(output.output.value())
        .key("overall")
        .string(outcome(output.overall));
    json.key("checks").begin('[');
    for (const auto& check : output.checks) {
        json.begin('{')
            .key("check")
            .string(checkName(check.check))
            .key("outcome")
            .string(outcome(check.outcome))
            .key("code")
            .string(check.code)
            .key("expected");
        writeCheckValue(json, check.expected);
        json.key("observed");
        writeCheckValue(json, check.observed);
        json.end('}');
    }
    json.end(']');
    const auto& artifacts = output.artifacts;
    json.key("artifacts")
        .begin('{')
        .key("count")
        .number(artifacts.publishedArtifacts)
        .key("first")
        .string(artifacts.firstPath.generic_string())
        .key("last")
        .string(artifacts.lastPath.generic_string())
        .key("bytes")
        .number(artifacts.totalBytes);
    if (artifacts.digest) {
        const auto hex = artifacts.digest->toLowercaseHex();
        json.key("sha256").string(std::string_view(hex.data(), hex.size()));
    }
    json.end('}');
    json.key("provenance")
        .begin('{')
        .key("gpuFrames")
        .number(output.provenance.gpuFrames)
        .key("cpuReferenceFrames")
        .number(output.provenance.cpuReferenceFrames)
        .key("framesReused")
        .number(output.provenance.framesReused)
        .key("encoder")
        .string(output.provenance.encoder)
        .end('}');
    json.end('}');
}

void writePreflight(Json& json, const std::vector<host::ExportPreflightIssue>& issues) {
    json.key("preflight").begin('[');
    for (const auto& issue : issues)
        json.begin('{')
            .key("code")
            .number(static_cast<std::uint64_t>(issue.code))
            .key("target")
            .number(static_cast<std::uint64_t>(issue.target))
            .key("severity")
            .string(issue.severity == host::ExportPreflightSeverity::Blocking ? "blocking"
                    : issue.severity == host::ExportPreflightSeverity::Warning
                        ? "warning"
                        : "needs-confirmation")
            .key("reason")
            .string(issue.reason)
            .end('}');
    json.end(']');
}

void writeImport(Json& json, const host::ExportAssetImportResult& result) {
    json.begin('{')
        .key("output")
        .number(result.output.value())
        .key("status")
        .string(importStatus(result.status));
    if (result.asset)
        json.key("asset").number(result.asset->value());
    if (!result.code.empty())
        json.key("code").string(result.code).key("reason").string(result.reason);
    json.end('}');
}

std::optional<std::uint64_t> integerArgument(const Arguments& arguments, std::string_view name) {
    const auto found = arguments.find(name);
    if (found == arguments.end())
        return std::nullopt;
    if (const auto* value = std::get_if<std::int64_t>(&found->second.storage); value && *value >= 0)
        return static_cast<std::uint64_t>(*value);
    if (const auto* id = std::get_if<StableId>(&found->second.storage))
        return id->value;
    if (const auto* real = std::get_if<double>(&found->second.storage); real && *real >= 0)
        return static_cast<std::uint64_t>(*real);
    return std::nullopt;
}

ExportCallResult fromCommand(const host::ExportCommandResult& result) {
    if (result.accepted)
        return {true,
                "{\"accepted\":true,\"revision\":" + std::to_string(result.revision) + "}",
                {},
                {}};
    return {false,
            {},
            "bloom.export." + std::string(unavailableCode(result.availability.code)),
            result.availability.reason};
}

} // namespace

bool isExportCall(const std::string_view name) noexcept { return name.starts_with("export."); }

std::string exportVerificationJson(const host::ExportOutputVerification& output) {
    Json json;
    writeVerification(json, output);
    return json.take();
}

std::string exportImportJson(const host::ExportAssetImportResult& result) {
    Json json;
    writeImport(json, result);
    return json.take();
}

std::string exportSnapshotJson(const host::ExportQueueSnapshot& snapshot) {
    Json json;
    json.begin('{')
        .key("revision")
        .number(snapshot.revision)
        .key("runner")
        .string(snapshot.runner == host::ExportRunnerState::Idle      ? "idle"
                : snapshot.runner == host::ExportRunnerState::Running ? "running"
                                                                      : "shutting-down");
    json.key("jobs").begin('[');
    for (const auto& job : snapshot.jobs) {
        json.begin('{')
            .key("id")
            .number(job.id.value())
            .key("revision")
            .number(job.revision)
            .key("name")
            .string(job.displayName)
            .key("transient")
            .boolean(job.transient)
            .key("state")
            .string(jobState(job.state))
            .key("framesCompleted")
            .number(job.progress.framesCompleted)
            .key("framesTotal")
            .number(job.progress.framesTotal)
            .key("route")
            .string(route(job.progress.route));
        if (job.progress.remaining)
            json.key("remainingMs")
                .number(static_cast<std::uint64_t>(job.progress.remaining->count()));
        if (job.failure)
            json.key("failure")
                .string(job.failure->reason)
                .key("failureSummary")
                .string(job.failure->summary)
                .key("failureCode")
                .string(job.failure->diagnosticCode);
        writePreflight(json, job.preflight);
        const auto& start = job.commands[host::ExportCommand::Start];
        json.key("start").begin('{').key("available").boolean(start.available);
        if (!start.available)
            json.key("code").string(unavailableCode(start.code)).key("reason").string(start.reason);
        json.end('}');
        json.key("outputs").begin('[');
        for (const auto& output : job.outputs) {
            json.begin('{')
                .key("id")
                .number(output.id.value())
                .key("format")
                .string(document::exportFormatToken(output.record.settings.format))
                .key("state")
                .string(outputState(output.state))
                .key("destination")
                .string(output.destination.firstPath.generic_string());
            writePreflight(json, output.preflight);
            if (output.verification) {
                json.key("verification");
                writeVerification(json, *output.verification);
            }
            json.key("import");
            writeImport(json, output.import);
            json.end('}');
        }
        json.end(']').end('}');
    }
    json.end(']').end('}');
    return json.take();
}

std::string exportEventsJson(const host::ExportQueueEventBatch& batch) {
    Json json;
    json.begin('{').key("lastSequence").number(batch.lastSequence).key("gap").boolean(batch.gap);
    json.key("events").begin('[');
    for (const auto& event : batch.events) {
        json.begin('{')
            .key("sequence")
            .number(event.sequence)
            .key("revision")
            .number(event.revision);
        std::visit(
            [&](const auto& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, host::ExportQueueDrained>) {
                    json.key("type")
                        .string("drained")
                        .key("succeeded")
                        .number(payload.succeeded + payload.succeededWithWarnings)
                        .key("failed")
                        .number(payload.failed)
                        .key("cancelled")
                        .number(payload.cancelled)
                        .key("importFailures")
                        .number(payload.importFailures);
                } else {
                    json.key("job").number(payload.job.value()).key("run").number(payload.run);
                    if constexpr (std::is_same_v<T, host::ExportJobSucceeded>)
                        json.key("type").string(payload.withWarnings ? "succeeded-with-warnings"
                                                                     : "succeeded");
                    else if constexpr (std::is_same_v<T, host::ExportJobFailed>)
                        json.key("type").string("failed").key("reason").string(
                            payload.reason.reason);
                    else
                        json.key("type").string("cancelled");
                    json.key("outputs").begin('[');
                    for (const auto& output : payload.outputs)
                        writeVerification(json, output);
                    json.end(']').key("imports").begin('[');
                    for (const auto& result : payload.imports)
                        writeImport(json, result);
                    json.end(']');
                }
            },
            event.payload);
        json.end('}');
    }
    json.end(']').end('}');
    return json.take();
}

ExportCallResult ExportCalls::call(const std::string_view name, const Arguments& arguments,
                                   const bool headless) {
    const auto job = [&]() -> std::optional<document::ExportJobId> {
        if (const auto id = integerArgument(arguments, "job"))
            return document::ExportJobId::fromRaw(*id);
        return lastAdded_;
    };
    const auto missing = [](std::string_view what) {
        return ExportCallResult{
            false, {}, "bloom.scripting.invalid-argument", std::string(what) + " is required"};
    };
    service_.pump();
    if (name == "export.start") {
        const auto id = job();
        if (!id)
            return missing("job");
        const document::ExportJobId ids[] = {*id};
        return fromCommand(service_.start(ids));
    }
    if (name == "export.start-all")
        return fromCommand(service_.startAll());
    if (name == "export.stop-all")
        return fromCommand(service_.stopAll());
    if (name == "export.pause" || name == "export.resume" || name == "export.stop" ||
        name == "export.retry") {
        const auto id = job();
        if (!id)
            return missing("job");
        return fromCommand(name == "export.pause"    ? service_.pause(*id)
                           : name == "export.resume" ? service_.resume(*id)
                           : name == "export.stop"   ? service_.stop(*id)
                                                     : service_.retry(*id));
    }
    if (name == "export.confirm-replace") {
        const auto id = job();
        const auto output = integerArgument(arguments, "output");
        if (!id || !output)
            return missing("job and output");
        return fromCommand(
            service_.confirmReplace(*id, document::ExportOutputId::fromRaw(*output)));
    }
    if (name == "export.status") {
        const auto snapshot = service_.snapshot();
        if (const auto since = integerArgument(arguments, "sinceRevision");
            since && *since >= snapshot->revision)
            return {true,
                    "{\"revision\":" + std::to_string(snapshot->revision) + ",\"changed\":false}",
                    {},
                    {}};
        return {true, exportSnapshotJson(*snapshot), {}, {}};
    }
    if (name == "export.events")
        return {true,
                exportEventsJson(
                    service_.events(integerArgument(arguments, "afterSequence").value_or(0))),
                {},
                {}};
    if (name == "export.log") {
        const auto id = job();
        if (!id)
            return missing("job");
        const auto batch =
            service_.log(*id, integerArgument(arguments, "afterSequence").value_or(0));
        Json json;
        json.begin('{')
            .key("lastSequence")
            .number(batch.lastSequence)
            .key("gap")
            .boolean(batch.gap);
        json.key("entries").begin('[');
        for (const auto& entry : batch.entries)
            json.begin('{')
                .key("sequence")
                .number(entry.sequence)
                .key("code")
                .string(entry.code)
                .key("message")
                .string(entry.message)
                .end('}');
        json.end(']').end('}');
        return {true, json.take(), {}, {}};
    }
    if (name == "export.wait") {
        if (!headless)
            return {false,
                    {},
                    "bloom.export.wait-headless-only",
                    "export.wait runs only in headless clients"};
        const auto timeout =
            std::chrono::milliseconds(integerArgument(arguments, "timeoutMs").value_or(600000));
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        const auto target = integerArgument(arguments, "job");
        while (std::chrono::steady_clock::now() < deadline) {
            service_.pump();
            const auto snapshot = service_.snapshot();
            bool done = true;
            for (const auto& view : snapshot->jobs)
                if ((!target || view.id.value() == *target) && host::isActive(view.state))
                    done = false;
            if (done)
                return {true, exportSnapshotJson(*snapshot), {}, {}};
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return {false, {}, "bloom.export.wait-timeout", "The export did not finish in time"};
    }
    if (name == "export.presets") {
        const auto catalogue = service_.presets().snapshot();
        Json json;
        json.begin('[');
        for (const auto& preset : catalogue->presets)
            json.begin('{')
                .key("id")
                .string(preset.id)
                .key("name")
                .string(preset.displayName)
                .key("format")
                .string(document::exportFormatToken(preset.settings.format))
                .key("available")
                .boolean(preset.platformAvailability.available)
                .key("reason")
                .string(preset.platformAvailability.reason)
                .key("settings")
                .raw(host::exportSettingsToJson(preset.settings))
                .end('}');
        json.end(']');
        return {true, json.take(), {}, {}};
    }
    if (name == "export.script") {
        const auto id = job();
        if (!id)
            return missing("job");
        const auto snapshot = service_.snapshot();
        for (const auto& view : snapshot->jobs)
            if (view.id == *id) {
                const auto python =
                    arguments.contains("format") &&
                    std::get_if<std::string>(&arguments.find("format")->second.storage) &&
                    *std::get_if<std::string>(&arguments.find("format")->second.storage) ==
                        "python";
                Json json;
                json.begin('{').key("script").string(
                    host::exportJobScript(view.record, python ? host::ExportScriptFormat::Python
                                                              : host::ExportScriptFormat::CliJson));
                json.end('}');
                return {true, json.take(), {}, {}};
            }
        return {false, {}, "bloom.export.unknown-job", "The job does not exist"};
    }
    return {false, {}, "bloom.scripting.unknown-call", "Unknown export call"};
}

} // namespace bloom::scripting
