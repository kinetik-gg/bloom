#include "layer_lock.hpp"
#include <bloom/commands/command_stack.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

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
        .renderAffecting = true,
        .affectedTimes = std::nullopt,
        .layerIdentityRemaps = std::nullopt,
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

// Counts each retained DocumentState shell and each shared immutable composition payload once,
// even though adjacent history entries refer to the same snapshot. The charge is a deterministic
// authored-content estimate, not allocator RSS (which depends on libc and vector spare capacity).
class HistoryCharge final {
  public:
    template <typename State> void add(const State& state) {
        const auto root = state.rootByteCharge();
        addChunk(state.stateIdentity(),
                 root > std::numeric_limits<std::size_t>::max() - sizeof(document::IdAllocator)
                     ? std::numeric_limits<std::size_t>::max()
                     : root + sizeof(document::IdAllocator));
        for (const auto& composition : state.project().compositions())
            addChunk(composition.contentIdentity(), composition.contentByteCharge());
        for (const auto& record : state.project().extensionRecords())
            if (record.payload.storageIdentity() != nullptr)
                addChunk(record.payload.storageIdentity(), record.payload.size());
        for (const auto& block : state.project().typedDataBlocks())
            if (const auto* payload = std::get_if<document::OpaqueExtensionPayload>(&block.payload);
                payload != nullptr && payload->storageIdentity() != nullptr)
                addChunk(payload->storageIdentity(), payload->size());
    }

    template <typename State> void remove(const State& state) noexcept {
        removeChunk(state.stateIdentity());
        for (const auto& composition : state.project().compositions())
            removeChunk(composition.contentIdentity());
        for (const auto& record : state.project().extensionRecords())
            if (record.payload.storageIdentity() != nullptr)
                removeChunk(record.payload.storageIdentity());
        for (const auto& block : state.project().typedDataBlocks())
            if (const auto* payload = std::get_if<document::OpaqueExtensionPayload>(&block.payload);
                payload != nullptr && payload->storageIdentity() != nullptr)
                removeChunk(payload->storageIdentity());
    }

    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

  private:
    struct Chunk final {
        std::size_t bytes = 0;
        std::size_t references = 0;
    };
    void addChunk(const void* identity, const std::size_t bytes) {
        auto [entry, inserted] = chunks_.try_emplace(identity, Chunk{bytes, 0});
        ++entry->second.references;
        if (inserted)
            bytes_ = bytes > std::numeric_limits<std::size_t>::max() - bytes_
                         ? std::numeric_limits<std::size_t>::max()
                         : bytes_ + bytes;
    }
    void removeChunk(const void* identity) noexcept {
        auto entry = chunks_.find(identity);
        if (entry == chunks_.end())
            return;
        if (--entry->second.references == 0) {
            bytes_ -= entry->second.bytes;
            chunks_.erase(entry);
        }
    }

    std::unordered_map<const void*, Chunk> chunks_;
    std::size_t bytes_ = 0;
};

} // namespace

CommandStack::CommandStack(document::Document& document)
    : CommandStack(document, HistoryLimits{}) {}

