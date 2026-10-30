#include <bloom/ui/export_queue_adapter.hpp>

#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/ui_feedback.hpp>

#include <algorithm>
#include <utility>
#include <variant>

namespace bloom::ui {
namespace {

QString text(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

ExportQueueAdapter::ExportQueueAdapter(ExportQueuePort& port, UiFeedback* const feedback,
                                       QObject* const parent)
    : QObject(parent), port_(port), feedback_(feedback) {
    // Start from what the service already holds: a first observe() must not announce history.
    if (const auto snapshot = port_.snapshot())
        revision_ = snapshot->revision;
    lastEventSequence_ = port_.events(0).lastSequence;
}

void ExportQueueAdapter::attach(TaskUiBridge& bridge) {
    connect(&bridge, &TaskUiBridge::resultsAvailable, this, &ExportQueueAdapter::drain);
}

void ExportQueueAdapter::rebind() {
    pendingSelection_.reset();
    const auto snapshot = port_.snapshot();
    revision_ = snapshot ? snapshot->revision : 0;
    lastEventSequence_ = port_.events(0).lastSequence;
    Q_EMIT rebound();
    Q_EMIT queueChanged();
}

void ExportQueueAdapter::drain() {
    port_.pump();
    observe();
}

void ExportQueueAdapter::observe() {
    handleEvents();
    const auto snapshot = port_.snapshot();
    const std::uint64_t revision = snapshot ? snapshot->revision : 0;
    if (revision == revision_)
        return;
    revision_ = revision;
    Q_EMIT queueChanged();
}

void ExportQueueAdapter::handleEvents() {
    const auto batch = port_.events(lastEventSequence_);
    for (const auto& event : batch.events) {
        if (event.sequence <= lastEventSequence_)
            continue;
        lastEventSequence_ = event.sequence;
        if (feedback_ == nullptr)
            continue;
        if (const auto* done = std::get_if<host::ExportJobSucceeded>(&event.payload)) {
            const auto name = jobName(done->job);
            feedback_->announceRender(true, done->withWarnings
                                                ? tr("“%1” finished with warnings").arg(name)
                                                : tr("“%1” finished").arg(name));
        } else if (const auto* failed = std::get_if<host::ExportJobFailed>(&event.payload)) {
            feedback_->announceRender(false, tr("“%1” failed: %2")
                                                 .arg(jobName(failed->job), text(failed->reason.reason)));
        }
        // Cancelled and Drained make no sound: the artist asked for the first, and every job in a
        // drained queue has already been announced.
    }
    lastEventSequence_ = std::max(lastEventSequence_, batch.lastSequence);
}

QString ExportQueueAdapter::jobName(const document::ExportJobId job) const {
    if (const auto snapshot = port_.snapshot())
        for (const auto& view : snapshot->jobs)
            if (view.id == job)
                return text(view.displayName);
    return tr("Export");
}

std::shared_ptr<const host::ExportQueueSnapshot> ExportQueueAdapter::snapshot() const {
    return port_.snapshot();
}

std::shared_ptr<const host::ExportPresetCatalogueSnapshot> ExportQueueAdapter::catalogue() const {
    return port_.catalogue();
}

host::ExportLogBatch ExportQueueAdapter::log(const document::ExportJobId job,
                                             const std::uint64_t afterSequence) const {
    return port_.log(job, afterSequence);
}

std::optional<document::ExportJobId>
ExportQueueAdapter::jobForTaskOwner(const runtime::TaskOwnerId owner) const {
    return port_.jobForTaskOwner(owner);
}

host::ExportCommandResult ExportQueueAdapter::startAll() {
    auto result = port_.startAll();
    observe();
    return result;
}

host::ExportCommandResult ExportQueueAdapter::start(const document::ExportJobId job) {
    auto result = port_.start(std::span<const document::ExportJobId>(&job, 1));
    observe();
    return result;
}

host::ExportCommandResult ExportQueueAdapter::pause(const document::ExportJobId job) {
    auto result = port_.pause(job);
    observe();
    return result;
}

host::ExportCommandResult ExportQueueAdapter::resume(const document::ExportJobId job) {
    auto result = port_.resume(job);
    observe();
    return result;
}

host::ExportCommandResult ExportQueueAdapter::stop(const document::ExportJobId job) {
    auto result = port_.stop(job);
    observe();
    return result;
}

host::ExportCommandResult ExportQueueAdapter::stopAll() {
    auto result = port_.stopAll();
    observe();
    return result;
}

host::ExportCommandResult ExportQueueAdapter::retry(const document::ExportJobId job) {
    auto result = port_.retry(job);
    observe();
    return result;
}

host::ExportCommandResult ExportQueueAdapter::confirmReplace(const document::ExportJobId job,
                                                             const document::ExportOutputId output) {
    auto result = port_.confirmReplace(job, output);
    observe();
    return result;
}

void ExportQueueAdapter::refreshPreflight() { port_.refreshPreflight(); }

ExportEditResult ExportQueueAdapter::duplicateJob(const document::ExportJobId job) {
    auto result = port_.duplicateJob(job);
    observe();
    return result;
}

ExportEditResult ExportQueueAdapter::removeJob(const document::ExportJobId job) {
    auto result = port_.removeJob(job);
    observe();
    return result;
}

ExportEditResult ExportQueueAdapter::removeFinished() {
    auto result = port_.removeFinished();
    observe();
    return result;
}

ExportEditResult ExportQueueAdapter::moveJob(const document::ExportJobId job,
                                             const std::size_t index) {
    auto result = port_.moveJob(job, index);
    observe();
    return result;
}

ExportEditResult ExportQueueAdapter::editJob(const document::ExportJobId job,
                                             const ExportJobEdit& edit) {
    auto result = port_.editJob(job, edit);
    observe();
    return result;
}

ExportEditResult ExportQueueAdapter::addOutput(const document::ExportJobId job) {
    auto result = port_.addOutput(job);
    observe();
    return result;
}

ExportEditResult ExportQueueAdapter::applyPreset(const document::ExportJobId job,
                                                 const document::ExportOutputId output,
                                                 const std::string& presetId) {
    auto result = port_.applyPreset(job, output, presetId);
    observe();
    return result;
}

ExportEditResult ExportQueueAdapter::editOutput(const document::ExportJobId job,
                                                const document::ExportOutputId output,
                                                const ExportOutputEdit& edit) {
    auto result = port_.editOutput(job, output, edit);
    observe();
    return result;
}

ExportEditResult ExportQueueAdapter::removeOutput(const document::ExportJobId job,
                                                  const document::ExportOutputId output) {
    auto result = port_.removeOutput(job, output);
    observe();
    return result;
}

ExportEditResult ExportQueueAdapter::addCompositionToQueue(const document::CompositionId composition) {
    auto result = port_.addJob(composition);
    if (!result.accepted) {
        observe();
        return result;
    }
    const auto added = result.addedJob;
    observe();
    pendingSelection_ = added;
    Q_EMIT revealRequested();
    if (added.has_value())
        Q_EMIT jobAdded(static_cast<qulonglong>(added->value()));
    return result;
}

void ExportQueueAdapter::showJob(const document::ExportJobId job) {
    pendingSelection_ = job;
    Q_EMIT revealRequested();
    Q_EMIT jobAdded(static_cast<qulonglong>(job.value()));
}

QString ExportQueueAdapter::addUnavailableReason() const {
    const auto snapshot = port_.snapshot();
    if (!snapshot)
        return tr("The export queue is not available.");
    const auto& availability = snapshot->commands[host::ExportCommand::AddJob];
    return availability.available ? QString{} : text(availability.reason);
}

std::optional<document::ExportJobId> ExportQueueAdapter::takePendingSelection() {
    return std::exchange(pendingSelection_, std::nullopt);
}

void ExportQueueAdapter::requestRemedy(const host::ExportRemedy remedy) {
    Q_EMIT remedyRequested(static_cast<int>(remedy));
}

} // namespace bloom::ui
