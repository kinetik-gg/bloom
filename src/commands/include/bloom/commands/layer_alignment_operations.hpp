#pragma once

// Align and distribute layers (ADR 0031). One operation moves every requested layer so its
// evaluated visual bounds line up with a target, as one transaction and one undo step.
//
// The command is Qt-free and evaluation-free: it never samples a curve or evaluates a frame.
// Its caller measures each layer at the alignment time and passes the result as
// LayerAlignmentGeometry -- the interactive Viewer from its delivered preview frame, scripting and
// MCP from the headless CPU reference query (host::measureLayerAlignmentGeometry). Both paths build
// the geometry with host::layerAlignmentGeometry, so the same evaluated bounds give the same move.
//
// Semantics (docs/architecture/layer-alignment.md):
// - Bounds are each layer's evaluated output bounds at the time: the axis-aligned composition-space
//   rectangle around its transformed content, unclipped by the composition frame.
// - Composition targets the composition frame, Selection the union of every requested layer's
//   bounds, Key Object one layer's bounds; the key layer itself never moves.
// - A move is a composition-space delta. It is converted into the layer's PARENT space through the
//   parent's evaluated world transform (rotation, scale and shear; a translation delta does not
//   depend on any anchor) and added to Position. A layer whose moving ancestor already carries it
//   moves only by the remainder.
// - An animated Position gets a key at the time (an existing key there is updated); its other
//   keys are untouched. A constant Position is set.
// - Locked layers and layers whose Position is driven are skipped with a reason, never silently.
//   When every layer is skipped the operation is rejected and nothing changes.
// - Distribute keeps the centres of the outermost layers (Selection), puts the outermost layers'
//   outer edges on the composition frame's edges (Composition), or keeps the key fixed as well
//   (Key Object, piecewise between the fixed layers), and spaces every other centre evenly.

#include <bloom/commands/operation.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/layer_time.hpp>
#include <bloom/document/parameter.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::document {
class Composition;
} // namespace bloom::document

namespace bloom::commands {

enum class LayerArrangement : std::uint8_t {
    AlignLeft,
    AlignHorizontalCentre,
    AlignRight,
    AlignTop,
    AlignVerticalMiddle,
    AlignBottom,
    DistributeHorizontalCentres,
    DistributeVerticalCentres,
};

[[nodiscard]] constexpr bool isDistribution(const LayerArrangement arrangement) noexcept {
    return arrangement == LayerArrangement::DistributeHorizontalCentres ||
           arrangement == LayerArrangement::DistributeVerticalCentres;
}

enum class AlignmentTarget : std::uint8_t {
    Composition,
    Selection,
    KeyObject,
};

// Distribution needs at least this many layers (the key included).
inline constexpr std::size_t kMinimumDistributedLayers = 3;

// An axis-aligned composition-space rectangle in pixel-edge coordinates.
struct AlignmentRect final {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;

