#include "node_operation_support.hpp"

#include <bloom/commands/layer_time_operations.hpp>

#include <algorithm>
#include <limits>
#include <vector>

namespace bloom::commands {
namespace {

using core::RationalTime;

// Portable checked product (no compiler builtins: MSVC builds this too).
[[nodiscard]] std::optional<std::int64_t> multiplied(const std::int64_t a, const std::int64_t b) {
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    if (a == std::numeric_limits<std::int64_t>::min() ||
        b == std::numeric_limits<std::int64_t>::min())
        return std::nullopt;
    if (a != 0 && (b > limit / (a < 0 ? -a : a) || b < -(limit / (a < 0 ? -a : a))))
        return std::nullopt;
    return a * b;
}

// Nearest frame start on the composition grid, unbounded (times outside the composition snap on
// the same grid); an exact half rounds to the later frame, as scrubbing does.
[[nodiscard]] std::optional<RationalTime> snapToFrame(const RationalTime time,
                                                      const document::FrameRate rate) {
    // frames = time * rate = (p * num) / (q * den)
    const auto numerator = multiplied(time.numerator(), rate.numerator());
    const auto denominator = multiplied(time.denominator(), rate.denominator());
    if (!numerator || !denominator || *denominator <= 0)
        return std::nullopt;
    const auto twice = multiplied(*numerator, 2);
    const auto doubleDenominator = multiplied(*denominator, 2);
    if (!twice || !doubleDenominator ||
        *twice > std::numeric_limits<std::int64_t>::max() - *denominator)
        return std::nullopt;
    const auto shifted = *twice + *denominator;
    auto frames = shifted / *doubleDenominator;
    if (shifted % *doubleDenominator != 0 && shifted < 0)
        --frames; // floor division
    const auto frameNumerator = multiplied(frames, rate.denominator());
    if (!frameNumerator)
        return std::nullopt;
    return RationalTime::create(*frameNumerator, rate.numerator());
}

[[nodiscard]] OperationResult invalidTiming(std::string message) {
    return OperationResult::rejected(OperationIssueCode::InvalidValue, std::move(message));
}

// The changed-time footprint of replacing span `before` by span `after`, clipped to the
// composition. When the content mapping is unchanged (a trim) only the symmetric difference
// changes; otherwise every time either span covers may show different pixels.
[[nodiscard]] std::optional<AffectedTimeFootprint>
footprint(const document::CompositionId composition, const AffectedTimeRange before,
          const AffectedTimeRange after, const bool mappingChanged, const RationalTime duration) {
    std::vector<AffectedTimeRange> changed;
    const auto add = [&](RationalTime start, RationalTime end) {
        start = std::max(start, RationalTime{});
        end = std::min(end, duration);
        if (start < end)
            changed.push_back({start, end});
    };
    if (mappingChanged) {
        add(before.start, before.end);
        add(after.start, after.end);
    } else {
        const auto difference = [&](const AffectedTimeRange& a, const AffectedTimeRange& b) {
            if (a.end <= b.start || b.end <= a.start) {
                add(a.start, a.end);
                return;
            }
            if (a.start < b.start)
                add(a.start, std::min(a.end, b.start));
            if (b.end < a.end)
                add(std::max(a.start, b.end), a.end);
        };
        difference(before, after);
        difference(after, before);
    }
    return normalizeAffectedTimeFootprint({composition, std::move(changed)});
}

struct Target final {
    document::Composition* composition = nullptr;
    document::LayerOutputBoundary* layer = nullptr;
};

[[nodiscard]] std::optional<OperationResult> findTarget(document::Draft& draft,
                                                        const document::CompositionId compositionId,
                                                        const document::LayerId layerId,
                                                        Target& target) {
    target.composition = draft.project().findComposition(compositionId);
    target.layer = target.composition ? target.composition->graph().findLayer(layerId) : nullptr;
    if (target.layer == nullptr)
        return detail::invalidTarget();
    if (target.layer->locked)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer is locked");
    return std::nullopt;
}

// The whole activity span change of a mapping-only edit (modes, extent).
[[nodiscard]] OperationResult spanResult(const document::CompositionId composition,
                                         const document::LayerOutputBoundary& layer,
                                         const RationalTime duration) {
    const AffectedTimeRange span{layer.inPoint, layer.endPoint(duration)};
    auto result = OperationResult::applied();
    result.affectedTimes = footprint(composition, span, span, true, duration);
    return result;
}

} // namespace

std::string_view SetLayerTiming::typeId() const noexcept { return "bloom.layer.set-timing"; }

OperationResult SetLayerTiming::apply(document::Draft& draft) const {
    Target target;
    if (auto failure = findTarget(draft, composition_, layer_, target))
        return std::move(*failure);
    auto& composition = *target.composition;
    auto& layer = *target.layer;
    const auto duration = composition.duration();
    const auto rate = composition.format().frameRate();
    const auto in = snapToFrame(in_, rate);
    // The exact composition end is a valid endpoint even when it is not a frame start.
    const auto out = out_ == duration ? std::optional{duration} : snapToFrame(out_, rate);
    if (!in || !out || *in >= *out)
        return invalidTiming("Layer range must start before it ends on composition frames");
    if (*in >= duration || *out <= RationalTime{})
        return invalidTiming("Layer range must overlap the composition");
    const auto before = AffectedTimeRange{layer.inPoint, layer.endPoint(duration)};
    const bool mappingChanged = layer.timing.startTime != startTime_ || layer.timing.speed != speed_;
    if (!mappingChanged && before.start == *in && before.end == *out)
        return OperationResult::noChange();
    layer.timing.startTime = startTime_;
    layer.timing.speed = speed_;
    layer.inPoint = *in;
    layer.outPoint = *out;
    auto result = OperationResult::applied();
    result.affectedTimes =
        footprint(composition_, before, {*in, *out}, mappingChanged, duration);
    return result;
}

std::string_view SetLayerTimeModes::typeId() const noexcept { return "bloom.layer.set-time-modes"; }

OperationResult SetLayerTimeModes::apply(document::Draft& draft) const {
    Target target;
    if (auto failure = findTarget(draft, composition_, layer_, target))
        return std::move(*failure);
    auto& timing = target.layer->timing;
    if (timing.beforeMode == before_ && timing.afterMode == after_)
        return OperationResult::noChange();
    timing.beforeMode = before_;
    timing.afterMode = after_;
    return spanResult(composition_, *target.layer, target.composition->duration());
}

std::string_view SetLayerContentExtent::typeId() const noexcept {
    return "bloom.layer.set-content-extent";
}

OperationResult SetLayerContentExtent::apply(document::Draft& draft) const {
    Target target;
    if (auto failure = findTarget(draft, composition_, layer_, target))
        return std::move(*failure);
    auto& timing = target.layer->timing;
    if (timing.contentExtent == extent_)
        return OperationResult::noChange();
    document::LayerTiming candidate = timing;
    candidate.contentExtent = extent_;
    if (!candidate.isValid())
        return invalidTiming("Layer content extent is invalid");
    if (const auto* source = std::get_if<document::SourceLayerExtent>(&extent_);
        source && target.composition->graph().findNode(source->sourceNodeId) == nullptr)
        return invalidTiming("Layer content extent source must be a node in the composition");
    timing.contentExtent = extent_;
    return spanResult(composition_, *target.layer, target.composition->duration());
}

} // namespace bloom::commands
