#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/layer_alignment_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bloom::commands {
namespace {

using document::LayerId;
using document::Vec2d;

// Deltas below this many composition pixels are already in place; floating-point noise from the
// parent-space round trip never authors a change.
constexpr double kInPlace = 1e-9;

[[nodiscard]] std::optional<std::int64_t> multiplied(const std::int64_t a, const std::int64_t b) {
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    if (a == std::numeric_limits<std::int64_t>::min() ||
        b == std::numeric_limits<std::int64_t>::min())
        return std::nullopt;
    if (a != 0 && (b > limit / (a < 0 ? -a : a) || b < -(limit / (a < 0 ? -a : a))))
        return std::nullopt;
    return a * b;
}

[[nodiscard]] bool finite(const AlignmentRect& rect) noexcept {
    return std::isfinite(rect.left) && std::isfinite(rect.top) && std::isfinite(rect.right) &&
           std::isfinite(rect.bottom);
}

[[nodiscard]] bool finite(const Vec2d value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] std::string layerName(const document::Composition& composition, const LayerId id) {
    const auto* layer = composition.graph().findLayer(id);
    const auto number = std::to_string(id.value());
    return layer == nullptr || layer->name.empty() ? "Layer " + number
                                                   : "'" + layer->name + "' (" + number + ")";
}

[[nodiscard]] std::optional<document::ParameterId>
positionParameter(const document::Composition& composition,
                  const document::LayerOutputBoundary& layer) {
    const auto* node = composition.graph().findNode(layer.nodeId);
    if (node == nullptr)
        return std::nullopt;
    const auto binding = std::ranges::find(node->parameters, document::kPositionParameterRole,
                                           &document::ParameterBinding::role);
    return binding == node->parameters.end() ? std::nullopt : std::optional(binding->parameterId);
}

// parentLinear^-1 * delta, or nullopt when the parent transform has no finite inverse.
[[nodiscard]] std::optional<Vec2d> toParentSpace(const AlignmentLinear& parent, const Vec2d delta) {
    const double determinant = parent.xx * parent.yy - parent.yx * parent.xy;
    if (!std::isfinite(determinant) || determinant == 0.0)
        return std::nullopt;
    const Vec2d result{(parent.yy * delta.x - parent.yx * delta.y) / determinant,
                       (-parent.xy * delta.x + parent.xx * delta.y) / determinant};
    return finite(result) ? std::optional(result) : std::nullopt;
}

[[nodiscard]] bool horizontal(const LayerArrangement arrangement) noexcept {
    switch (arrangement) {
    case LayerArrangement::AlignLeft:
    case LayerArrangement::AlignHorizontalCentre:
    case LayerArrangement::AlignRight:
    case LayerArrangement::DistributeHorizontalCentres:
        return true;
    default:
        return false;
    }
}

// The one-axis composition-space delta an alignment gives a layer with these bounds.
[[nodiscard]] double alignmentDelta(const LayerArrangement arrangement, const AlignmentRect& target,
                                    const AlignmentRect& bounds) noexcept {
    switch (arrangement) {
    case LayerArrangement::AlignLeft:
        return target.left - bounds.left;
    case LayerArrangement::AlignHorizontalCentre:
        return target.centreX() - bounds.centreX();
    case LayerArrangement::AlignRight:
        return target.right - bounds.right;
    case LayerArrangement::AlignTop:
        return target.top - bounds.top;
    case LayerArrangement::AlignVerticalMiddle:
        return target.centreY() - bounds.centreY();
    case LayerArrangement::AlignBottom:
        return target.bottom - bounds.bottom;
    case LayerArrangement::DistributeHorizontalCentres:
    case LayerArrangement::DistributeVerticalCentres:
        break;
    }
    return 0.0;
}

struct Member final {
    LayerId layer;
    AlignmentRect bounds;
    bool fixed = false;
};

// Target centres for a distribution, indexed like `members` (which this sorts by centre, request
// order breaking ties). The outermost layers and, for Key Object, the key are fixed; the layers
// between two fixed layers are spaced evenly.
[[nodiscard]] std::unordered_map<std::uint64_t, double>
distributedCentres(std::vector<Member> members, const bool alongX, const AlignmentTarget target,
                   const AlignmentRect& frame) {
    const auto centre = [alongX](const AlignmentRect& rect) {
        return alongX ? rect.centreX() : rect.centreY();
    };
    std::ranges::stable_sort(members, [&](const Member& a, const Member& b) {
        return centre(a.bounds) < centre(b.bounds);
    });
    std::vector<double> centres(members.size());
    for (std::size_t index = 0; index < members.size(); ++index)
        centres[index] = centre(members[index].bounds);
    if (target == AlignmentTarget::Composition) {
        const auto& first = members.front().bounds;
        const auto& last = members.back().bounds;
        const double firstHalf = (alongX ? first.right - first.left : first.bottom - first.top) / 2;
        const double lastHalf = (alongX ? last.right - last.left : last.bottom - last.top) / 2;
        centres.front() = (alongX ? frame.left : frame.top) + firstHalf;
        centres.back() = (alongX ? frame.right : frame.bottom) - lastHalf;
    }
    members.front().fixed = true;
    members.back().fixed = true;
    std::size_t anchor = 0;
    for (std::size_t index = 1; index < members.size(); ++index) {
        if (!members[index].fixed)
            continue;
        const auto span = static_cast<double>(index - anchor);
        for (std::size_t inner = anchor + 1; inner < index; ++inner)
            centres[inner] = centres[anchor] + (centres[index] - centres[anchor]) *
                                                   static_cast<double>(inner - anchor) / span;
        anchor = index;
    }
    std::unordered_map<std::uint64_t, double> result;
    for (std::size_t index = 0; index < members.size(); ++index)
        result.emplace(members[index].layer.value(), centres[index]);
    return result;
}

} // namespace

