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
class SetLayerEnabled final : public Operation {
 public:
    SetLayerEnabled(document::CompositionId composition, document::LayerId layer, bool value)
        : composition_(composition), layer_(layer), value_(value) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
 private:
    document::CompositionId composition_;
    document::LayerId layer_;
    bool value_;
};
class SetLayerSolo final : public Operation {
 public:
    SetLayerSolo(document::CompositionId composition, document::LayerId layer, bool value)
        : composition_(composition), layer_(layer), value_(value) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
 private:
    document::CompositionId composition_;
    document::LayerId layer_;
    bool value_;
};
class SetLayerLocked final : public Operation {
 public:
    SetLayerLocked(document::CompositionId composition, document::LayerId layer, bool value)
        : composition_(composition), layer_(layer), value_(value) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
 private:
    document::CompositionId composition_;
    document::LayerId layer_;
    bool value_;
};
class SetLayerLabelColor final : public Operation {
 public:
    SetLayerLabelColor(document::CompositionId composition, document::LayerId layer,
        std::optional<std::array<std::uint8_t, 3>> color)
        : composition_(composition), layer_(layer), color_(color) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
 private:
    document::CompositionId composition_;
    document::LayerId layer_;
    std::optional<std::array<std::uint8_t, 3>> color_;
};
class SetWorkArea final : public Operation {
 public:
    SetWorkArea(document::CompositionId composition, core::RationalTime start, core::RationalTime end)
        : composition_(composition), start_(start), end_(end) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
 private:
    document::CompositionId composition_;
    core::RationalTime start_, end_;
};
class ClearWorkArea final : public Operation {
 public:
    explicit ClearWorkArea(document::CompositionId composition) : composition_(composition) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
 private:
    document::CompositionId composition_;
};
} // namespace bloom::commands
