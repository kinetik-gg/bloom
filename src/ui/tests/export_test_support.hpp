#pragma once

// A scripted export queue for the Export panel's tests and captures: hand-built snapshots behind
// the ExportQueuePort seam, with every call the panel makes recorded. It is deterministic and has
// no scheduler; a test publishes the next snapshot itself and the adapter's drain() sees it.

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

#include <bloom/ui/export_queue_adapter.hpp>

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bloom::ui::test {

[[nodiscard]] inline host::ExportCommandAvailabilitySet allAvailable() {
    host::ExportCommandAvailabilitySet set;
    for (auto& entry : set.entries) {
        entry.available = true;
        entry.code = host::ExportUnavailableCode::None;
    }
    return set;
}

inline void refuse(host::ExportCommandAvailabilitySet& set, const host::ExportCommand command,
                   const host::ExportUnavailableCode code, std::string reason) {
    auto& entry = set.entries[static_cast<std::size_t>(command)];
    entry.available = false;
    entry.code = code;
    entry.reason = std::move(reason);
}

[[nodiscard]] inline host::ExportOutputView
makeOutput(const std::uint64_t id, const std::string& presetId = "builtin:h264-mp4",
           const document::ExportFormat format = document::ExportFormat::H264Mp4,
           const std::string& path = "renders/out.mp4") {
    host::ExportOutputView view;
    view.id = document::ExportOutputId::fromRaw(id);
    view.record.id = view.id;
    view.record.presetId = presetId;
    view.record.settings.format = format;
    view.record.destination.path = path;
    view.destination.firstPath = "/project/" + path;
    view.destination.lastPath = view.destination.firstPath;
    view.destination.fileCount = 1;
    view.availability.available = true;
    view.availability.code = host::ExportAvailabilityCode::Available;
    view.commands = allAvailable();
    return view;
}

[[nodiscard]] inline host::ExportJobView makeJob(const std::uint64_t id, const std::string& name,
                                                 const document::CompositionId composition,
                                                 std::vector<host::ExportOutputView> outputs) {
    host::ExportJobView view;
    view.id = document::ExportJobId::fromRaw(id);
    view.revision = 1;
    view.record.id = view.id;
    view.record.composition = composition;
    view.record.name = name;
    view.displayName = name;
    view.resolvedRange = host::ExportResolvedRange{0, 239};
    for (auto& output : outputs)
        view.record.outputs.push_back(output.record);
    view.outputs = std::move(outputs);
    view.commands = allAvailable();
    return view;
}

[[nodiscard]] inline host::ExportPreset makePreset(std::string id, std::string name,
                                                   const host::ExportPresetCategory category,
                                                   const document::ExportFormat format) {
    host::ExportPreset preset;
    preset.id = std::move(id);
    preset.displayName = std::move(name);
    preset.category = category;
    preset.settings.format = format;
    preset.platformAvailability.available = true;
    preset.platformAvailability.code = host::ExportAvailabilityCode::Available;
    return preset;
}

[[nodiscard]] inline std::shared_ptr<host::ExportPresetCatalogueSnapshot> makeCatalogue() {
    auto catalogue = std::make_shared<host::ExportPresetCatalogueSnapshot>();
    catalogue->revision = 1;
    catalogue->presets.push_back(makePreset("builtin:h264-mp4", "H.264 MP4",
                                            host::ExportPresetCategory::Video,
                                            document::ExportFormat::H264Mp4));
    catalogue->presets.push_back(makePreset("builtin:prores-422hq-mov", "ProRes 422 HQ",
                                            host::ExportPresetCategory::Video,
                                            document::ExportFormat::ProResMov));
    catalogue->presets.push_back(makePreset("builtin:exr-sequence", "OpenEXR sequence",
                                            host::ExportPresetCategory::VfxSequences,
                                            document::ExportFormat::OpenExr));
    return catalogue;
}


// Three jobs for the panel's tests and captures: one rendering, one waiting, one failed with
// a failed verification.
inline void fillThreeJobs(host::ExportQueueSnapshot& snapshot, const document::CompositionId composition) {
            auto running = makeJob(1, "Shot 010", composition, {makeOutput(11)});
            running.state = host::ExportJobState::Rendering;
            running.progress = {.framesCompleted = 120, .framesTotal = 240, .outputIndex = 0,
                                .outputCount = 1, .currentFrame = 120, .fraction = 0.5,
                                .elapsed = std::chrono::milliseconds(7000),
                                .remaining = std::chrono::milliseconds(42000),
                                .route = host::ExportDeviceRoute::Gpu};
            running.outputs[0].state = host::ExportOutputState::Rendering;
            running.commands = allAvailable();
            refuse(running.commands, host::ExportCommand::EditJob,
                   host::ExportUnavailableCode::LockedWhileRunning,
                   "Edits are locked while the job runs.");
            auto waiting = makeJob(2, "Shot 020", composition, {makeOutput(21)});
            waiting.state = host::ExportJobState::Waiting;
            auto failed = makeJob(3, "Shot 030", composition,
                                  {makeOutput(31, "builtin:exr-sequence",
                                              document::ExportFormat::OpenExr, "seq/shot.####.exr")});
            failed.state = host::ExportJobState::Failed;
            failed.failure = host::ExportFailure{.code = host::ExportFailureCode::VerificationFailed,
                                                 .reason = "Frame count differs.",
                                                 .summary = std::string(host::exportFailureSummary(
                                                     host::ExportFailureCode::VerificationFailed))};
            failed.outputs[0].state = host::ExportOutputState::Failed;
            host::ExportOutputVerification verification;
            verification.output = failed.outputs[0].id;
            verification.overall = host::ExportCheckOutcome::Failed;
            verification.checks.push_back({.check = host::ExportCheck::FrameCount,
                                           .outcome = host::ExportCheckOutcome::Failed,
                                           .expected = std::uint64_t{240},
                                           .observed = std::uint64_t{239}});
            failed.outputs[0].verification = verification;
            snapshot.jobs = {running, waiting, failed};
            snapshot.activeJob = running.id;
}

class FakeExportPort final : public ExportQueuePort {
  public:
    FakeExportPort() : snap_(std::make_shared<host::ExportQueueSnapshot>()), catalogue_(makeCatalogue()) {
        snap_->commands = allAvailable();
    }

    // Publishes a new snapshot with the next revision, edited by `change`.
    void publish(const std::function<void(host::ExportQueueSnapshot&)>& change) {
        auto next = std::make_shared<host::ExportQueueSnapshot>(*snap_);
        change(*next);
        next->revision = snap_->revision + 1;
        snap_ = std::move(next);
    }
    void raise(host::ExportQueueEventPayload payload) {
        host::ExportQueueEvent event;
        event.sequence = ++lastEvent_;
        event.revision = snap_->revision;
        event.payload = std::move(payload);
        events_.push_back(std::move(event));
    }

    std::vector<std::string> calls;
    int pumps = 0;
    // Runs inside pump(): a test script advancing the queue the way a task completion would.
    std::function<void(FakeExportPort&)> onPump;
    std::map<std::uint64_t, std::vector<host::ExportLogEntry>> logs;
    bool logGap = false;
    ExportEditResult nextEdit{true, {}, std::nullopt};
    std::string script = "bloom-cli export-script";
    std::uint64_t nextJobRaw = 100;

    [[nodiscard]] std::shared_ptr<const host::ExportQueueSnapshot> snapshot() const override {
        return snap_;
    }
    [[nodiscard]] std::shared_ptr<const host::ExportPresetCatalogueSnapshot>
    catalogue() const override {
        return catalogue_;
    }
    [[nodiscard]] host::ExportQueueEventBatch events(const std::uint64_t after) const override {
        host::ExportQueueEventBatch batch;
        for (const auto& event : events_)
            if (event.sequence > after)
                batch.events.push_back(event);
        batch.lastSequence = lastEvent_;
        return batch;
    }
    [[nodiscard]] host::ExportLogBatch log(const document::ExportJobId job,
                                           const std::uint64_t after) const override {
        host::ExportLogBatch batch;
        batch.gap = logGap;
        if (const auto found = logs.find(job.value()); found != logs.end())
            for (const auto& entry : found->second)
                if (entry.sequence > after) {
                    batch.entries.push_back(entry);
                    batch.lastSequence = entry.sequence;
                }
        if (batch.lastSequence == 0)
            batch.lastSequence = after;
        return batch;
    }
    // Which job a task owner id belongs to, for the Jobs link.
    std::map<std::uint64_t, std::uint64_t> taskOwners;
    [[nodiscard]] std::optional<document::ExportJobId>
    jobForTaskOwner(runtime::TaskOwnerId owner) const override {
        if (const auto found = taskOwners.find(owner.value()); found != taskOwners.end())
            return document::ExportJobId::fromRaw(found->second);
        return std::nullopt;
    }
    [[nodiscard]] std::string jobScript(const document::ExportJob&,
                                        const host::ExportScriptFormat) const override {
        return script;
    }
    void pump() override {
        ++pumps;
        if (onPump)
            onPump(*this);
    }

    host::ExportCommandResult startAll() override { return session("startAll"); }
    host::ExportCommandResult start(std::span<const document::ExportJobId> jobs) override {
        return session("start " + std::to_string(jobs.front().value()));
    }
    host::ExportCommandResult pause(document::ExportJobId job) override {
        return session("pause " + std::to_string(job.value()));
    }
    host::ExportCommandResult resume(document::ExportJobId job) override {
        return session("resume " + std::to_string(job.value()));
    }
    host::ExportCommandResult stop(document::ExportJobId job) override {
        return session("stop " + std::to_string(job.value()));
    }
    host::ExportCommandResult stopAll() override { return session("stopAll"); }
    host::ExportCommandResult retry(document::ExportJobId job) override {
        return session("retry " + std::to_string(job.value()));
    }
    host::ExportCommandResult confirmReplace(document::ExportJobId job,
                                             document::ExportOutputId output) override {
        return session("confirmReplace " + std::to_string(job.value()) + " " +
                       std::to_string(output.value()));
    }
    void refreshPreflight() override { calls.emplace_back("refreshPreflight"); }

    ExportEditResult addJob(document::CompositionId composition) override {
        calls.push_back("addJob " + std::to_string(composition.value()));
        if (!nextEdit.accepted)
            return nextEdit;
        const auto added = nextJobRaw;
        publish([&](host::ExportQueueSnapshot& next) {
            next.jobs.push_back(makeJob(nextJobRaw, "Added", composition,
                                        {makeOutput(nextJobRaw + 1000)}));
            ++nextJobRaw;
        });
        auto result = nextEdit;
        result.addedJob = document::ExportJobId::fromRaw(added);
        return result;
    }
    ExportEditResult duplicateJob(document::ExportJobId job) override {
        return edit("duplicateJob " + std::to_string(job.value()));
    }
    ExportEditResult removeJob(document::ExportJobId job) override {
        return edit("removeJob " + std::to_string(job.value()));
    }
    ExportEditResult removeFinished() override { return edit("removeFinished"); }
    ExportEditResult moveJob(document::ExportJobId job, std::size_t index) override {
        return edit("moveJob " + std::to_string(job.value()) + " " + std::to_string(index));
    }
    ExportEditResult editJob(document::ExportJobId job, const ExportJobEdit& change) override {
        std::string what = "editJob " + std::to_string(job.value());
        if (change.name)
            what += " name=" + *change.name;
        if (change.range)
            what += " range=" + std::to_string(static_cast<int>(change.range->kind)) + ":" +
                    std::to_string(change.range->first) + "-" + std::to_string(change.range->last);
        if (change.enabled)
            what += std::string(" enabled=") + (*change.enabled ? "1" : "0");
        if (change.labelColor)
            what += change.labelColor->has_value() ? " label=set" : " label=clear";
        return edit(what);
    }
    ExportEditResult addOutput(document::ExportJobId job) override {
        return edit("addOutput " + std::to_string(job.value()));
    }
    ExportEditResult applyPreset(document::ExportJobId job, document::ExportOutputId output,
                                 const std::string& presetId) override {
        return edit("applyPreset " + std::to_string(job.value()) + " " +
                    std::to_string(output.value()) + " " + presetId);
    }
    ExportEditResult editOutput(document::ExportJobId job, document::ExportOutputId output,
                                const ExportOutputEdit& change) override {
        std::string what =
            "editOutput " + std::to_string(job.value()) + " " + std::to_string(output.value());
        if (change.settings)
            what += " settings";
        if (change.destination)
            what += " destination=" + change.destination->path + " policy=" +
                    std::to_string(static_cast<int>(change.destination->overwrite));
        if (change.enabled)
            what += std::string(" enabled=") + (*change.enabled ? "1" : "0");
        if (change.importIntoAssets)
            what += std::string(" import=") + (*change.importIntoAssets ? "1" : "0");
        return edit(what);
    }
    ExportEditResult removeOutput(document::ExportJobId job, document::ExportOutputId output) override {
        return edit("removeOutput " + std::to_string(job.value()) + " " +
                    std::to_string(output.value()));
    }

  private:
    host::ExportCommandResult session(const std::string& what) {
        calls.push_back(what);
        host::ExportCommandResult result;
        result.accepted = true;
        result.revision = snap_->revision;
        return result;
    }
    ExportEditResult edit(const std::string& what) {
        calls.push_back(what);
        return nextEdit;
    }

    std::shared_ptr<host::ExportQueueSnapshot> snap_;
    std::shared_ptr<host::ExportPresetCatalogueSnapshot> catalogue_;
    std::vector<host::ExportQueueEvent> events_;
    std::uint64_t lastEvent_ = 0;
};

} // namespace bloom::ui::test