std::optional<core::RationalTime> layerKeyTimeOfCompositionTime(const document::LayerTiming& timing,
                                                                const core::RationalTime time) {
    const auto& start = timing.startTime;
    if (start == core::RationalTime{} && timing.speed.isIdentity())
        return time;
    // tau = (t - start) * n / d, every product checked.
    const auto left = multiplied(time.numerator(), start.denominator());
    const auto right = multiplied(start.numerator(), time.denominator());
    const auto denominator = multiplied(time.denominator(), start.denominator());
    if (!left || !right || !denominator)
        return std::nullopt;
    if ((*right > 0 && *left < std::numeric_limits<std::int64_t>::min() + *right) ||
        (*right < 0 && *left > std::numeric_limits<std::int64_t>::max() + *right))
        return std::nullopt;
    const auto numerator = multiplied(*left - *right, timing.speed.numerator());
    const auto scaled = multiplied(*denominator, timing.speed.denominator());
    if (!numerator || !scaled)
        return std::nullopt;
    return core::RationalTime::create(*numerator, *scaled);
}

std::string_view layerArrangementId(const LayerArrangement arrangement) noexcept {
    switch (arrangement) {
    case LayerArrangement::AlignLeft:
        return "left";
    case LayerArrangement::AlignHorizontalCentre:
        return "horizontal-centre";
    case LayerArrangement::AlignRight:
        return "right";
    case LayerArrangement::AlignTop:
        return "top";
    case LayerArrangement::AlignVerticalMiddle:
        return "vertical-middle";
    case LayerArrangement::AlignBottom:
        return "bottom";
    case LayerArrangement::DistributeHorizontalCentres:
        return "horizontal-centres";
    case LayerArrangement::DistributeVerticalCentres:
        return "vertical-centres";
    }
    return {};
}

std::optional<LayerArrangement> layerArrangementFromId(const std::string_view id) noexcept {
    for (const auto arrangement :
         {LayerArrangement::AlignLeft, LayerArrangement::AlignHorizontalCentre,
          LayerArrangement::AlignRight, LayerArrangement::AlignTop,
          LayerArrangement::AlignVerticalMiddle, LayerArrangement::AlignBottom,
          LayerArrangement::DistributeHorizontalCentres,
          LayerArrangement::DistributeVerticalCentres})
        if (layerArrangementId(arrangement) == id)
            return arrangement;
    return std::nullopt;
}