CommandStack::CommandStack(document::Document& document, const HistoryLimits limits)
    : limits_(limits), trackedRevision_(document.snapshot().revision()), document_(document) {
    if (limits_.maxEntries == 0 || limits_.maxChargedBytes == 0 ||
        limits_.maxChargedBytes > std::numeric_limits<std::size_t>::max() / 2)
        throw std::invalid_argument("Invalid Bloom undo history limits");
}

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
    bool renderAffecting = false;
    // True as soon as an applied render-affecting operation did NOT prove a finite footprint, which
    // makes the transaction's affected time the conservative whole render.
    bool wholeRender = false;
    std::optional<AffectedTimeFootprint> affectedTimes;
    // SPLIT-1. Ordered remaps, appended in operation order. Any unknown/mixed/cap failure clears
    // them and forces whole render.
    std::vector<LayerIdentityRemap> layerIdentityRemaps;
    bool remapsValid = true;
    std::vector<CommandOutput> outputs;
    std::size_t operationIndex = 0;
    for (const auto& operation : transaction.operations()) {
        OperationResult operationResult = operation->apply(draft);
        ++work_.operationApplies;
        ++work_.lockedLayerChecks;
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
        // Only an operation that actually applied can change pixels. A pixel-affecting operation
        // that reported NoChange contributes nothing, so a layout edit beside it stays layout-only.
        if (operationResult.status == OperationStatus::Applied && operation->renderAffecting()) {
            renderAffecting = true;
            // A proven finite footprint narrows the affected time; an operation that proved none
            // (the default) dominates to the whole render. A mixed-composition union also fails
            // closed to whole render.
            if (operationResult.affectedTimes.has_value() && remapsValid) {
                const auto merged =
                    mergeAffectedTimeFootprints(affectedTimes, *operationResult.affectedTimes);
                if (!merged.has_value()) {
                    wholeRender = true;
                    remapsValid = false;
                } else {
                    affectedTimes = merged;
                    // Ordered remaps ride alongside a finite footprint. Every remap must name the
                    // SAME composition as the finite footprint it accompanies, or the whole
                    // transaction fails closed: normalizing remaps among themselves cannot catch a
                    // footprint/remap composition mismatch. A malformed descriptor, an invalid
                    // composition id, an inverted range, or a cap overflow also fails closed.
                    if (operationResult.layerIdentityRemaps.has_value()) {
                        for (const auto& remap : *operationResult.layerIdentityRemaps) {
                            if (remap.compositionId != merged->compositionId) {
                                wholeRender = true;
                                remapsValid = false;
                                break;
                            }
                        }
                    }
                    if (remapsValid && operationResult.layerIdentityRemaps.has_value()) {
                        const auto normalized = normalizeLayerIdentityRemaps([&] {
                            auto combined = layerIdentityRemaps;
                            combined.insert(combined.end(),
                                            operationResult.layerIdentityRemaps->begin(),
                                            operationResult.layerIdentityRemaps->end());
                            return combined;
                        }());
                        if (normalized.has_value())
                            layerIdentityRemaps = *normalized;
                        else {
                            wholeRender = true;
                            remapsValid = false;
                        }
                    }
                }
            } else {
                // An operation that proved no finite footprint, or a transaction already failed
                // closed on remaps, dominates to whole render and can never publish remaps.
                wholeRender = true;
                remapsValid = false;
            }
        }
        ++operationIndex;
    }

    if (!changed) {
        auto result = makeResult(CommandAction::Execute, CommandStatus::NoChange, before,
                                 std::string(transaction.label()));
        result.outputs = std::move(outputs);
        notify(result);
        return result;
    }

    // Preflight the charge of the new undo step before publication. A single step too large for
    // the policy is refused atomically; older steps are retired only when the new one fits.
    HistoryCharge oneStep;
    oneStep.add(before);
    oneStep.add(std::as_const(draft));
    work_.chargedSnapshotsVisited += 2;
    if (oneStep.bytes() > limits_.maxChargedBytes) {
        // Keep malformed-draft diagnostics ahead of resource policy, as Document::commit does.
        // Ordinary fits still pass through commit's single full validation.
        ++work_.overBudgetValidations;
        auto validation = draft.validate();
        if (!validation.ok()) {
            auto invalid = makeResult(CommandAction::Execute, CommandStatus::ValidationFailed,
                                      before, std::string(transaction.label()));
            invalid.validation = std::move(validation);
            notify(invalid);
            return invalid;
        }
        if (document_.snapshot().revision() != before.revision()) {
            auto stale = makeResult(CommandAction::Execute, CommandStatus::StaleRevision, before,
                                    std::string(transaction.label()));
            stale.expectedRevision = before.revision();
            notify(stale);
            return stale;
        }
        if (before.revision().value() == std::numeric_limits<std::uint64_t>::max()) {
            auto overflow = makeResult(CommandAction::Execute, CommandStatus::RevisionOverflow,
                                       before, std::string(transaction.label()));
            notify(overflow);
            return overflow;
        }
        auto result = makeResult(CommandAction::Execute, CommandStatus::HistoryLimit, before,
                                 std::string(transaction.label()));
        notify(result);
        return result;
    }

    HistoryCharge projected;
    for (std::size_t index = 0; index < cursor_; ++index) {
        projected.add(history_[index].before);
        projected.add(history_[index].after);
        work_.chargedSnapshotsVisited += 2;
    }
    projected.add(before);
    projected.add(std::as_const(draft));
    work_.chargedSnapshotsVisited += 2;
    std::size_t oldestDropped = 0;
    auto projectedEntries = cursor_ + 1;
    while (projectedEntries > limits_.maxEntries || projected.bytes() > limits_.maxChargedBytes) {
        projected.remove(history_[oldestDropped].before);
        projected.remove(history_[oldestDropped].after);
        ++oldestDropped;
        --projectedEntries;
    }

    // Stage every allocation needed for history and event publication before Document::commit.
    // A committed edit must never be left without its newest undo step by a later allocation.
    auto result = makeResult(CommandAction::Execute, CommandStatus::Succeeded, before,
                             std::string(transaction.label()));
    result.outputs = std::move(outputs);
    result.renderAffecting = renderAffecting;
    // A finite footprint is published only when the whole transaction's render effect is confined
    // to it. A layout-only transaction (renderAffecting false) publishes none, matching its
    // render-neutral semantics.
    if (renderAffecting && !wholeRender && remapsValid && affectedTimes.has_value()) {
        result.affectedTimes = affectedTimes;
        // Remaps require the whole transaction to be finite; a nonempty set is published only when
        // every descriptor validated. An empty set is a legitimate "no remaps".
        const auto normalized = normalizeLayerIdentityRemaps(layerIdentityRemaps);
        if (normalized.has_value())
            result.layerIdentityRemaps = *normalized;
        else
            result.affectedTimes.reset();
    }
    result.historyEntriesDropped = oldestDropped;
    HistoryEntry staged{
        std::string(transaction.label()), before, before, renderAffecting, result.affectedTimes,
        result.layerIdentityRemaps};
    history_.reserve(history_.size() + 1);
    static_assert(std::is_nothrow_move_constructible_v<HistoryEntry> &&
                  std::is_nothrow_move_assignable_v<HistoryEntry>);

    document::CommitResult commitResult = document_.commit(before.revision(), std::move(draft));
    if (!commitResult.committed() || !commitResult.snapshot.has_value()) {
        auto failed = resultForCommit(CommandAction::Execute, std::string(transaction.label()),
                                      before, std::move(commitResult));
        notify(failed);
        return failed;
    }

    const document::Snapshot after = *commitResult.snapshot;
    result.afterRevision = after.revision();
    staged.after = after;
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    history_.push_back(std::move(staged));
    history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(oldestDropped));
    cursor_ = history_.size();
    historyChargedBytes_ = projected.bytes();
    work_.historyEntriesRetired += oldestDropped;
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
    result.renderAffecting = entry.renderAffecting;
    result.affectedTimes = entry.affectedTimes;
    // Undo reverses order and swaps before/after IDs.
    if (entry.layerIdentityRemaps.has_value() && !entry.layerIdentityRemaps->empty())
        result.layerIdentityRemaps = invertLayerIdentityRemaps(entry.layerIdentityRemaps);
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
    result.renderAffecting = entry.renderAffecting;
    result.affectedTimes = entry.affectedTimes;
    // Redo replays the stored FORWARD ordered list.
    result.layerIdentityRemaps = entry.layerIdentityRemaps;
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
    historyChargedBytes_ = 0;
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
