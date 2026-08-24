#pragma once

#include <bloom/commands/result.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::commands {

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

  private:
    struct HistoryEntry {
        std::string label;
        document::Snapshot before;
        document::Snapshot after;
    };

    [[nodiscard]] std::optional<CommandResult> staleResult(CommandAction action, std::string label,
                                                           const document::Snapshot& current) const;

    std::vector<HistoryEntry> history_;
    std::size_t cursor_ = 0;
    document::Revision trackedRevision_;
    document::Document& document_;
};

} // namespace bloom::commands
