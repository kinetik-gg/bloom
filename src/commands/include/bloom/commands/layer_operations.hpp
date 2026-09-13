#pragma once
#include <bloom/commands/operation.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>

namespace bloom::commands {
class SetLayerRange final : public Operation {
 public:
    SetLayerRange(document::CompositionId composition, document::LayerId layer,
                  core::RationalTime in, core::RationalTime out)
        : composition_(composition), layer_(layer), in_(in), out_(out) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
 private:
    document::CompositionId composition_;
    document::LayerId layer_;
    core::RationalTime in_, out_;
};
class SplitLayerAtTime final : public Operation {
 public:
    SplitLayerAtTime(document::CompositionId composition, document::LayerId layer, core::RationalTime time)
        : composition_(composition), layer_(layer), time_(time) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
 private:
    document::CompositionId composition_;
    document::LayerId layer_;
    core::RationalTime time_;
};
} // namespace bloom::commands
