#include <bloom/commands/command_stack.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace bloom::commands {
namespace {

CommandResult makeResult(CommandAction action, CommandStatus status,
                         const document::Snapshot& snapshot, std::string label = {}) {
    return {
        .action = action,
        .status = status,
        .beforeRevision = snapshot.revision(),
        .afterRevision = snapshot.revision(),
        .expectedRevision = std::nullopt,
        .label = std::move(label),
        .operationFailures = {},
        .outputs = {},
        .validation = {},
    };
}

CommandStatus statusForCommit(document::CommitStatus status) {
    switch (status) {
    case document::CommitStatus::Committed:
        return CommandStatus::Succeeded;
    case document::CommitStatus::RevisionConflict:
        return CommandStatus::StaleRevision;
    case document::CommitStatus::InvalidDraft:
        return CommandStatus::ValidationFailed;
    case document::CommitStatus::RevisionOverflow:
        return CommandStatus::RevisionOverflow;
    case document::CommitStatus::ForeignDocument:
        return CommandStatus::ForeignDocument;
    case document::CommitStatus::DraftBaseMismatch:
        return CommandStatus::DraftBaseMismatch;
    }
    return CommandStatus::Rejected;
}

CommandResult resultForCommit(CommandAction action, std::string label,
                              const document::Snapshot& before,
                              document::CommitResult&& commitResult) {
    auto result =
        makeResult(action, statusForCommit(commitResult.status), before, std::move(label));
    result.validation = std::move(commitResult.validation);
    if (commitResult.status == document::CommitStatus::RevisionConflict) {
        result.expectedRevision = before.revision();
    }
    if (commitResult.snapshot.has_value()) {
        result.afterRevision = commitResult.snapshot->revision();
    }
    return result;
}

} // namespace

CommandStack::CommandStack(document::Document& document)
    : trackedRevision_(document.snapshot().revision()), document_(document) {}

CommandResult CommandStack::execute(Transaction&& transaction) {
    const document::Snapshot before = document_.snapshot();
    if (const auto stale =
            staleResult(CommandAction::Execute, std::string(transaction.label()), before)) {
        return *stale;
    }
    if (transaction.expectedRevision().has_value() &&
        *transaction.expectedRevision() != before.revision()) {
        auto result = makeResult(CommandAction::Execute, CommandStatus::StaleRevision, before,
                                 std::string(transaction.label()));
        result.expectedRevision = transaction.expectedRevision();
        return result;
    }
    if (transaction.empty()) {
        return makeResult(CommandAction::Execute, CommandStatus::NoChange, before,
                          std::string(transaction.label()));
    }

    document::Draft draft = document_.draft(before);
    bool changed = false;
    std::vector<CommandOutput> outputs;
    std::size_t operationIndex = 0;
    for (const auto& operation : transaction.operations()) {
        OperationResult operationResult = operation->apply(draft);
        if (operationResult.status == OperationStatus::Rejected) {
            auto result = makeResult(CommandAction::Execute, CommandStatus::Rejected, before,
                                     std::string(transaction.label()));
            if (operationResult.issues.empty()) {
                operationResult.issues.push_back(
                    {OperationIssueCode::InvalidValue, "Operation rejected the transaction"});
            }
            for (auto& issue : operationResult.issues) {
                result.operationFailures.push_back(
                    {operationIndex, std::string(operation->typeId()), std::move(issue)});
            }
            return result;
        }
        for (auto& output : operationResult.outputs) {
            outputs.push_back(
                {operationIndex, std::string(operation->typeId()), std::move(output)});
        }
        changed = changed || operationResult.status == OperationStatus::Applied;
        ++operationIndex;
    }

    if (!changed) {
        auto result = makeResult(CommandAction::Execute, CommandStatus::NoChange, before,
                                 std::string(transaction.label()));
        result.outputs = std::move(outputs);
        return result;
    }

    document::CommitResult commitResult = document_.commit(before.revision(), std::move(draft));
    if (!commitResult.committed()) {
        return resultForCommit(CommandAction::Execute, std::string(transaction.label()), before,
                               std::move(commitResult));
    }

    document::Snapshot after = *commitResult.snapshot;
    auto result = resultForCommit(CommandAction::Execute, std::string(transaction.label()), before,
                                  std::move(commitResult));
    result.outputs = std::move(outputs);
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    history_.push_back({std::string(transaction.label()), before, after});
    cursor_ = history_.size();
    trackedRevision_ = after.revision();
    return result;
}

CommandResult CommandStack::undo() {
    const document::Snapshot before = document_.snapshot();
    if (!canUndo()) {
        return makeResult(CommandAction::Undo, CommandStatus::NothingToUndo, before);
    }

    const HistoryEntry& entry = history_[cursor_ - 1];
    if (const auto stale = staleResult(CommandAction::Undo, entry.label, before)) {
        return *stale;
    }

    document::CommitResult restoreResult = document_.restore(before.revision(), entry.before);
    if (!restoreResult.committed()) {
        return resultForCommit(CommandAction::Undo, entry.label, before, std::move(restoreResult));
    }

    const document::Revision restoredRevision = restoreResult.snapshot->revision();
    auto result =
        resultForCommit(CommandAction::Undo, entry.label, before, std::move(restoreResult));
    --cursor_;
    trackedRevision_ = restoredRevision;
    return result;
}

CommandResult CommandStack::redo() {
    const document::Snapshot before = document_.snapshot();
    if (!canRedo()) {
        return makeResult(CommandAction::Redo, CommandStatus::NothingToRedo, before);
    }

    const HistoryEntry& entry = history_[cursor_];
    if (const auto stale = staleResult(CommandAction::Redo, entry.label, before)) {
        return *stale;
    }

    document::CommitResult restoreResult = document_.restore(before.revision(), entry.after);
    if (!restoreResult.committed()) {
        return resultForCommit(CommandAction::Redo, entry.label, before, std::move(restoreResult));
    }

    const document::Revision restoredRevision = restoreResult.snapshot->revision();
    auto result =
        resultForCommit(CommandAction::Redo, entry.label, before, std::move(restoreResult));
    ++cursor_;
    trackedRevision_ = restoredRevision;
    return result;
}

std::optional<std::string_view> CommandStack::undoLabel() const noexcept {
    if (!canUndo()) {
        return std::nullopt;
    }
    return history_[cursor_ - 1].label;
}

std::optional<std::string_view> CommandStack::redoLabel() const noexcept {
    if (!canRedo()) {
        return std::nullopt;
    }
    return history_[cursor_].label;
}

void CommandStack::clear() {
    history_.clear();
    cursor_ = 0;
    trackedRevision_ = document_.snapshot().revision();
}

std::optional<CommandResult> CommandStack::staleResult(CommandAction action, std::string label,
                                                       const document::Snapshot& current) const {
    if (trackedRevision_ == current.revision()) {
        return std::nullopt;
    }

    auto result = makeResult(action, CommandStatus::StaleRevision, current, std::move(label));
    result.expectedRevision = trackedRevision_;
    return result;
}

} // namespace bloom::commands
