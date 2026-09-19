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

OperationResult OperationResult::applied(std::vector<OperationOutput> outputs) {
    return {
        .status = OperationStatus::Applied,
        .issues = {},
        .outputs = std::move(outputs),
        .affectedTimes = std::nullopt,
    };
}

OperationResult OperationResult::noChange(std::vector<OperationOutput> outputs) {
    return {
        .status = OperationStatus::NoChange,
        .issues = {},
        .outputs = std::move(outputs),
        .affectedTimes = std::nullopt,
    };
}

OperationResult OperationResult::rejected(OperationIssueCode code, std::string message) {
    return {
        .status = OperationStatus::Rejected,
        .issues = {{code, std::move(message)}},
        .outputs = {},
        .affectedTimes = std::nullopt,
    };
}

} // namespace bloom::commands
