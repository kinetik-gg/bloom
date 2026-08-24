#pragma once

#include <bloom/commands/result.hpp>
#include <bloom/document/document.hpp>

#include <string_view>

namespace bloom::commands {

class Operation {
  public:
    Operation() = default;
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    Operation(Operation&&) = delete;
    Operation& operator=(Operation&&) = delete;
    virtual ~Operation() = default;

    [[nodiscard]] virtual std::string_view typeId() const noexcept = 0;
    [[nodiscard]] virtual OperationResult apply(document::Draft& draft) const = 0;
};

} // namespace bloom::commands
