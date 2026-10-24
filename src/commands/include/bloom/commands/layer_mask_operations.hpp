#pragma once
#include <bloom/commands/operation.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

// ADR 0028 layer-mask and track-matte commands. Each edits one layer boundary atomically, respects
// the layer lock, and publishes the layer's whole active span as its footprint.
namespace bloom::commands {

// Appends a mask with constant parameters (feather 0, expansion 0, opacity 1). An empty name
// becomes "Mask N". Outputs: mask, path/feather/expansion/opacity parameters.
class AddLayerMask final : public Operation {
  public:
    AddLayerMask(document::CompositionId composition, document::LayerId layer,
                 document::PathValue path,
                 document::LayerMaskMode mode = document::LayerMaskMode::Add, std::string name = {})
        : composition_(composition), layer_(layer), path_(std::move(path)), mode_(mode),
          name_(std::move(name)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    document::PathValue path_;
    document::LayerMaskMode mode_;
    std::string name_;
};

// Erases the mask, its four parameters and any curves they own.
class RemoveLayerMask final : public Operation {
  public:
    RemoveLayerMask(document::CompositionId composition, document::LayerId layer,
                    document::LayerMaskId mask)
        : composition_(composition), layer_(layer), mask_(mask) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    document::LayerMaskId mask_;
};

// Moves the mask to newIndex in the ordered list.
class MoveLayerMask final : public Operation {
  public:
    MoveLayerMask(document::CompositionId composition, document::LayerId layer,
                  document::LayerMaskId mask, std::size_t newIndex)
        : composition_(composition), layer_(layer), mask_(mask), newIndex_(newIndex) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    document::LayerMaskId mask_;
    std::size_t newIndex_;
};
class SetLayerMaskMode final : public Operation {
  public:
    SetLayerMaskMode(document::CompositionId composition, document::LayerId layer,
                     document::LayerMaskId mask, document::LayerMaskMode mode)
        : composition_(composition), layer_(layer), mask_(mask), mode_(mode) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    document::LayerMaskId mask_;
    document::LayerMaskMode mode_;
};
class SetLayerMaskInverted final : public Operation {
  public:
    SetLayerMaskInverted(document::CompositionId composition, document::LayerId layer,
                         document::LayerMaskId mask, bool inverted)
        : composition_(composition), layer_(layer), mask_(mask), inverted_(inverted) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    document::LayerMaskId mask_;
    bool inverted_;
};
class RenameLayerMask final : public Operation {
  public:
    RenameLayerMask(document::CompositionId composition, document::LayerId layer,
                    document::LayerMaskId mask, std::string name)
        : composition_(composition), layer_(layer), mask_(mask), name_(std::move(name)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    document::LayerMaskId mask_;
    std::string name_;
};

// Replaces a constant path, or sets the key at exactly `time` of an animated path (updating the key
// there or inserting a Linear one). `time` is the parameter's own clock time: callers map
// composition time through the layer's timing first, because commands do not depend on the runtime.
class SetLayerMaskPath final : public Operation {
  public:
    SetLayerMaskPath(document::CompositionId composition, document::LayerId layer,
                     document::LayerMaskId mask, core::RationalTime time, document::PathValue path)
        : composition_(composition), layer_(layer), mask_(mask), time_(time),
          path_(std::move(path)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    document::LayerMaskId mask_;
    core::RationalTime time_;
    document::PathValue path_;
};

// Turns a constant path into a Path curve with one key at `time` (parameter clock time) holding the
// constant.
class AnimateLayerMaskPath final : public Operation {
  public:
    AnimateLayerMaskPath(document::CompositionId composition, document::LayerId layer,
                         document::LayerMaskId mask, core::RationalTime time)
        : composition_(composition), layer_(layer), mask_(mask), time_(time) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    document::LayerMaskId mask_;
    core::RationalTime time_;
};

// Erases the Path curve and makes the path a constant `value`.
class StopAnimatingLayerMaskPath final : public Operation {
  public:
    StopAnimatingLayerMaskPath(document::CompositionId composition, document::LayerId layer,
                               document::LayerMaskId mask, document::PathValue value)
        : composition_(composition), layer_(layer), mask_(mask), value_(std::move(value)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId layer_;
    document::LayerMaskId mask_;
    document::PathValue value_;
};

// Sets or clears the target layer's track matte. The source must be another layer of the same
// Merge and the matte relation must stay acyclic. The footprint unions the target and the old and
// new source spans, because a matte source is hidden from its Merge.
class SetTrackMatte final : public Operation {
  public:
    SetTrackMatte(document::CompositionId composition, document::LayerId target,
                  std::optional<document::TrackMatte> matte)
        : composition_(composition), target_(target), matte_(matte) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::LayerId target_;
    std::optional<document::TrackMatte> matte_;
};

} // namespace bloom::commands
