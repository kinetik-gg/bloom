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

    // Whether applying this operation can change the composition's rendered pixels. The default is
    // deliberately conservative: an operation that does not opt out is treated as render-affecting,
    // so a new operation can never silently inherit the layout-only classification. Only operations
    // that provably touch no compiled or evaluated input (for example the node-layout card
    // geometry) return false.
    [[nodiscard]] virtual bool renderAffecting() const noexcept { return true; }
};

} // namespace bloom::commands