    [[nodiscard]] bool empty() const noexcept { return right <= left || bottom <= top; }
    [[nodiscard]] double centreX() const noexcept { return (left + right) / 2.0; }
    [[nodiscard]] double centreY() const noexcept { return (top + bottom) / 2.0; }
    friend bool operator==(const AlignmentRect&, const AlignmentRect&) = default;
};

// A 2 x 2 linear map in column-vector form: (x, y) -> (xx * x + yx * y, xy * x + yy * y).
struct AlignmentLinear final {
    double xx = 1.0;
    double xy = 0.0;
    double yx = 0.0;
    double yy = 1.0;
    friend bool operator==(const AlignmentLinear&, const AlignmentLinear&) = default;
};

// One layer as evaluated at the alignment time.
struct LayerAlignmentGeometry final {
    document::LayerId layer;
    // The evaluated output bounds (runtime::EvaluatedOperationBounds::output).
    AlignmentRect bounds;
    // The linear part of the parent's composition-space world transform; identity without a
    // parent. A composition-space delta d moves Position by parentLinear^-1 * d.
    AlignmentLinear parentLinear{};
    // Position at the alignment time in the layer's own key time, sampled when it is animated;
    // nullopt when it could not be sampled. A constant Position is read from the document instead.
    std::optional<document::Vec2d> position{};
    friend bool operator==(const LayerAlignmentGeometry&, const LayerAlignmentGeometry&) = default;
};

struct LayerAlignmentRequest final {
    document::CompositionId composition;
    // Composition time. Each layer keys at its own layer time for it (ADR 0024).
    core::RationalTime time;
    // The layers to move, in selection order. The key layer may be among them; it never moves.
    std::vector<document::LayerId> layers;
    LayerArrangement arrangement = LayerArrangement::AlignLeft;
    AlignmentTarget target = AlignmentTarget::Selection;
    // Required by AlignmentTarget::KeyObject, ignored otherwise.
    std::optional<document::LayerId> keyLayer;
    // At least every requested layer (and the key) that has visible bounds at the time.
    std::vector<LayerAlignmentGeometry> geometry;
};

enum class LayerAlignmentSkipReason : std::uint8_t {
    Locked,
    // Position is bound to a driver (a value-graph output), so there is no value to set.
    DrivenPosition,
    // The layer has no visible bounds at the time (outside its range, disabled or empty).
    NoBounds,
    // The layer has no Vec2 Position this operation can edit.
    NoPosition,
    // The parent's transform is singular or not finite, so no parent-space delta exists.
    SingularParent,
};

struct LayerAlignmentSkip final {
    document::LayerId layer;
    LayerAlignmentSkipReason reason = LayerAlignmentSkipReason::Locked;
    std::string message;
    friend bool operator==(const LayerAlignmentSkip&, const LayerAlignmentSkip&) = default;
};

struct LayerAlignmentMove final {
    document::LayerId layer;
    document::ParameterId position;
    // The composition-space delta this layer's own Position carries (after what a moving ancestor
    // already contributes) and the same delta in parent space.
    document::Vec2d compositionDelta{};
    document::Vec2d parentDelta{};
    // The value written: the constant, or the key at `keyTime`.
    document::Vec2d value{};
    bool keyed = false;
    core::RationalTime keyTime{};
    friend bool operator==(const LayerAlignmentMove&, const LayerAlignmentMove&) = default;
};

struct LayerAlignmentPlan final {
    // Non-empty when the request is refused as a whole; nothing changes.
    std::vector<std::string> rejections;
    // Layers that move, in request order. A layer that is already in place is not listed.
    std::vector<LayerAlignmentMove> moves;
    // Layers that cannot move, with their reasons, in request order.
    std::vector<LayerAlignmentSkip> skips;

    [[nodiscard]] bool rejected() const noexcept { return !rejections.empty(); }
};

// The whole decision, without changing anything: the target, each layer's delta, its parent-space
// conversion, the value or key it gets, and every skip. AlignLayers applies exactly this plan; the
// UI may call it to explain why an action would do nothing.
[[nodiscard]] LayerAlignmentPlan planLayerAlignment(const document::Composition& composition,
                                                    const LayerAlignmentRequest& request);

// The stored key time of a layer's own parameter at a composition time: (t - start) * speed. The
// identity timing returns t; nullopt only on 64-bit overflow.
[[nodiscard]] std::optional<core::RationalTime>
layerKeyTimeOfCompositionTime(const document::LayerTiming& timing, core::RationalTime time);

[[nodiscard]] std::string_view layerArrangementId(LayerArrangement arrangement) noexcept;
[[nodiscard]] std::optional<LayerArrangement> layerArrangementFromId(std::string_view id) noexcept;
[[nodiscard]] std::string_view alignmentTargetId(AlignmentTarget target) noexcept;
[[nodiscard]] std::optional<AlignmentTarget> alignmentTargetFromId(std::string_view id) noexcept;

// Outputs of an applied AlignLayers, each a LayerId.
inline constexpr std::string_view kAlignLayersMovedLayerOutput = "movedLayer";
inline constexpr std::string_view kAlignLayersSkippedLayerOutput = "skippedLayer";

class AlignLayers final : public Operation {
  public:
    explicit AlignLayers(LayerAlignmentRequest request) : request_(std::move(request)) {}

    // "bloom.layer.align-layers" for an alignment, "bloom.layer.distribute-layers" for a
    // distribution.
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId>
    parameterEditScope() const noexcept override {
        return request_.composition;
    }
    [[nodiscard]] const LayerAlignmentRequest& request() const noexcept { return request_; }

  private:
    LayerAlignmentRequest request_;
};

} // namespace bloom::commands
