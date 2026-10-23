#pragma once
#include <bloom/commands/operation.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/layer_time.hpp>

// ADR 0024 layer-owned time commands. Each one edits one layer boundary atomically, respects the
// layer lock, and publishes the composition-time footprint its change is confined to.
namespace bloom::commands {

// Sets a layer's start time, activity range and speed together: a body move shifts all three
// times, a trim changes the range only, a slip changes the start only. In and out snap to the
// composition frame grid (outside the composition too); the start time stays exact. The range
// must keep at least part of itself inside [0, duration).
class SetLayerTiming final : public Operation {
  public:
    SetLayerTiming(document::CompositionId composition, document::LayerId layer,
                   core::RationalTime startTime, core::RationalTime in, core::RationalTime out,
                   document::LayerSpeed speed = {})
        : composition_(composition), layer_(layer), startTime_(startTime), in_(in), out_(out),
          speed_(speed) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    core::RationalTime startTime_, in_, out_;
    document::LayerSpeed speed_;
};

// Sets the out-of-extent modes and the content extent of a layer.
class SetLayerTimeModes final : public Operation {
  public:
    SetLayerTimeModes(document::CompositionId composition, document::LayerId layer,
                      document::LayerTimeMode before, document::LayerTimeMode after)
        : composition_(composition), layer_(layer), before_(before), after_(after) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    document::LayerTimeMode before_, after_;
};

class SetLayerContentExtent final : public Operation {
  public:
    SetLayerContentExtent(document::CompositionId composition, document::LayerId layer,
                          document::LayerContentExtent extent)
        : composition_(composition), layer_(layer), extent_(std::move(extent)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    document::LayerContentExtent extent_;
};

// Binds an animatable Time Remap on the layer's Layer Output (schema 5) with two Linear keys that
// map its range identically: content time equals layer time at the in and out points.
class EnableLayerTimeRemap final : public Operation {
  public:
    EnableLayerTimeRemap(document::CompositionId composition, document::LayerId layer)
        : composition_(composition), layer_(layer) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
};

// Unbinds the Time Remap parameter and erases it with its curve.
class DisableLayerTimeRemap final : public Operation {
  public:
    DisableLayerTimeRemap(document::CompositionId composition, document::LayerId layer)
        : composition_(composition), layer_(layer) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
};

} // namespace bloom::commands
