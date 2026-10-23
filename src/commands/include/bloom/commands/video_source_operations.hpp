#pragma once
#include <bloom/commands/operation.hpp>
#include <bloom/document/ids.hpp>
#include <cstdint>

// Document 1.25 Video node commands.
namespace bloom::commands {

// Chooses which of a Video node asset's audio streams (0-based among its audio streams) the node
// plays. Nodes created before the parameter existed have no binding; the first choice creates the
// non-animatable constant parameter and binds it, later choices change its value. Respects the
// node lock and refuses a stream the asset does not have.
class SetVideoAudioStream final : public Operation {
  public:
    SetVideoAudioStream(document::CompositionId composition, document::NodeId node,
                        std::uint32_t audioStream)
        : composition_(composition), node_(node), audioStream_(audioStream) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId>
    parameterEditScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::NodeId node_;
    std::uint32_t audioStream_ = 0;
};

} // namespace bloom::commands