std::string_view alignmentTargetId(const AlignmentTarget target) noexcept {
    switch (target) {
    case AlignmentTarget::Composition:
        return "composition";
    case AlignmentTarget::Selection:
        return "selection";
    case AlignmentTarget::KeyObject:
        return "key-object";
    }
    return {};
}

std::optional<AlignmentTarget> alignmentTargetFromId(const std::string_view id) noexcept {
    for (const auto target :
         {AlignmentTarget::Composition, AlignmentTarget::Selection, AlignmentTarget::KeyObject})
        if (alignmentTargetId(target) == id)
            return target;
    return std::nullopt;
}

LayerAlignmentPlan planLayerAlignment(const document::Composition& composition,
                                      const LayerAlignmentRequest& request) {
    LayerAlignmentPlan plan;
    const auto reject = [&plan](std::string message) {
        plan.rejections.push_back(std::move(message));
        plan.moves.clear();
        return plan;
    };
    if (composition.id() != request.composition)
        return reject("The alignment names a different composition");
    if (request.layers.empty())
        return reject("Select at least one layer to align");

    // Requested layers, in order, without duplicates; each one must belong to this composition.
    std::vector<LayerId> requested;
    std::unordered_set<std::uint64_t> seen;
    for (const auto id : request.layers) {
        if (composition.graph().findLayer(id) == nullptr)
            return reject("Layer " + std::to_string(id.value()) + " is not in this composition");
        if (seen.insert(id.value()).second)
            requested.push_back(id);
    }
    const bool keyTarget = request.target == AlignmentTarget::KeyObject;
    if (keyTarget && !request.keyLayer.has_value())
        return reject("Align to Key Object needs a key layer");
    if (keyTarget && composition.graph().findLayer(*request.keyLayer) == nullptr)
        return reject("The key layer is not in this composition");

    std::unordered_map<std::uint64_t, const LayerAlignmentGeometry*> geometry;
    for (const auto& entry : request.geometry) {
        if (!finite(entry.bounds) || entry.bounds.empty())
            return reject("The evaluated bounds of " + layerName(composition, entry.layer) +
                          " are not a finite, non-empty rectangle");
        if (!geometry.emplace(entry.layer.value(), &entry).second)
            return reject("The evaluated bounds name " + layerName(composition, entry.layer) +
                          " twice");
    }
    const auto boundsOf = [&geometry](const LayerId id) -> const LayerAlignmentGeometry* {
        const auto found = geometry.find(id.value());
        return found == geometry.end() ? nullptr : found->second;
    };
    const LayerAlignmentGeometry* key = keyTarget ? boundsOf(*request.keyLayer) : nullptr;
    if (keyTarget && key == nullptr)
        return reject("The key layer " + layerName(composition, *request.keyLayer) +
                      " has no visible bounds at this time");

    // Every layer the arrangement places: the requested ones with bounds, and the key.
    std::vector<Member> members;
    for (std::size_t index = 0; index < requested.size(); ++index)
        if (const auto* entry = boundsOf(requested[index]))
            members.push_back({requested[index], entry->bounds, false});
    if (keyTarget && !seen.contains(request.keyLayer->value()))
        members.push_back({*request.keyLayer, key->bounds, false});

    const bool distribution = isDistribution(request.arrangement);
    if (distribution) {
        std::size_t count = requested.size();
        if (keyTarget && !seen.contains(request.keyLayer->value()))
            ++count;
        if (count < kMinimumDistributedLayers)
            return reject("Distribute needs at least " + std::to_string(kMinimumDistributedLayers) +
                          " layers");
    }

    const auto format = composition.format();
    const AlignmentRect frame{0.0, 0.0, static_cast<double>(format.width()),
                              static_cast<double>(format.height())};
    AlignmentRect target = frame;
    if (request.target == AlignmentTarget::Selection && !members.empty()) {
        target = members.front().bounds;
        for (const auto& member : members) {
            target.left = std::min(target.left, member.bounds.left);
            target.top = std::min(target.top, member.bounds.top);
            target.right = std::max(target.right, member.bounds.right);
            target.bottom = std::max(target.bottom, member.bounds.bottom);
        }
    } else if (keyTarget) {
        target = key->bounds;
    }

    // The whole composition-space delta each placed layer needs.
    const bool alongX = horizontal(request.arrangement);
    std::unordered_map<std::uint64_t, double> centres;
    if (distribution && members.size() >= kMinimumDistributedLayers) {
        if (keyTarget)
            for (auto& member : members)
                member.fixed = member.layer == *request.keyLayer;
        centres = distributedCentres(members, alongX, request.target, frame);
    }
    const auto wanted = [&](const Member& member) -> Vec2d {
        double delta = 0.0;
        if (distribution) {
            const auto found = centres.find(member.layer.value());
            if (found != centres.end())
                delta =
                    found->second - (alongX ? member.bounds.centreX() : member.bounds.centreY());
        } else {
            delta = alignmentDelta(request.arrangement, target, member.bounds);
        }
        return alongX ? Vec2d{delta, 0.0} : Vec2d{0.0, delta};
    };

    // Decide who moves. Skips are final before any delta is computed, so a skipped ancestor is
    // known not to carry its children.
    struct Mover final {
        const Member* member = nullptr;
        document::ParameterId position;
        const document::ParameterRecord* record = nullptr;
    };
    std::unordered_map<std::uint64_t, Mover> movers;
    std::vector<LayerId> moverOrder;
    const auto skip = [&](const LayerId id, const LayerAlignmentSkipReason reason,
                          std::string why) {
        plan.skips.push_back({id, reason, layerName(composition, id) + " was not moved: " + why});
    };
    std::size_t movable = 0;
    for (const auto id : requested) {
        if (keyTarget && id == *request.keyLayer)
            continue;
        ++movable;
        const auto* layer = composition.graph().findLayer(id);
        const auto member = std::ranges::find(members, id, &Member::layer);
        const auto position = positionParameter(composition, *layer);
        const auto* record = position ? composition.parameters().find(*position) : nullptr;
        const auto* constant =
            record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
        const auto* entry = boundsOf(id);
        if (layer->locked || (position && composition.parameterLocked(*position))) {
            skip(id, LayerAlignmentSkipReason::Locked, "it is locked");
        } else if (member == members.end()) {
            skip(id, LayerAlignmentSkipReason::NoBounds, "it has no visible bounds at this time");
        } else if (record == nullptr) {
            skip(id, LayerAlignmentSkipReason::NoPosition, "it has no Position to edit");
        } else if (std::holds_alternative<document::DriverBindingSource>(record->source)) {
            skip(id, LayerAlignmentSkipReason::DrivenPosition,
                 "its Position is driven; disconnect the driver to move it");
        } else if (constant && !std::holds_alternative<Vec2d>(constant->value)) {
            skip(id, LayerAlignmentSkipReason::NoPosition, "its Position is not a 2D value");
        } else if (!constant && (!entry->position || !finite(*entry->position) ||
                                 !layerKeyTimeOfCompositionTime(layer->timing, request.time))) {
            skip(id, LayerAlignmentSkipReason::NoPosition,
                 "its Position has no value at this time");
        } else if (layer->parent && !toParentSpace(entry->parentLinear, Vec2d{1.0, 0.0})) {
            skip(id, LayerAlignmentSkipReason::SingularParent,
                 "its parent's transform cannot be inverted at this time");
        } else {
            movers.emplace(id.value(), Mover{&*member, *position, record});
            moverOrder.push_back(id);
        }
    }
    if (movable == 0)
        return reject("Select at least one layer besides the key layer");
    if (plan.skips.size() == movable) {
        plan.rejections.push_back("No selected layer can move");
        for (const auto& skipped : plan.skips)
            plan.rejections.push_back(skipped.message);
        return plan;
    }

    // A moving ancestor carries its descendants rigidly by its own whole delta, so each layer's
    // Position takes only what its nearest moving ancestor has not already supplied.
    std::unordered_map<std::uint64_t, Vec2d> motion;
    const auto totalMotion = [&](const auto& self, const LayerId id,
                                 const std::size_t depth) -> Vec2d {
        if (const auto found = motion.find(id.value()); found != motion.end())
            return found->second;
        Vec2d result{};
        if (const auto mover = movers.find(id.value()); mover != movers.end()) {
            result = wanted(*mover->second.member);
        } else if (const auto* layer = composition.graph().findLayer(id);
                   layer && layer->parent && depth < composition.graph().layerOutputs().size()) {
            result = self(self, *layer->parent, depth + 1);
        }
        motion.emplace(id.value(), result);
        return result;
    };

    for (const auto id : moverOrder) {
        const auto& mover = movers.at(id.value());
        const auto* layer = composition.graph().findLayer(id);
        const auto* entry = boundsOf(id);
        const auto whole = totalMotion(totalMotion, id, 0);
        const auto inherited =
            layer->parent ? totalMotion(totalMotion, *layer->parent, 1) : Vec2d{};
        const Vec2d own{whole.x - inherited.x, whole.y - inherited.y};
        if (std::abs(own.x) <= kInPlace && std::abs(own.y) <= kInPlace)
            continue;
        const auto parentDelta =
            layer->parent ? toParentSpace(entry->parentLinear, own) : std::optional(own);
        if (!parentDelta)
            return reject("The move of " + layerName(composition, id) +
                          " has no finite parent-space value");
        LayerAlignmentMove move{.layer = id,
                                .position = mover.position,
                                .compositionDelta = own,
                                .parentDelta = *parentDelta};
        if (const auto* constant =
                std::get_if<document::ConstantValueSource>(&mover.record->source)) {
            const auto value = std::get<Vec2d>(constant->value);
            move.value = {value.x + parentDelta->x, value.y + parentDelta->y};
        } else {
            move.keyed = true;
            move.keyTime = *layerKeyTimeOfCompositionTime(layer->timing, request.time);
            move.value = {entry->position->x + parentDelta->x, entry->position->y + parentDelta->y};
        }
        if (!finite(move.value))
            return reject("The new Position of " + layerName(composition, id) +
                          " would not be a finite value");
        plan.moves.push_back(move);
    }
    return plan;
}

