#include <bloom/ui/export_queue_service_port.hpp>

#include <bloom/ui/export_queue_edits.hpp>

#include <utility>

namespace bloom::ui {

ServiceExportQueuePort::ServiceExportQueuePort(ServiceSource source, ExportQueueEdits& edits)
    : source_(std::move(source)), edits_(edits) {
    auto none = std::make_shared<host::ExportQueueSnapshot>();
    for (auto& entry : none->commands.entries) {
        entry.available = false;
        entry.code = host::ExportUnavailableCode::NoProject;
        entry.reason = "No project is open.";
    }
    none_ = std::move(none);
}

namespace {
host::ExportCommandResult unavailable() {
    host::ExportCommandResult result;
    result.availability.code = host::ExportUnavailableCode::NoProject;
    result.availability.reason = "No project is open.";
    return result;
}
} // namespace

std::shared_ptr<const host::ExportQueueSnapshot> ServiceExportQueuePort::snapshot() const {
    const auto* s = service();
    return s != nullptr ? s->snapshot() : none_;
}
std::shared_ptr<const host::ExportPresetCatalogueSnapshot>
ServiceExportQueuePort::catalogue() const {
    const auto* s = service();
    return s != nullptr ? s->presets().snapshot() : nullptr;
}
host::ExportQueueEventBatch ServiceExportQueuePort::events(const std::uint64_t after) const {
    const auto* s = service();
    return s != nullptr ? s->events(after) : host::ExportQueueEventBatch{};
}
host::ExportLogBatch ServiceExportQueuePort::log(const document::ExportJobId job,
                                                 const std::uint64_t after) const {
    const auto* s = service();
    return s != nullptr ? s->log(job, after) : host::ExportLogBatch{};
}
std::optional<document::ExportJobId>
ServiceExportQueuePort::jobForTaskOwner(const runtime::TaskOwnerId owner) const {
    const auto* s = service();
    return s != nullptr ? s->jobForTaskOwner(owner) : std::nullopt;
}
std::string ServiceExportQueuePort::jobScript(const document::ExportJob& job,
                                              const host::ExportScriptFormat format) const {
    return host::exportJobScript(job, format);
}
ExportEditResult ServiceExportQueuePort::published(ExportEditResult result) {
    // The service reads the document when it pumps: an accepted edit is visible at once.
    if (result.accepted)
        pump();
    return result;
}

void ServiceExportQueuePort::pump() {
    if (auto* s = service())
        s->pump();
}

host::ExportCommandResult ServiceExportQueuePort::startAll() { auto* s = service();
    return s != nullptr ? s->startAll() : unavailable(); }
host::ExportCommandResult
ServiceExportQueuePort::start(const std::span<const document::ExportJobId> jobs) {
    auto* s = service();
    return s != nullptr ? s->start(jobs) : unavailable();
}
host::ExportCommandResult ServiceExportQueuePort::pause(const document::ExportJobId job) {
    auto* s = service();
    return s != nullptr ? s->pause(job) : unavailable();
}
host::ExportCommandResult ServiceExportQueuePort::resume(const document::ExportJobId job) {
    auto* s = service();
    return s != nullptr ? s->resume(job) : unavailable();
}
host::ExportCommandResult ServiceExportQueuePort::stop(const document::ExportJobId job) {
    auto* s = service();
    return s != nullptr ? s->stop(job) : unavailable();
}
host::ExportCommandResult ServiceExportQueuePort::stopAll() { auto* s = service();
    return s != nullptr ? s->stopAll() : unavailable(); }
host::ExportCommandResult ServiceExportQueuePort::retry(const document::ExportJobId job) {
    auto* s = service();
    return s != nullptr ? s->retry(job) : unavailable();
}
host::ExportCommandResult
ServiceExportQueuePort::confirmReplace(const document::ExportJobId job,
                                       const document::ExportOutputId output) {
    auto* s = service();
    return s != nullptr ? s->confirmReplace(job, output) : unavailable();
}
void ServiceExportQueuePort::refreshPreflight() {
    if (auto* s = service())
        s->refreshPreflight();
}

ExportEditResult ServiceExportQueuePort::addJob(const document::CompositionId composition) {
    return published(edits_.addJob(composition));
}
ExportEditResult ServiceExportQueuePort::duplicateJob(const document::ExportJobId job) {
    return published(edits_.duplicateJob(job));
}
ExportEditResult ServiceExportQueuePort::removeJob(const document::ExportJobId job) {
    return published(edits_.removeJob(job));
}
ExportEditResult ServiceExportQueuePort::removeFinished() {
    std::vector<document::ExportJobId> finished;
    if (const auto snapshot = this->snapshot())
        for (const auto& job : snapshot->jobs)
            if (!job.transient && (job.state == host::ExportJobState::Done ||
                                   job.state == host::ExportJobState::DoneWithWarnings))
                finished.push_back(job.id);
    return published(edits_.removeFinished(finished));
}
ExportEditResult ServiceExportQueuePort::moveJob(const document::ExportJobId job,
                                                 const std::size_t index) {
    return published(edits_.moveJob(job, index));
}
ExportEditResult ServiceExportQueuePort::editJob(const document::ExportJobId job,
                                                 const ExportJobEdit& edit) {
    return published(edits_.editJob(job, edit));
}
ExportEditResult ServiceExportQueuePort::addOutput(const document::ExportJobId job) {
    return published(edits_.addOutput(job));
}
ExportEditResult ServiceExportQueuePort::applyPreset(const document::ExportJobId,
                                                     const document::ExportOutputId output,
                                                     const std::string& presetId) {
    return published(edits_.applyPreset(output, presetId));
}
ExportEditResult ServiceExportQueuePort::editOutput(const document::ExportJobId,
                                                    const document::ExportOutputId output,
                                                    const ExportOutputEdit& edit) {
    return published(edits_.editOutput(output, edit));
}
ExportEditResult ServiceExportQueuePort::removeOutput(const document::ExportJobId,
                                                      const document::ExportOutputId output) {
    return published(edits_.removeOutput(output));
}

} // namespace bloom::ui
