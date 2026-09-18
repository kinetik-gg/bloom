#include "layer_lock.hpp"
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
    const auto status = commitResult.status == document::CommitStatus::Committed &&
                                !commitResult.snapshot.has_value()
                            ? CommandStatus::Rejected
                            : statusForCommit(commitResult.status);
    auto result = makeResult(action, status, before, std::move(label));
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

CommandObserverId CommandStack::addObserver(CommandObserver observer) {
    if (!observer) {
        return 0;
    }
    const std::scoped_lock lock(observerMutex_);
    if (nextObserverId_ == 0) {
        return 0;
    }
    const auto id = nextObserverId_++;
    if (nextObserverId_ == 0) {
        nextObserverId_ = 0;
    }
    observers_.emplace_back(id, std::move(observer));
    return id;
}

void CommandStack::removeObserver(const CommandObserverId observerId) noexcept {
    if (observerId == 0) {
        return;
    }
    const std::scoped_lock lock(observerMutex_);
    std::erase_if(observers_,
                  [observerId](const auto& entry) { return entry.first == observerId; });
}

void CommandStack::notify(const CommandResult& result) const noexcept {
    std::vector<CommandObserver> callbacks;
    try {
        const std::scoped_lock lock(observerMutex_);
        callbacks.reserve(observers_.size());
        for (const auto& [id, observer] : observers_) {
            static_cast<void>(id);
            callbacks.push_back(observer);
        }
    } catch (...) {
        return;
    }

    const auto send = [&callbacks, &result](const CommandEventKind kind) noexcept {
        for (const auto& callback : callbacks) {
            try {
                callback(CommandEvent{.kind = kind, .result = result});
            } catch (...) {
                // Observers cannot change the command outcome or interrupt history publication.
                static_cast<void>(0);
            }
        }
    };
    if (!result.succeeded()) {
        send(CommandEventKind::Rejected);
        return;
    }
    if (result.changed()) {
        send(CommandEventKind::RevisionChanged);
    }
    send(CommandEventKind::HistoryChanged);
}

CommandResult CommandStack::execute(Transaction&& transaction) {
    const document::Snapshot before = document_.snapshot();
    if (const auto stale =
            staleResult(CommandAction::Execute, std::string(transaction.label()), before)) {
        auto result = *stale;
        notify(result);
        return result;
    }
    if (transaction.expectedRevision().has_value() &&
        *transaction.expectedRevision() != before.revision()) {
        auto result = makeResult(CommandAction::Execute, CommandStatus::StaleRevision, before,
                                 std::string(transaction.label()));
        result.expectedRevision = transaction.expectedRevision();
        notify(result);
        return result;
    }
    if (transaction.empty()) {
        auto result = makeResult(CommandAction::Execute, CommandStatus::NoChange, before,
                                 std::string(transaction.label()));
        notify(result);
        return result;
    }

    document::Draft draft = document_.draft(before);
    bool changed = false;
    std::vector<CommandOutput> outputs;
    std::size_t operationIndex = 0;
    for (const auto& operation : transaction.operations()) {
        OperationResult operationResult = operation->apply(draft);
        if (operationResult.status != OperationStatus::Rejected &&
            detail::changesLockedLayers(before.project(), draft.project()))
            operationResult = OperationResult::rejected(
                OperationIssueCode::InvalidValue, "Layer is locked; unlock it before editing");
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
            notify(result);
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
        notify(result);
        return result;
    }

    document::CommitResult commitResult = document_.commit(before.revision(), std::move(draft));
    if (!commitResult.committed() || !commitResult.snapshot.has_value()) {
        auto result = resultForCommit(CommandAction::Execute, std::string(transaction.label()),
                                      before, std::move(commitResult));
        notify(result);
        return result;
    }

    document::Snapshot after = *commitResult.snapshot;
    auto result = resultForCommit(CommandAction::Execute, std::string(transaction.label()), before,
                                  std::move(commitResult));
    result.outputs = std::move(outputs);
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    history_.push_back({std::string(transaction.label()), before, after});
    cursor_ = history_.size();
    trackedRevision_ = after.revision();
    notify(result);
    return result;
}

CommandResult CommandStack::undo() {
    const document::Snapshot before = document_.snapshot();
    if (!canUndo()) {
        auto result = makeResult(CommandAction::Undo, CommandStatus::NothingToUndo, before);
        notify(result);
        return result;
    }

    const HistoryEntry& entry = history_[cursor_ - 1];
    if (const auto stale = staleResult(CommandAction::Undo, entry.label, before)) {
        auto result = *stale;
        notify(result);
        return result;
    }

    document::CommitResult restoreResult = document_.restore(before.revision(), entry.before);
    if (!restoreResult.committed() || !restoreResult.snapshot.has_value()) {
        auto result =
            resultForCommit(CommandAction::Undo, entry.label, before, std::move(restoreResult));
        notify(result);
        return result;
    }

    const document::Revision restoredRevision = restoreResult.snapshot->revision();
    auto result =
        resultForCommit(CommandAction::Undo, entry.label, before, std::move(restoreResult));
    --cursor_;
    trackedRevision_ = restoredRevision;
    notify(result);
    return result;
}

CommandResult CommandStack::redo() {
    const document::Snapshot before = document_.snapshot();
    if (!canRedo()) {
        auto result = makeResult(CommandAction::Redo, CommandStatus::NothingToRedo, before);
        notify(result);
        return result;
    }

    const HistoryEntry& entry = history_[cursor_];
    if (const auto stale = staleResult(CommandAction::Redo, entry.label, before)) {
        auto result = *stale;
        notify(result);
        return result;
    }

    document::CommitResult restoreResult = document_.restore(before.revision(), entry.after);
    if (!restoreResult.committed() || !restoreResult.snapshot.has_value()) {
        auto result =
            resultForCommit(CommandAction::Redo, entry.label, before, std::move(restoreResult));
        notify(result);
        return result;
    }

    const document::Revision restoredRevision = restoreResult.snapshot->revision();
    auto result =
        resultForCommit(CommandAction::Redo, entry.label, before, std::move(restoreResult));
    ++cursor_;
    trackedRevision_ = restoredRevision;
    notify(result);
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
    if (history_.empty() && cursor_ == 0) {
        return;
    }
    history_.clear();
    cursor_ = 0;
    const auto snapshot = document_.snapshot();
    trackedRevision_ = snapshot.revision();
    notify(makeResult(CommandAction::Execute, CommandStatus::NoChange, snapshot));
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
