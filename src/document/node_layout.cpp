#include <bloom/document/node_layout.hpp>

#include <bloom/document/persisted_text.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace bloom::document {

namespace {
struct CardRect final {
    Vec2d position;
    double width = 0.0;
    double height = 0.0;
};

[[nodiscard]] double saneExtent(const double value, const double fallback) noexcept {
    return std::isfinite(value) && value > 0.0 ? value : fallback;
}

[[nodiscard]] CardRect cardRect(const NodeLayoutRecord& record, const NodeCardSize size) noexcept {
    return {record.position, saneExtent(size.width, saneExtent(record.width, 240.0)),
            saneExtent(size.height, kConservativeNodeCardSize.height)};
}

[[nodiscard]] bool overlapsWithGap(const CardRect& candidate, const CardRect& existing,
                                   const double gap) noexcept {
    const double candidateRight = candidate.position.x + candidate.width;
    const double candidateBottom = candidate.position.y + candidate.height;
    const double existingRight = existing.position.x + existing.width;
    const double existingBottom = existing.position.y + existing.height;
    return candidate.position.x < existingRight + gap &&
           candidateRight + gap > existing.position.x &&
           candidate.position.y < existingBottom + gap &&
           candidateBottom + gap > existing.position.y;
}

class SpatialIndex final {
  public:
    static constexpr double kBucketSize = 256.0;

    void insert(CardRect rect) {
        const auto index = rectangles_.size();
        rectangles_.push_back(rect);
        const auto firstX = bucket(rect.position.x);
        const auto lastX = bucket(rect.position.x + rect.width);
        const auto firstY = bucket(rect.position.y);
        const auto lastY = bucket(rect.position.y + rect.height);
        for (auto x = firstX; x <= lastX; ++x)
            for (auto y = firstY; y <= lastY; ++y)
                buckets_[{x, y}].push_back(index);
    }

    [[nodiscard]] bool clear(const CardRect& candidate, const double gap) const noexcept {
        const auto firstX = bucket(candidate.position.x - gap);
        const auto lastX = bucket(candidate.position.x + candidate.width + gap);
        const auto firstY = bucket(candidate.position.y - gap);
        const auto lastY = bucket(candidate.position.y + candidate.height + gap);
        for (auto x = firstX; x <= lastX; ++x)
            for (auto y = firstY; y <= lastY; ++y) {
                const auto found = buckets_.find({x, y});
                if (found == buckets_.end())
                    continue;
                for (const auto index : found->second)
                    if (overlapsWithGap(candidate, rectangles_[index], gap))
                        return false;
            }
        return true;
    }

