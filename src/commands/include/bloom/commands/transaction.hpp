#pragma once

#include <bloom/commands/operation.hpp>
#include <bloom/document/document.hpp>

#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::commands {

class Transaction final {
  public:
    explicit Transaction(std::string label,
                         std::optional<document::Revision> expectedRevision = std::nullopt)
        : label_(std::move(label)), expectedRevision_(expectedRevision) {}

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) noexcept = default;
    Transaction& operator=(Transaction&&) noexcept = default;
    ~Transaction() = default;

    template <typename OperationType, typename... Args>
        requires std::derived_from<OperationType, Operation>
    OperationType& emplace(Args&&... args) {
        auto operation = std::make_unique<OperationType>(std::forward<Args>(args)...);
        OperationType& result = *operation;
        operations_.push_back(std::move(operation));
        return result;
    }

    [[nodiscard]] bool add(std::unique_ptr<Operation> operation);
    [[nodiscard]] std::string_view label() const noexcept { return label_; }
    [[nodiscard]] std::optional<document::Revision> expectedRevision() const noexcept {
        return expectedRevision_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return operations_.size(); }
    [[nodiscard]] bool empty() const noexcept { return operations_.empty(); }
    [[nodiscard]] const std::vector<std::unique_ptr<Operation>>& operations() const noexcept {
        return operations_;
    }

  private:
    std::string label_;
    std::optional<document::Revision> expectedRevision_;
    std::vector<std::unique_ptr<Operation>> operations_;
};

} // namespace bloom::commands
