#include <bloom/ui/export_queue_edits.hpp>

#include <bloom/commands/export_queue_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QCoreApplication>

#include <utility>

namespace bloom::ui {
namespace {

ExportEditResult report(const commands::CommandResult& result) {
    if (result.succeeded())
        return {true, {}, commands::addedExportIds(result).job};
    QString reason = QCoreApplication::translate("ExportQueueEdits", "The edit was refused.");
    if (!result.operationFailures.empty()) {
        const auto& message = result.operationFailures.front().issue.message;
        reason = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
    }
    return {false, reason, std::nullopt};
}

} // namespace

ExportQueueEdits::ExportQueueEdits(CompositionSession& session, DefaultPreset defaultPreset,
                                   PresetById preset)
    : session_(session), defaultPreset_(std::move(defaultPreset)), preset_(std::move(preset)) {}

ExportEditResult ExportQueueEdits::addJob(const document::CompositionId composition) {
    const auto job = host::defaultExportJob(composition, defaultPreset_());
    commands::Transaction transaction("Add to Export Queue", session_.snapshot().revision());
    transaction.emplace<commands::AddExportJob>(job);
    return report(session_.executeTransaction(std::move(transaction)));
}

ExportEditResult ExportQueueEdits::duplicateJob(const document::ExportJobId job) {
    commands::Transaction transaction("Duplicate Export Job", session_.snapshot().revision());
    transaction.emplace<commands::DuplicateExportJob>(job);
    return report(session_.executeTransaction(std::move(transaction)));
}

ExportEditResult ExportQueueEdits::removeJob(const document::ExportJobId job) {
    commands::Transaction transaction("Remove Export Job", session_.snapshot().revision());
    transaction.emplace<commands::RemoveExportJobs>(std::vector{job});
    return report(session_.executeTransaction(std::move(transaction)));
}

ExportEditResult
ExportQueueEdits::removeFinished(const std::vector<document::ExportJobId>& finished) {
    commands::Transaction transaction("Remove Finished Export Jobs",
                                      session_.snapshot().revision());
    transaction.emplace<commands::RemoveExportJobs>(finished, true);
    return report(session_.executeTransaction(std::move(transaction)));
}

ExportEditResult ExportQueueEdits::moveJob(const document::ExportJobId job,
                                           const std::size_t index) {
    commands::Transaction transaction("Move Export Job", session_.snapshot().revision());
    transaction.emplace<commands::MoveExportJobs>(std::vector{job}, index);
    return report(session_.executeTransaction(std::move(transaction)));
}

ExportEditResult ExportQueueEdits::editJob(const document::ExportJobId job,
                                           const ExportJobEdit& edit) {
    commands::Transaction transaction("Edit Export Job", session_.snapshot().revision());
    if (edit.name)
        transaction.emplace<commands::SetExportJobName>(job, *edit.name);
    if (edit.range)
        transaction.emplace<commands::SetExportJobRange>(job, *edit.range);
    if (edit.enabled)
        transaction.emplace<commands::SetExportJobEnabled>(job, *edit.enabled);
    if (edit.labelColor)
        transaction.emplace<commands::SetExportJobLabelColor>(job, *edit.labelColor);
    return report(session_.executeTransaction(std::move(transaction)));
}

ExportEditResult ExportQueueEdits::addOutput(const document::ExportJobId job) {
    commands::Transaction transaction("Add Export Output", session_.snapshot().revision());
    transaction.emplace<commands::AddExportOutput>(
        job, host::defaultExportJob(document::CompositionId{}, defaultPreset_()).outputs.front());
    return report(session_.executeTransaction(std::move(transaction)));
}

ExportEditResult ExportQueueEdits::applyPreset(const document::ExportOutputId output,
                                               const std::string& presetId) {
    const auto choice = preset_ ? preset_(presetId) : std::nullopt;
    if (!choice.has_value())
        return {false, QCoreApplication::translate("ExportQueueEdits",
                                                   "That preset is not in the catalogue."),
                std::nullopt};
    commands::Transaction transaction("Apply Export Preset", session_.snapshot().revision());
    transaction.emplace<commands::ApplyExportPreset>(output, presetId, choice->settings);
    return report(session_.executeTransaction(std::move(transaction)));
}

ExportEditResult ExportQueueEdits::editOutput(const document::ExportOutputId output,
                                              const ExportOutputEdit& edit) {
    commands::Transaction transaction("Edit Export Output", session_.snapshot().revision());
    if (edit.settings)
        transaction.emplace<commands::SetExportOutputSettings>(output, *edit.settings);
    if (edit.chosenPath) {
        const auto folder = projectFolder_ ? projectFolder_() : std::nullopt;
        const auto destination = host::exportDestinationForPath(
            edit.chosenPath->first, folder, edit.chosenPath->second);
        if (!destination.has_value())
            return {false, QCoreApplication::translate("ExportQueueEdits",
                                                       "Choose a full path for the destination."),
                    std::nullopt};
        transaction.emplace<commands::SetExportOutputDestination>(output, *destination);
    } else if (edit.destination)
        transaction.emplace<commands::SetExportOutputDestination>(output, *edit.destination);
    if (edit.enabled)
        transaction.emplace<commands::SetExportOutputEnabled>(output, *edit.enabled);
    if (edit.importIntoAssets)
        transaction.emplace<commands::SetExportOutputImport>(output, *edit.importIntoAssets);
    return report(session_.executeTransaction(std::move(transaction)));
}

ExportEditResult ExportQueueEdits::removeOutput(const document::ExportOutputId output) {
    commands::Transaction transaction("Remove Export Output", session_.snapshot().revision());
    transaction.emplace<commands::RemoveExportOutput>(output);
    return report(session_.executeTransaction(std::move(transaction)));
}

} // namespace bloom::ui
