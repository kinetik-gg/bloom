#pragma once

#include <bloom/commands/result.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::commands {

enum class CommandEventKind : std::uint8_t {
    RevisionChanged,
    HistoryChanged,
    Rejected,
};

struct CommandEvent final {
    CommandEventKind kind = CommandEventKind::Rejected;
    CommandResult result;
};

using CommandObserverId = std::uint64_t;
using CommandObserver = std::function<void(const CommandEvent&)>;

class CommandStack final {
  public:
    explicit CommandStack(document::Document& document);
    CommandStack(const CommandStack&) = delete;
    CommandStack& operator=(const CommandStack&) = delete;
    CommandStack(CommandStack&&) = delete;
    CommandStack& operator=(CommandStack&&) = delete;
    ~CommandStack() = default;

    [[nodiscard]] CommandResult execute(Transaction&& transaction);
    [[nodiscard]] CommandResult undo();
    [[nodiscard]] CommandResult redo();

    [[nodiscard]] bool canUndo() const noexcept { return cursor_ > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return cursor_ < history_.size(); }
    [[nodiscard]] std::optional<std::string_view> undoLabel() const noexcept;
    [[nodiscard]] std::optional<std::string_view> redoLabel() const noexcept;
    [[nodiscard]] document::Revision trackedRevision() const noexcept { return trackedRevision_; }
    [[nodiscard]] std::size_t size() const noexcept { return history_.size(); }

    void clear();

    [[nodiscard]] CommandObserverId addObserver(CommandObserver observer);
    void removeObserver(CommandObserverId observerId) noexcept;

  private:
    struct HistoryEntry {
        std::string label;
        document::Snapshot before;
        document::Snapshot after;
        // The render impact of the transaction that created this entry. Restoring the entry on
        // undo/redo replays it, because a restore does not re-run the operations that declared it.
        bool renderAffecting = true;
        // The proven finite changed-time footprint, when the transaction proved one; std::nullopt
        // is the conservative whole-render default. Replayed verbatim by undo/redo.
        std::optional<AffectedTimeFootprint> affectedTimes;
        // SPLIT-1. The transaction's FORWARD ordered geometry remaps, when every applied
        // render-affecting op proved a finite footprint. Undo publishes the inverted list; redo
        // publishes this stored forward list.
        std::optional<std::vector<LayerIdentityRemap>> layerIdentityRemaps;
    };

    [[nodiscard]] std::optional<CommandResult> staleResult(CommandAction action, std::string label,
                                                           const document::Snapshot& current) const;
    void notify(const CommandResult& result) const noexcept;

    std::vector<HistoryEntry> history_;
    std::size_t cursor_ = 0;
    document::Revision trackedRevision_;
    document::Document& document_;
    mutable std::mutex observerMutex_;
    std::vector<std::pair<CommandObserverId, CommandObserver>> observers_;
    CommandObserverId nextObserverId_ = 1;
};

} // namespace bloom::commands