std::string_view AlignLayers::typeId() const noexcept {
    return isDistribution(request_.arrangement) ? "bloom.layer.distribute-layers"
                                                : "bloom.layer.align-layers";
}

OperationResult AlignLayers::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(request_.composition);
    if (composition == nullptr)
        return OperationResult::rejected(
            OperationIssueCode::InvalidTarget,
            "Composition " + std::to_string(request_.composition.value()) + " does not exist");
    const auto plan = planLayerAlignment(*composition, request_);
    if (plan.rejected()) {
        auto result =
            OperationResult::rejected(OperationIssueCode::InvalidTarget, plan.rejections.front());
        for (std::size_t index = 1; index < plan.rejections.size(); ++index)
            result.issues.push_back({OperationIssueCode::InvalidTarget, plan.rejections[index]});
        return result;
    }
    std::vector<OperationOutput> outputs;
    bool changed = false;
    for (const auto& move : plan.moves) {
        const auto applied =
            move.keyed ? SetKeyframeAtTimeForParameter(request_.composition, move.position,
                                                       move.keyTime, move.value)
                             .apply(draft)
                       : SetParameterSource(request_.composition, move.position,
                                            document::ConstantValueSource{move.value})
                             .apply(draft);
        if (applied.status == OperationStatus::Rejected)
            return applied;
        changed = changed || applied.status == OperationStatus::Applied;
        outputs.push_back({std::string(kAlignLayersMovedLayerOutput), move.layer});
    }
    for (const auto& skipped : plan.skips)
        outputs.push_back({std::string(kAlignLayersSkippedLayerOutput), skipped.layer});
    auto result = changed ? OperationResult::applied(std::move(outputs))
                          : OperationResult::noChange(std::move(outputs));
    for (const auto& skipped : plan.skips)
        result.issues.push_back({OperationIssueCode::InvalidTarget, skipped.message});
    return result;
}

} // namespace bloom::commands
