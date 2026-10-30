#include <bloom/scripting/command_diagnostics.hpp>

#include <string>

namespace bloom::scripting {
namespace {

std::string revisionText(const document::Revision revision) {
    return std::to_string(revision.value());
}

std::string statusMessage(const commands::CommandResult& result) {
    using commands::CommandStatus;
    switch (result.status) {
    case CommandStatus::StaleRevision:
        return "The project is at revision " + revisionText(result.beforeRevision) +
               (result.expectedRevision
                    ? ", not the expected revision " + revisionText(*result.expectedRevision)
                    : std::string{}) +
               "; nothing was applied. Read the project again and retry with its current "
               "revision.";
    case CommandStatus::HistoryLimit:
        return "The edit is larger than the undo history can hold; nothing was applied. Split it "
               "into smaller transactions.";
    case CommandStatus::RevisionOverflow:
        return "The project's revision counter is exhausted; nothing was applied. Save and reopen "
               "the project.";
    case CommandStatus::ForeignDocument:
    case CommandStatus::DraftBaseMismatch:
        return "The project changed underneath the edit; nothing was applied. Read the project "
               "again and retry.";
    case CommandStatus::NothingToUndo:
        return "There is nothing to undo.";
    case CommandStatus::NothingToRedo:
        return "There is nothing to redo.";
    case CommandStatus::ValidationFailed:
        return "The edit would leave the project invalid; nothing was applied.";
    case CommandStatus::Rejected:
        return "An operation refused the edit without giving a reason; nothing was applied.";
    case CommandStatus::Succeeded:
    case CommandStatus::NoChange:
        return {};
    }
    return "Nothing was applied.";
}

std::string statusCode(const commands::CommandStatus status) {
    using commands::CommandStatus;
    switch (status) {
    case CommandStatus::StaleRevision:
        return "bloom.scripting.stale-revision";
    case CommandStatus::HistoryLimit:
        return "bloom.scripting.history-limit";
    case CommandStatus::ValidationFailed:
        return "bloom.scripting.validation-failed";
    case CommandStatus::NothingToUndo:
    case CommandStatus::NothingToRedo:
        return "bloom.scripting.nothing-to-do";
    default:
        return "bloom.scripting.command-rejected";
    }
}

} // namespace

std::vector<CommandDiagnostic> commandDiagnostics(const commands::CommandResult& result) {
    std::vector<CommandDiagnostic> diagnostics;
    for (const auto& failure : result.operationFailures) {
        diagnostics.push_back({.code = "bloom.scripting.command-rejected",
                               .severity = "error",
                               .operationIndex = failure.operationIndex,
                               .operation = failure.operationType,
                               .path = {},
                               .message = failure.issue.message});
    }
    for (const auto& warning : result.warnings) {
        diagnostics.push_back({.code = "bloom.scripting.operation-warning",
                               .severity = "warning",
                               .operationIndex = warning.operationIndex,
                               .operation = warning.operationType,
                               .path = {},
                               .message = warning.issue.message});
    }
    if (result.succeeded() || !result.operationFailures.empty())
        return diagnostics;
    if (result.status == commands::CommandStatus::ValidationFailed) {
        std::size_t listed = 0;
        std::size_t omitted = 0;
        for (const auto& issue : result.validation.issues()) {
            if (issue.severity != document::ValidationSeverity::Error)
                continue;
            if (listed == kMaxCommandValidationDiagnostics) {
                ++omitted;
                continue;
            }
            ++listed;
            diagnostics.push_back(
                {.code = "bloom.scripting.validation-failed",
                 .severity = "error",
                 .operationIndex = result.validationOperationIndex,
                 .operation = {},
                 .path = issue.path,
                 .message = issue.message + " (" + issue.path +
                            "); nothing was applied. Change or remove the operation that "
                            "causes it."});
        }
        if (omitted != 0)
            diagnostics.push_back({.code = "bloom.scripting.validation-failed",
                                   .severity = "error",
                                   .operationIndex = result.validationOperationIndex,
                                   .operation = {},
                                   .path = {},
                                   .message = std::to_string(omitted) +
                                              " further validation issues were not listed."});
        if (listed != 0)
            return diagnostics;
    }
    diagnostics.push_back(
        {.code = statusCode(result.status),
         .severity = "error",
         .operationIndex = result.status == commands::CommandStatus::ValidationFailed
                               ? result.validationOperationIndex
                               : std::nullopt,
         .operation = {},
         .path = {},
         .message = statusMessage(result)});
    return diagnostics;
}

} // namespace bloom::scripting
