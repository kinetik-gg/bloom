#include <bloom/commands/result.hpp>

#include <algorithm>
#include <utility>

namespace bloom::commands {

std::optional<AffectedTimeFootprint>
normalizeAffectedTimeFootprint(AffectedTimeFootprint footprint) {
    auto& intervals = footprint.intervals;
    std::ranges::sort(intervals, {}, &AffectedTimeRange::start);
    std::vector<AffectedTimeRange> merged;
    merged.reserve(intervals.size());
    for (const auto& interval : intervals) {
        // A malformed (inverted or empty) interval is not provable, so the whole footprint is
        // abandoned rather than silently treated as empty.
        if (interval.start >= interval.end)
            return std::nullopt;
        if (!merged.empty() && interval.start <= merged.back().end) {
            // Overlapping OR exactly adjacent: one contiguous changed region.
            merged.back().end = std::max(merged.back().end, interval.end);
            continue;
        }
        merged.push_back(interval);
        if (merged.size() > kMaxAffectedTimeIntervals)
            return std::nullopt;
    }
    footprint.intervals = std::move(merged);
    return footprint;
}

std::optional<AffectedTimeFootprint>
mergeAffectedTimeFootprints(const std::optional<AffectedTimeFootprint>& accumulated,
                            const AffectedTimeFootprint& next) {
    if (!accumulated.has_value())
        return normalizeAffectedTimeFootprint(next);
    if (accumulated->compositionId != next.compositionId)
        return std::nullopt;
    auto combined = *accumulated;
    combined.intervals.insert(combined.intervals.end(), next.intervals.begin(),
                              next.intervals.end());
    return normalizeAffectedTimeFootprint(std::move(combined));
}

std::optional<std::vector<LayerIdentityRemap>>
normalizeLayerIdentityRemaps(std::vector<LayerIdentityRemap> remaps) {
    if (remaps.size() > kMaxLayerIdentityRemaps)
        return std::nullopt;
    if (remaps.empty())
        return remaps;
    const auto compositionId = remaps.front().compositionId;
    if (!compositionId.isValid())
        return std::nullopt;
    for (const auto& remap : remaps) {
        if (remap.compositionId != compositionId || remap.start < core::RationalTime{} ||
            remap.start >= remap.end || !remap.beforeLayerId.isValid() ||
            !remap.afterLayerId.isValid() || remap.beforeLayerId == remap.afterLayerId ||
            !remap.beforeNodeId.isValid() || !remap.afterNodeId.isValid() ||
            remap.beforeNodeId == remap.afterNodeId) {
            return std::nullopt;
        }
    }
    // Order is preserved: undo inversion depends on it.
    return remaps;
}

std::vector<LayerIdentityRemap>
invertLayerIdentityRemaps(const std::optional<std::vector<LayerIdentityRemap>>& remaps) {
    std::vector<LayerIdentityRemap> inverted;
    if (!remaps.has_value())
        return inverted;
    inverted.reserve(remaps->size());
    for (auto remap = remaps->rbegin(); remap != remaps->rend(); ++remap) {
        auto copy = *remap;
        std::swap(copy.beforeLayerId, copy.afterLayerId);
        std::swap(copy.beforeNodeId, copy.afterNodeId);
        inverted.push_back(copy);
    }
    return inverted;
}

OperationResult OperationResult::applied(std::vector<OperationOutput> outputs) {
    return {
        .status = OperationStatus::Applied,
        .issues = {},
        .outputs = std::move(outputs),
        .affectedTimes = std::nullopt,
        .layerIdentityRemaps = std::nullopt,
    };
}

OperationResult OperationResult::noChange(std::vector<OperationOutput> outputs) {
    return {
        .status = OperationStatus::NoChange,
        .issues = {},
        .outputs = std::move(outputs),
        .affectedTimes = std::nullopt,
        .layerIdentityRemaps = std::nullopt,
    };
}

OperationResult OperationResult::rejected(OperationIssueCode code, std::string message) {
    return {
        .status = OperationStatus::Rejected,
        .issues = {{code, std::move(message)}},
        .outputs = {},
        .affectedTimes = std::nullopt,
        .layerIdentityRemaps = std::nullopt,
    };
}

} // namespace bloom::commands
