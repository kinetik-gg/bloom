#pragma once

#include <bloom/commands/result.hpp>
#include <bloom/document/document.hpp>

#include <optional>
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

    // Issue #355. An operation that changes ONLY parameter sources, animation curves and keyframes
    // of one composition -- never graph topology, node layout, or composition settings -- names
    // that composition here. When it publishes no footprint of its own, the command stack proves
    // one once per transaction by diffing the composition's parameters and curves (see
    // render_footprint.hpp). The default (std::nullopt) keeps the whole-render default.
    [[nodiscard]] virtual std::optional<document::CompositionId>
    parameterEditScope() const noexcept {
        return std::nullopt;
    }

    // Issue #355. The single composition whose content this operation can change, so a whole-render
    // result still names the composition it touched (CommandResult::affectedComposition). An
    // operation that edits project-wide state (assets, colour settings, compositions themselves)
    // must keep the default, which means "any composition".
    [[nodiscard]] virtual std::optional<document::CompositionId> renderScope() const noexcept {
        return parameterEditScope();
    }
};

} // namespace bloom::commands
