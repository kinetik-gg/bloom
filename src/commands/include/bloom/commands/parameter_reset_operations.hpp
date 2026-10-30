#pragma once
// Properties > Reset to default, as one command a script or an MCP agent can issue.
#include <bloom/commands/operation.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>

#include <optional>
#include <string_view>

namespace bloom::commands {

// Restores a node parameter to its node definition's default the way the Properties row's
// "Reset to default" does: a driven parameter is disconnected (which restores the default), a
// constant one takes the default value, and an animated one gets a key with the default value at
// `keyTime` (the parameter's own key time, as the parameter-keyed animation operations take it);
// its other keys stay. A parameter with no node binding or definition is refused.
class ResetParameter final : public Operation {
  public:
    ResetParameter(document::CompositionId composition, document::ParameterId parameter,
                   core::RationalTime keyTime)
        : composition_(composition), parameter_(parameter), keyTime_(keyTime) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.parameter.reset";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::ParameterId parameter_;
    core::RationalTime keyTime_;
};

} // namespace bloom::commands