  private:
    [[nodiscard]] static std::int64_t bucket(const double x) noexcept {
        const auto value = std::floor(x / kBucketSize);
        if (value <= static_cast<double>(std::numeric_limits<std::int64_t>::min()))
            return std::numeric_limits<std::int64_t>::min();
        if (value >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
            return std::numeric_limits<std::int64_t>::max();
        return static_cast<std::int64_t>(value);
    }

    std::vector<CardRect> rectangles_;
    std::map<std::pair<std::int64_t, std::int64_t>, std::vector<std::size_t>> buckets_;
};

[[nodiscard]] CardRect probeRect(const Vec2d position, const NodeCardSize size) noexcept {
    return {position, saneExtent(size.width, kConservativeNodeCardSize.width),
            saneExtent(size.height, kConservativeNodeCardSize.height)};
}

} // namespace

Vec2d findNearestFreeNodePosition(const NodeLayout& layout, const NodeCardSizes& sizes,
                                  NodeCardSize newSize, Vec2d preferred, const double gap) {
    const double safeGap = saneExtent(gap, kNodeLayoutSpacing);
    newSize.width = saneExtent(newSize.width, kConservativeNodeCardSize.width);
    newSize.height = saneExtent(newSize.height, kConservativeNodeCardSize.height);
    if (!std::isfinite(preferred.x) || !std::isfinite(preferred.y))
        preferred = {};

    SpatialIndex index;
    for (const auto& [id, record] : layout) {
        const auto found = sizes.find(id);
        const auto size =
            found == sizes.end()
                ? NodeCardSize{std::max(record.width, kConservativeNodeCardSize.width),
                               kConservativeNodeCardSize.height}
                : found->second;
        index.insert(cardRect(record, size));
    }

    const auto isFree = [&](const Vec2d position) {
        return index.clear(probeRect(position, newSize), safeGap);
    };
    if (isFree(preferred))
        return preferred;

    // A failed probe first walks the same column below and above the anchor. This is both pleasant
    // for layer creation (new rows grow downward) and deterministic for a dense row. Only after a
    // complete vertical pair is exhausted do we move to the next origin column.
    const double rowStep = newSize.height + safeGap;
    const double columnStep = newSize.width + safeGap;
    constexpr std::int64_t kMaximumProbe = 100000;
    for (std::int64_t distance = 1; distance <= kMaximumProbe; ++distance) {
        const double offset = static_cast<double>(distance) * rowStep;
        if (isFree({preferred.x, preferred.y + offset}))
            return {preferred.x, preferred.y + offset};
        if (isFree({preferred.x, preferred.y - offset}))
            return {preferred.x, preferred.y - offset};

        // Search the next columns only after the vertical candidates at this radius. The first
        // probe in each column is the aligned slot; its own down/up expansion follows on later
        // passes, keeping the search local without a quadratic walk over all cards.
        const double columnOffset = static_cast<double>(distance) * columnStep;
        if (isFree({preferred.x - columnOffset, preferred.y}))
            return {preferred.x - columnOffset, preferred.y};
        if (isFree({preferred.x + columnOffset, preferred.y}))
            return {preferred.x + columnOffset, preferred.y};
    }
    // A finite layout cannot fill this bounded search space in practice. Keeping the fallback
    // deterministic is preferable to returning NaNs if a hostile document is supplied.
    return preferred;
}

std::size_t defaultNodeLayoutColumn(const std::string_view typeId) noexcept {
    if (typeId == kLayerOutputNodeType)
        return 1;
    if (typeId == kLayerStackNodeType)
        return 2;
    if (typeId == kCompositionOutputNodeType)
        return 3;
    return 0;
}

NodeLayoutRecord defaultNodeLayoutRecord(const std::size_t column, const std::size_t row) noexcept {
    // The original four-column editor placement, frozen in document units for migration.
    return {{32.0 + static_cast<double>(column) * 256.0, 32.0 + static_cast<double>(row) * 180.0},
            128.0,
            false,
            false};
}

NodeLayout defaultNodeLayout(const std::span<const NodeRecord> nodes) {
    std::array<std::size_t, 4> rows{};
    NodeLayout layout;
    for (const auto& node : nodes) {
        const auto column = defaultNodeLayoutColumn(node.typeId);
        layout.emplace(node.id, defaultNodeLayoutRecord(column, rows.at(column)++));
    }
    return layout;
}

const NodeGroupRecord* findNodeGroupOf(const NodeGroups& groups, const NodeId node) {
    for (const auto& [id, group] : groups) {
        if (group.members.contains(node))
            return &group;
    }
    return nullptr;
}

ValidationResult validateNodeLayout(const NodeLayout& layout, const CanonicalGraph& graph) {
    ValidationResult result;
    for (const auto& [id, record] : layout) {
        const auto path = "nodeLayout[" + std::to_string(id.value()) + "]";
        if (graph.findNode(id) == nullptr) {
            result.add(ValidationCode::MissingReference, path,
                       "Layout entry references an unknown node", ValidationSeverity::Warning);
        }
        if (!std::isfinite(record.position.x) || !std::isfinite(record.position.y) ||
            !std::isfinite(record.width) || record.width <= 0.0) {
            result.add(ValidationCode::InvalidValue, path,
                       "Node layout requires a finite position and positive finite width");
        }
    }
    return result;
}

ValidationResult validateNodeGroups(const NodeGroups& groups, const CanonicalGraph& graph) {
    ValidationResult result;
    // Which group already claimed a node, so "a node is in at most one group" is reported against
    // the second claimant rather than discovered twice.
    std::map<NodeId, NodeGroupId> owners;
    for (const auto& [id, group] : groups) {
        const auto path = "nodeGroups[" + std::to_string(id.value()) + "]";
        if (!id.isValid() || group.id != id) {
            result.add(ValidationCode::InvalidId, path + ".id",
                       "Node group ID must be nonzero and match its own key");
        }
        validateHumanFacingName(group.name, path + ".name", "Node group name", result);
        if (!std::isfinite(group.padding.x) || !std::isfinite(group.padding.y) ||
            group.padding.x < 0.0 || group.padding.y < 0.0) {
            result.add(ValidationCode::InvalidValue, path + ".padding",
                       "Node group padding requires finite nonnegative components");
        }
        for (const auto member : group.members) {
            // A missing member is a warning, exactly as an unknown-node layout entry is: a group is
            // presentation, and an unreadable module's node must not cost the artist the document.
            if (graph.findNode(member) == nullptr) {
                result.add(ValidationCode::MissingReference,
                           path + ".members[" + std::to_string(member.value()) + "]",
                           "Node group member references an unknown node",
                           ValidationSeverity::Warning);
            }
            const auto [owner, inserted] = owners.try_emplace(member, id);
            if (!inserted) {
                result.add(ValidationCode::DuplicateId,
                           path + ".members[" + std::to_string(member.value()) + "]",
                           "Node is already a member of node group " +
                               std::to_string(owner->second.value()));
            }
        }
    }
    return result;
}

} // namespace bloom::document
