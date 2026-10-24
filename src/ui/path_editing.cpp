#include <bloom/ui/path_editing.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

namespace bloom::ui::path_editing {
namespace {

using document::PathAnchor;
using document::PathAnchorMode;
using document::PathValue;
using document::Vec2d;

[[nodiscard]] Vec2d add(const Vec2d a, const Vec2d b) noexcept { return {a.x + b.x, a.y + b.y}; }
[[nodiscard]] Vec2d sub(const Vec2d a, const Vec2d b) noexcept { return {a.x - b.x, a.y - b.y}; }
[[nodiscard]] Vec2d scale(const Vec2d a, const double s) noexcept { return {a.x * s, a.y * s}; }
[[nodiscard]] double length(const Vec2d a) noexcept { return std::hypot(a.x, a.y); }
[[nodiscard]] Vec2d lerp(const Vec2d a, const Vec2d b, const double t) noexcept {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}
[[nodiscard]] Vec2d mirror(const Vec2d point, const Vec2d handle) noexcept {
    return sub(scale(point, 2.0), handle);
}

[[nodiscard]] std::optional<std::size_t> previous(const PathValue& path, const std::size_t index) {
    if (index > 0)
        return index - 1;
    if (path.closed && path.anchors.size() > 1)
        return path.anchors.size() - 1;
    return std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> next(const PathValue& path, const std::size_t index) {
    if (index + 1 < path.anchors.size())
        return index + 1;
    if (path.closed && path.anchors.size() > 1)
        return std::size_t{0};
    return std::nullopt;
}

// Gives a handle-less or one-handled anchor both handles: neighbour-sized symmetric ones when it
// has none, the mirror of the one it has otherwise.
void ensureHandles(PathValue& path, const std::size_t index) {
    auto& anchor = path.anchors[index];
    if (!anchor.inHandle && !anchor.outHandle) {
        if (const auto handles = autoHandles(path, index)) {
            anchor.inHandle = handles->first;
            anchor.outHandle = handles->second;
        }
    } else if (!anchor.inHandle) {
        anchor.inHandle = mirror(anchor.point, *anchor.outHandle);
    } else if (!anchor.outHandle) {
        anchor.outHandle = mirror(anchor.point, *anchor.inHandle);
    }
}

// The unit tangent through the two handles, or nullopt when they coincide.
[[nodiscard]] std::optional<Vec2d> tangent(const PathAnchor& anchor) {
    if (!anchor.inHandle || !anchor.outHandle)
        return std::nullopt;
    auto direction = sub(*anchor.outHandle, *anchor.inHandle);
    const auto size = length(direction);
    if (!(size > 0.0)) {
        direction = sub(*anchor.outHandle, anchor.point);
        const auto outward = length(direction);
        if (!(outward > 0.0))
            return std::nullopt;
        return scale(direction, 1.0 / outward);
    }
    return scale(direction, 1.0 / size);
}

} // namespace

PathAnchorMode modeOf(const PathAnchor& anchor) noexcept {
    if (anchor.mode == PathAnchorMode::Corner && (anchor.inHandle || anchor.outHandle))
        return document::inferPathAnchorMode(anchor);
    return anchor.mode;
}

std::optional<PathAnchorMode> commonMode(const PathValue& path, const AnchorSet& anchors) {
    std::optional<PathAnchorMode> common;
    for (const auto index : anchors) {
        if (index >= path.anchors.size())
            continue;
        const auto mode = modeOf(path.anchors[index]);
        if (common && *common != mode)
            return std::nullopt;
        common = mode;
    }
    return common;
}

std::optional<std::pair<Vec2d, Vec2d>> autoHandles(const PathValue& path, const std::size_t index) {
    if (index >= path.anchors.size())
        return std::nullopt;
    const auto point = path.anchors[index].point;
    const auto before = previous(path, index);
    const auto after = next(path, index);
    Vec2d step;
    if (before && after)
        step = scale(sub(path.anchors[*after].point, path.anchors[*before].point), 1.0 / 6.0);
    else if (after)
        step = scale(sub(path.anchors[*after].point, point), 1.0 / 3.0);
    else if (before)
        step = scale(sub(point, path.anchors[*before].point), 1.0 / 3.0);
    else
        return std::nullopt;
    return std::pair{sub(point, step), add(point, step)};
}

void refreshAuto(PathValue& path) {
    for (std::size_t index = 0; index < path.anchors.size(); ++index) {
        auto& anchor = path.anchors[index];
        if (anchor.mode != PathAnchorMode::Auto)
            continue;
        if (const auto handles = autoHandles(path, index)) {
            anchor.inHandle = handles->first;
            anchor.outHandle = handles->second;
        } else {
            anchor.inHandle.reset();
            anchor.outHandle.reset();
        }
    }
}

void setMode(PathValue& path, const AnchorSet& anchors, const PathAnchorMode mode) {
    for (const auto index : anchors) {
        if (index >= path.anchors.size())
            continue;
        auto& anchor = path.anchors[index];
        anchor.mode = mode;
        if (mode == PathAnchorMode::Corner) {
            anchor.inHandle.reset();
            anchor.outHandle.reset();
            continue;
        }
        if (mode == PathAnchorMode::Auto)
            continue;
        ensureHandles(path, index);
        const auto direction = tangent(anchor);
        if (!direction || mode == PathAnchorMode::Free)
            continue;
        const auto inLength = length(sub(*anchor.inHandle, anchor.point));
        const auto outLength = length(sub(*anchor.outHandle, anchor.point));
        if (mode == PathAnchorMode::Mirrored) {
            const auto shared = (inLength + outLength) / 2.0;
            anchor.inHandle = sub(anchor.point, scale(*direction, shared));
            anchor.outHandle = add(anchor.point, scale(*direction, shared));
        } else {
            anchor.inHandle = sub(anchor.point, scale(*direction, inLength));
            anchor.outHandle = add(anchor.point, scale(*direction, outLength));
        }
    }
    refreshAuto(path);
}

void moveAnchors(PathValue& path, const AnchorSet& anchors, const Vec2d delta) {
    for (const auto index : anchors) {
        if (index >= path.anchors.size())
            continue;
        auto& anchor = path.anchors[index];
        anchor.point = add(anchor.point, delta);
        if (anchor.inHandle)
            anchor.inHandle = add(*anchor.inHandle, delta);
        if (anchor.outHandle)
            anchor.outHandle = add(*anchor.outHandle, delta);
    }
    refreshAuto(path);
}

void dragHandle(PathValue& path, const std::size_t index, const HandleSide side,
                const Vec2d position, const bool free) {
    if (index >= path.anchors.size())
        return;
    auto& anchor = path.anchors[index];
    auto mode = modeOf(anchor);
    auto& handle = side == HandleSide::In ? anchor.inHandle : anchor.outHandle;
    auto& opposite = side == HandleSide::In ? anchor.outHandle : anchor.inHandle;
    handle = position;
    if (free) {
        anchor.mode = PathAnchorMode::Free;
        return;
    }
    if (mode == PathAnchorMode::Auto || mode == PathAnchorMode::Corner)
        mode = PathAnchorMode::Aligned;
    if (mode == PathAnchorMode::Mirrored) {
        opposite = mirror(anchor.point, position);
    } else if (mode == PathAnchorMode::Aligned && opposite) {
        const auto away = sub(anchor.point, position);
        const auto size = length(away);
        if (size > 0.0)
            opposite = add(anchor.point, scale(away, length(sub(*opposite, anchor.point)) / size));
    }
    anchor.mode = mode;
}

void pullHandles(PathValue& path, const std::size_t index, const Vec2d outgoing) {
    if (index >= path.anchors.size())
        return;
    auto& anchor = path.anchors[index];
    anchor.outHandle = outgoing;
    anchor.inHandle = mirror(anchor.point, outgoing);
    anchor.mode = PathAnchorMode::Mirrored;
}

void toggleCorner(PathValue& path, const std::size_t index, const PathAnchorMode curved) {
    if (index >= path.anchors.size())
        return;
    const bool corner = modeOf(path.anchors[index]) == PathAnchorMode::Corner;
    setMode(path, {index},
            corner ? (curved == PathAnchorMode::Corner ? PathAnchorMode::Mirrored : curved)
                   : PathAnchorMode::Corner);
}

std::size_t segmentCount(const PathValue& path) noexcept {
    if (path.anchors.size() < 2)
        return 0;
    return path.closed ? path.anchors.size() : path.anchors.size() - 1;
}

std::array<Vec2d, 4> segmentControls(const PathValue& path, const std::size_t segment) {
    const auto& a = path.anchors[segment];
    const auto& b = path.anchors[(segment + 1) % path.anchors.size()];
    return {a.point, a.outHandle.value_or(a.point), b.inHandle.value_or(b.point), b.point};
}

Vec2d segmentPoint(const std::array<Vec2d, 4>& controls, const double t) noexcept {
    const auto p01 = lerp(controls[0], controls[1], t);
    const auto p12 = lerp(controls[1], controls[2], t);
    const auto p23 = lerp(controls[2], controls[3], t);
    return lerp(lerp(p01, p12, t), lerp(p12, p23, t), t);
}

std::optional<SegmentHit> nearestSegmentPoint(const PathValue& path,
                                              const std::function<Vec2d(Vec2d)>& toScreen,
                                              const Vec2d target) {
    constexpr int kSamples = 32;
    std::optional<SegmentHit> best;
    const auto distanceAt = [&](const std::array<Vec2d, 4>& controls, const double t) {
        return length(sub(toScreen(segmentPoint(controls, t)), target));
    };
    for (std::size_t segment = 0; segment < segmentCount(path); ++segment) {
        const auto controls = segmentControls(path, segment);
        int bestStep = 0;
        double bestDistance = std::numeric_limits<double>::infinity();
        for (int step = 0; step <= kSamples; ++step) {
            const auto distance = distanceAt(controls, static_cast<double>(step) / kSamples);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestStep = step;
            }
        }
        // Golden-section refinement inside the neighbouring sample interval.
        double low = std::max(0.0, static_cast<double>(bestStep - 1) / kSamples);
        double high = std::min(1.0, static_cast<double>(bestStep + 1) / kSamples);
        constexpr double kRatio = 0.6180339887498949;
        for (int iteration = 0; iteration < 32; ++iteration) {
            const double left = high - (high - low) * kRatio;
            const double right = low + (high - low) * kRatio;
            if (distanceAt(controls, left) < distanceAt(controls, right))
                high = right;
            else
                low = left;
        }
        const double t = (low + high) / 2.0;
        const double distance = distanceAt(controls, t);
        if (!best || distance < best->distance)
            best = SegmentHit{segment, t, distance};
    }
    return best;
}

std::optional<std::size_t> insertAnchor(PathValue& path, const SegmentHit& hit) {
    if (hit.segment >= segmentCount(path) || path.anchors.size() >= document::kMaximumPathAnchors)
        return std::nullopt;
    const auto t = std::clamp(hit.t, 0.0, 1.0);
    const auto first = hit.segment;
    const auto second = (hit.segment + 1) % path.anchors.size();
    auto& a = path.anchors[first];
    auto& b = path.anchors[second];
    PathAnchor inserted;
    if (!a.outHandle && !b.inHandle) {
        inserted.point = lerp(a.point, b.point, t);
        inserted.mode = PathAnchorMode::Corner;
    } else {
        const auto controls = segmentControls(path, hit.segment);
        const auto p01 = lerp(controls[0], controls[1], t);
        const auto p12 = lerp(controls[1], controls[2], t);
        const auto p23 = lerp(controls[2], controls[3], t);
        const auto p012 = lerp(p01, p12, t);
        const auto p123 = lerp(p12, p23, t);
        inserted.point = lerp(p012, p123, t);
        inserted.inHandle = p012;
        inserted.outHandle = p123;
        inserted.mode = PathAnchorMode::Aligned;
        // The neighbours' handles shorten along their own direction: they stay collinear but no
        // longer symmetric, and an Auto neighbour must keep exactly this geometry.
        const auto settle = [](PathAnchor& anchor) {
            const auto mode = modeOf(anchor);
            if (mode == PathAnchorMode::Mirrored || mode == PathAnchorMode::Auto)
                anchor.mode = PathAnchorMode::Aligned;
            else
                anchor.mode = mode;
        };
        if (a.outHandle) {
            a.outHandle = p01;
            settle(a);
        }
        if (b.inHandle) {
            b.inHandle = p23;
            settle(b);
        }
    }
    const auto at = first + 1;
    path.anchors.insert(path.anchors.begin() + static_cast<std::ptrdiff_t>(at), inserted);
    return at;
}

bool deleteAnchors(PathValue& path, const AnchorSet& anchors, const std::size_t minimum) {
    std::size_t removing = 0;
    for (const auto index : anchors)
        if (index < path.anchors.size())
            ++removing;
    if (removing == 0 || path.anchors.size() - removing < minimum)
        return false;
    for (const auto index : std::views::reverse(anchors))
        if (index < path.anchors.size())
            path.anchors.erase(path.anchors.begin() + static_cast<std::ptrdiff_t>(index));
    if (path.closed && path.anchors.size() < 3)
        path.closed = false;
    refreshAuto(path);
    return true;
}

bool setClosed(PathValue& path, const bool closed) {
    if (closed && path.anchors.size() < 3)
        return false;
    path.closed = closed;
    refreshAuto(path);
    return true;
}

void reverse(PathValue& path) {
    std::ranges::reverse(path.anchors);
    for (auto& anchor : path.anchors)
        std::swap(anchor.inHandle, anchor.outHandle);
}

} // namespace bloom::ui::path_editing
