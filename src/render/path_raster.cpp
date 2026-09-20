#include <algorithm>
#include <array>
#include <bloom/render/path_raster.hpp>
#include <cmath>
#include <limits>
#include <numbers>
namespace bloom::render {
namespace {
constexpr std::size_t kMaximumSegments = 262144;
constexpr double kKappa = 0.5522847498307936;
PathPoint add(PathPoint a, PathPoint b) { return {a.x + b.x, a.y + b.y}; }
PathPoint sub(PathPoint a, PathPoint b) { return {a.x - b.x, a.y - b.y}; }
PathPoint mul(PathPoint a, double b) { return {a.x * b, a.y * b}; }
double cross(PathPoint a, PathPoint b) { return a.x * b.y - a.y * b.x; }
double length(PathPoint a) { return std::hypot(a.x, a.y); }
bool finite(PathPoint a) { return std::isfinite(a.x) && std::isfinite(a.y); }
bool stopped(const PathCancellation& cancelled) { return cancelled && cancelled(); }
bool append(std::vector<PathPoint>& points, PathPoint point) {
    if (!finite(point) || points.size() >= kMaximumSegments)
        return false;
    if (points.empty() || points.back() != point)
        points.push_back(point);
    return true;
}
// The control polygon's excess length also detects collinear backtracking and closed cusps.
bool flatten(PathPoint a, PathPoint b, PathPoint c, PathPoint d, double tolerance, unsigned depth,
             std::vector<PathPoint>& points, const PathCancellation& cancelled) {
    if (stopped(cancelled))
        return false;
    const auto chord = sub(d, a);
    const auto chordLength = length(chord);
    const auto excess = length(sub(b, a)) + length(sub(c, b)) + length(sub(d, c)) - chordLength;
    const auto deviation = chordLength > 0 ? std::max(std::abs(cross(sub(b, a), chord)),
                                                      std::abs(cross(sub(c, a), chord))) /
                                                 chordLength
                                           : std::max(length(sub(b, a)), length(sub(c, a)));
    if (excess <= tolerance && deviation <= tolerance)
        return append(points, d);
    if (depth == 20)
        return false;
    const auto ab = mul(add(a, b), 0.5), bc = mul(add(b, c), 0.5), cd = mul(add(c, d), 0.5);
    const auto abc = mul(add(ab, bc), 0.5), bcd = mul(add(bc, cd), 0.5);
    const auto middle = mul(add(abc, bcd), 0.5);
    return flatten(a, ab, abc, middle, tolerance, depth + 1, points, cancelled) &&
           flatten(middle, bcd, cd, d, tolerance, depth + 1, points, cancelled);
}
void positive(std::vector<PathPoint>& polygon) {
    double area = 0;
    for (std::size_t i = 0; i < polygon.size(); ++i)
        area += cross(polygon[i], polygon[(i + 1) % polygon.size()]);
    if (area < 0)
        std::ranges::reverse(polygon);
}
struct Crossing {
    double x;
    int winding;
};
// One crossing reduced to the first integer sample index at which the CPU
// `crossing.x <= sampleX` test becomes true. `list` tags the winding accumulator
// (0 fill, 1 outline, 2 clip) so one merged sweep can track all three.
struct CoverageEvent {
    std::uint32_t threshold;
    std::uint8_t list;
    int winding;
};
constexpr std::uint32_t kMaximumCoverageSpans = 1U << 22U;
// Resolves the exact CPU boundary comparison `crossingX <= sampleX(g)` where
// `sampleX(g)` is the real quarter-sample expression from coverageRow. A bounded
// binary search avoids an approximate `ceil` that could disagree on a sample
// whose centre lies exactly on an edge.
[[nodiscard]] std::uint32_t coverageThreshold(const double crossingX, const std::int64_t x,
                                              const std::uint32_t width, const double scaleX) {
    const std::uint64_t sampleCount = static_cast<std::uint64_t>(width) * 4ULL;
    std::uint64_t lo = 0, hi = sampleCount;
    while (lo < hi) {
        const std::uint64_t middle = lo + (hi - lo) / 2;
        const auto pixel = static_cast<std::uint32_t>(middle >> 2);
        const auto sub = static_cast<std::uint32_t>(middle & 3U);
        const double sampleX = (static_cast<double>(x) + static_cast<double>(pixel) +
                                (static_cast<double>(sub) + 0.5) / 4) /
                               scaleX;
        if (sampleX >= crossingX) {
            hi = middle;
        } else {
            lo = middle + 1;
        }
    }
    return static_cast<std::uint32_t>(lo);
}
void appendCoverageEvents(const std::vector<Crossing>& crossings, const std::uint8_t list,
                          const std::int64_t x, const std::uint32_t width, const double scaleX,
                          std::vector<CoverageEvent>& out) {
    const std::uint32_t sampleCount = width * 4U;
    for (const auto& crossing : crossings) {
        const std::uint32_t threshold = coverageThreshold(crossing.x, x, width, scaleX);
        if (threshold < sampleCount) {
            out.push_back({threshold, list, crossing.winding});
        }
    }
}
void crossings(std::span<const PathPoint> polygon, double y, std::vector<Crossing>& out) {
    if (polygon.size() < 3)
        return;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
        if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y))
            out.push_back({a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x), b.y > a.y ? 1 : -1});
    }
}
void sortCrossings(std::vector<Crossing>& values) {
    std::ranges::sort(values, [](const auto& a, const auto& b) { return a.x < b.x; });
}
bool inside(int winding, PathFillRule rule) {
    return rule == PathFillRule::NonZero ? winding != 0 : winding % 2 != 0;
}
Path fromPoints(const std::vector<PathPoint>& points) {
    Path path;
    path.closed = true;
    for (auto point : points)
        path.anchors.push_back({point, {}, {}});
    return path;
}
void fitBox(Path& path, double width, double height) {
    if (path.anchors.empty())
        return;
    PathBounds b{path.anchors[0].point.x, path.anchors[0].point.y, path.anchors[0].point.x,
                 path.anchors[0].point.y};
    for (const auto& a : path.anchors) {
        b.left = std::min(b.left, a.point.x);
        b.right = std::max(b.right, a.point.x);
        b.top = std::min(b.top, a.point.y);
        b.bottom = std::max(b.bottom, a.point.y);
    }
    for (auto& a : path.anchors)
        a.point = {(a.point.x - b.left) * width / (b.right - b.left),
                   (a.point.y - b.top) * height / (b.bottom - b.top)};
}
} // namespace

ImageResult<PathRaster> PathRaster::create(const Path& path, const PathStroke stroke,
                                           const double scaleX, const double scaleY,
                                           PathCancellation cancelled) {
    const auto failure = [] {
        return ImageResult<PathRaster>::failure(
            ImageError::codeOnly(ImageErrorCode::InvalidParameter));
    };
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0 || scaleY <= 0 ||
        !std::isfinite(stroke.width) || stroke.width < 0 || path.anchors.size() > 4096)
        return failure();
    PathRaster raster;
    raster.scaleX_ = scaleX;
    raster.scaleY_ = scaleY;
    raster.align_ = path.closed ? stroke.align : PathStrokeAlign::Center;
    const double tolerance = 1.0 / (32.0 * std::max(scaleX, scaleY));
    for (const auto& a : path.anchors)
        if (!finite(a.point) || (a.inHandle && !finite(*a.inHandle)) ||
            (a.outHandle && !finite(*a.outHandle)))
            return failure();
    if (path.anchors.empty())
        return ImageResult<PathRaster>::success(std::move(raster));
    if (!append(raster.fill_, path.anchors.front().point))
        return failure();
    const auto count = path.anchors.size();
    for (std::size_t i = 0; i < count - (path.closed ? 0 : 1); ++i) {
        const auto& a = path.anchors[i];
        const auto& b = path.anchors[(i + 1) % count];
        if (!flatten(a.point, a.outHandle.value_or(a.point), b.inHandle.value_or(b.point), b.point,
                     tolerance, 0, raster.fill_, cancelled))
            return failure();
    }
    if (path.closed && raster.fill_.size() > 1 && raster.fill_.front() == raster.fill_.back())
        raster.fill_.pop_back();
    if (stroke.width == 0 || raster.fill_.size() < 2)
        return ImageResult<PathRaster>::success(std::move(raster));
    const double half = stroke.width * (raster.align_ == PathStrokeAlign::Center ? 0.5 : 1.0);
    std::size_t outlinePoints = 0;
    const auto polygon = [&](std::vector<PathPoint> p) {
        outlinePoints += p.size();
        if (outlinePoints > kMaximumSegments || !std::ranges::all_of(p, finite))
            return false;
        positive(p);
        raster.outlines_.push_back(std::move(p));
        return true;
    };
    const auto disc = [&](PathPoint center) {
        const double angle = 2 * std::acos(std::clamp(1 - tolerance / half, -1.0, 1.0));
        if (!std::isfinite(angle) || angle <= 0)
            return false;
        const auto countDouble = std::max(8.0, std::ceil(2 * std::numbers::pi / angle));
        if (countDouble > static_cast<double>(kMaximumSegments))
            return false;
        const auto segments = static_cast<std::size_t>(countDouble);
        std::vector<PathPoint> points;
        for (std::size_t i = 0; i < segments; ++i) {
            if (stopped(cancelled))
                return false;
            const double a =
                2 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(segments);
            points.push_back(add(center, {half * std::cos(a), half * std::sin(a)}));
        }
        return polygon(std::move(points));
    };
    const auto n = raster.fill_.size();
    for (std::size_t i = 0; i < n - (path.closed ? 0 : 1); ++i) {
        if (stopped(cancelled))
            return failure();
        auto a = raster.fill_[i], b = raster.fill_[(i + 1) % n];
        const auto direction = mul(sub(b, a), 1 / length(sub(b, a)));
        const PathPoint normal{-direction.y * half, direction.x * half};
        if (!path.closed && stroke.cap == PathStrokeCap::Square) {
            if (i == 0)
                a = sub(a, mul(direction, half));
            if (i + 2 == n)
                b = add(b, mul(direction, half));
        }
        if (!polygon({add(a, normal), add(b, normal), sub(b, normal), sub(a, normal)}))
            return failure();
    }
    for (std::size_t i = path.closed ? 0 : 1; i < n - (path.closed ? 0 : 1); ++i) {
        if (stopped(cancelled))
            return failure();
        const auto p = raster.fill_[i];
        if (stroke.join == PathStrokeJoin::Round) {
            if (!disc(p))
                return failure();
            continue;
        }
        const auto before = sub(p, raster.fill_[(i + n - 1) % n]);
        const auto after = sub(raster.fill_[(i + 1) % n], p);
        const auto u = mul(before, 1 / length(before)), v = mul(after, 1 / length(after));
        const auto turn = cross(u, v);
        if (std::abs(turn) < 1e-12)
            continue;
        const double side = turn > 0 ? -half : half;
        const auto a = add(p, {-u.y * side, u.x * side});
        const auto b = add(p, {-v.y * side, v.x * side});
        const auto miter = add(a, mul(u, cross(sub(b, a), v) / turn));
        if (stroke.join == PathStrokeJoin::Miter && length(sub(miter, p)) <= 4 * half) {
            if (!polygon({p, a, miter, b}))
                return failure();
        } else if (!polygon({p, a, b}))
            return failure();
    }
    if (!path.closed && stroke.cap == PathStrokeCap::Round &&
        (!disc(raster.fill_.front()) || !disc(raster.fill_.back())))
        return failure();
    return ImageResult<PathRaster>::success(std::move(raster));
}
ImageResult<PathRaster> PathRaster::transformed(std::span<const Path> paths, PathStroke stroke,
                                                PathMatrix matrix, double scaleX, double scaleY,
                                                const PathCancellation& cancelled,
                                                std::optional<PathBounds> clip) {
    // Frobenius norm bounds the maximum stretch, including shear and reflections.
    const double norm = std::hypot(std::hypot(matrix.a, matrix.b), std::hypot(matrix.c, matrix.d));
    if (!std::isfinite(norm) || !std::isfinite(matrix.x) || !std::isfinite(matrix.y) || norm == 0 ||
        !std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0 || scaleY <= 0)
        return ImageResult<PathRaster>::failure(
            ImageError::codeOnly(ImageErrorCode::InvalidParameter));
    PathRaster result;
    result.scaleX_ = scaleX;
    result.scaleY_ = scaleY;
    const auto transform = [&](auto& points) {
        for (auto& point : points) {
            point = matrix.map(point);
            if (!finite(point))
                return false;
        }
        return true;
    };
    std::size_t total = 0;
    for (const auto& path : paths) {
        auto native = create(path, stroke, scaleX * norm, scaleY * norm, cancelled);
        if (!native)
            return native;
        auto& raster = *native.value();
        result.align_ = raster.align_;
        total += raster.fill_.size();
        if (total > kMaximumSegments || !transform(raster.fill_))
            return ImageResult<PathRaster>::failure(
                ImageError::codeOnly(ImageErrorCode::InvalidParameter));
        result.contours_.push_back(std::move(raster.fill_));
        for (auto& outline : raster.outlines_) {
            total += outline.size();
            if (total > kMaximumSegments || !transform(outline))
                return ImageResult<PathRaster>::failure(
                    ImageError::codeOnly(ImageErrorCode::InvalidParameter));
            result.outlines_.push_back(std::move(outline));
        }
    }
    if (clip) {
        result.clip_ = {{clip->left, clip->top},
                        {clip->right, clip->top},
                        {clip->right, clip->bottom},
                        {clip->left, clip->bottom}};
        if (!transform(result.clip_))
            return ImageResult<PathRaster>::failure(
                ImageError::codeOnly(ImageErrorCode::InvalidParameter));
    }
    return ImageResult<PathRaster>::success(std::move(result));
}
ImageResult<PathRasterCoverageGeometry>
PathRaster::coverageGeometry(const std::int64_t x, const std::int64_t y, const std::uint32_t width,
                             const std::uint32_t height, const PathFillRule rule,
                             const bool stroke, const PathCancellation& cancelled) const {
    const auto failure = [] {
        return ImageResult<PathRasterCoverageGeometry>::failure(
            ImageError::codeOnly(ImageErrorCode::InvalidParameter));
    };
    if (width == 0 || height == 0 ||
        static_cast<std::uint64_t>(width) * 4ULL >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
        return failure();
    PathRasterCoverageGeometry geometry;
    geometry.width = width;
    geometry.height = height;
    geometry.rows.resize(static_cast<std::size_t>(height) * 4ULL);
    const std::uint32_t sampleCount = width * 4U;
    std::vector<Crossing> fill, outline, clip;
    std::vector<CoverageEvent> events;
    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t sy = 0; sy < 4; ++sy) {
            if (stopped(cancelled))
                return failure();
            const double sampleY =
                (static_cast<double>(y) + static_cast<double>(row) +
                 (static_cast<double>(sy) + 0.5) / 4) /
                scaleY_;
            fill.clear();
            outline.clear();
            clip.clear();
            events.clear();
            crossings(clip_, sampleY, clip);
            sortCrossings(clip);
            if (!stroke || align_ != PathStrokeAlign::Center) {
                crossings(fill_, sampleY, fill);
                for (const auto& contour : contours_)
                    crossings(contour, sampleY, fill);
            }
            if (stroke)
                for (const auto& polygon : outlines_)
                    crossings(polygon, sampleY, outline);
            sortCrossings(fill);
            sortCrossings(outline);
            appendCoverageEvents(fill, 0, x, width, scaleX_, events);
            appendCoverageEvents(outline, 1, x, width, scaleX_, events);
            if (!clip_.empty())
                appendCoverageEvents(clip, 2, x, width, scaleX_, events);
            std::ranges::sort(events, [](const CoverageEvent& a, const CoverageEvent& b) {
                return a.threshold < b.threshold;
            });

            const auto emit = [&](const std::uint32_t first, const std::uint32_t last, const int fw,
                                  const int sw, const int cw) -> bool {
                const bool inFill = inside(fw, rule);
                const bool covered =
                    stroke ? sw != 0 && (align_ == PathStrokeAlign::Center ||
                                         (align_ == PathStrokeAlign::Inside ? inFill : !inFill))
                           : inFill;
                if (!covered || (!clip_.empty() && cw == 0))
                    return true;
                if (geometry.spans.size() >= kMaximumCoverageSpans)
                    return false;
                geometry.spans.push_back({first, last});
                return true;
            };

            const auto rangeIndex = static_cast<std::size_t>(row) * 4ULL + sy;
            const auto beginOffset = geometry.spans.size();
            std::size_t eventIndex = 0;
            int fw = 0, sw = 0, cw = 0;
            const auto applyEventsAt = [&](const std::uint32_t threshold) {
                while (eventIndex < events.size() && events[eventIndex].threshold == threshold) {
                    const auto& event = events[eventIndex++];
                    if (event.list == 0)
                        fw += event.winding;
                    else if (event.list == 1)
                        sw += event.winding;
                    else
                        cw += event.winding;
                }
            };
            applyEventsAt(0);
            std::uint32_t cursor = 0;
            while (eventIndex < events.size()) {
                const std::uint32_t threshold = events[eventIndex].threshold;
                if (threshold > cursor) {
                    if (!emit(cursor, threshold - 1, fw, sw, cw))
                        return failure();
                    cursor = threshold;
                }
                applyEventsAt(threshold);
            }
            if (cursor < sampleCount && !emit(cursor, sampleCount - 1, fw, sw, cw))
                return failure();
            geometry.rows[rangeIndex] = {static_cast<std::uint32_t>(beginOffset),
                                         static_cast<std::uint32_t>(geometry.spans.size() -
                                                                    beginOffset)};
        }
    }
    return ImageResult<PathRasterCoverageGeometry>::success(std::move(geometry));
}

PathBounds PathRaster::bounds(bool fill, bool stroke) const noexcept {
    PathBounds bounds;
    bool first = true;
    const auto include = [&](const auto& points) {
        for (const auto p : points) {
            if (first) {
                bounds = {p.x, p.y, p.x, p.y};
                first = false;
            } else {
                bounds.left = std::min(bounds.left, p.x);
                bounds.top = std::min(bounds.top, p.y);
                bounds.right = std::max(bounds.right, p.x);
                bounds.bottom = std::max(bounds.bottom, p.y);
            }
        }
    };
    if (fill || (stroke && align_ == PathStrokeAlign::Inside)) {
        include(fill_);
        for (const auto& contour : contours_)
            include(contour);
    }
    if (stroke && align_ != PathStrokeAlign::Inside)
        for (const auto& outline : outlines_)
            include(outline);
    return bounds;
}
bool PathRaster::coverageRow(std::int64_t x, std::int64_t y, std::span<std::uint8_t> row,
                             PathFillRule rule, bool stroke,
                             const PathCancellation& cancelled) const {
    std::fill(row.begin(), row.end(), 0);
    std::vector<Crossing> fill, outline, clip;
    for (int sy = 0; sy < 4; ++sy) {
        if (stopped(cancelled))
            return false;
        const double sampleY =
            (static_cast<double>(y) + (static_cast<double>(sy) + 0.5) / 4) / scaleY_;
        fill.clear();
        outline.clear();
        clip.clear();
        crossings(clip_, sampleY, clip);
        sortCrossings(clip);
        if (!stroke || align_ != PathStrokeAlign::Center) {
            crossings(fill_, sampleY, fill);
            for (const auto& contour : contours_)
                crossings(contour, sampleY, fill);
        }
        if (stroke)
            for (const auto& polygon : outlines_)
                crossings(polygon, sampleY, outline);
        sortCrossings(fill);
        sortCrossings(outline);
        std::size_t fi = 0, si = 0, ci = 0;
        int fw = 0, sw = 0, cw = 0;
        for (std::size_t px = 0; px < row.size(); ++px) {
            if (px % 256 == 0 && stopped(cancelled))
                return false;
            for (int sx = 0; sx < 4; ++sx) {
                const double sampleX = (static_cast<double>(x) + static_cast<double>(px) +
                                        (static_cast<double>(sx) + 0.5) / 4) /
                                       scaleX_;
                while (fi < fill.size() && fill[fi].x <= sampleX)
                    fw += fill[fi++].winding;
                while (si < outline.size() && outline[si].x <= sampleX)
                    sw += outline[si++].winding;
                while (ci < clip.size() && clip[ci].x <= sampleX)
                    cw += clip[ci++].winding;
                const bool inFill = inside(fw, rule);
                const bool covered =
                    stroke ? sw != 0 && (align_ == PathStrokeAlign::Center ||
                                         (align_ == PathStrokeAlign::Inside ? inFill : !inFill))
                           : inFill;
                if (covered && (clip_.empty() || cw != 0))
                    ++row[px];
            }
        }
    }
    for (auto& coverage : row)
        coverage = static_cast<std::uint8_t>((static_cast<unsigned>(coverage) * 255U + 8U) / 16U);
    return true;
}
Path rectanglePath(double width, double height, double radius) {
    if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0)
        return {};
    const double r = std::clamp(radius, 0.0, std::min(width, height) / 2);
    if (r == 0)
        return fromPoints({{0, 0}, {width, 0}, {width, height}, {0, height}});
    const double k = r * kKappa;
    return {{{{r, 0}, PathPoint{r - k, 0}, {}},
             {{width - r, 0}, {}, PathPoint{width - r + k, 0}},
             {{width, r}, PathPoint{width, r - k}, {}},
             {{width, height - r}, {}, PathPoint{width, height - r + k}},
             {{width - r, height}, PathPoint{width - r + k, height}, {}},
             {{r, height}, {}, PathPoint{r - k, height}},
             {{0, height - r}, PathPoint{0, height - r + k}, {}},
             {{0, r}, {}, PathPoint{0, r - k}}},
            true};
}
Path ellipsePath(double width, double height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0)
        return {};
    const double x = width / 2, y = height / 2, kx = x * kKappa, ky = y * kKappa;
    return {{{{x, 0}, PathPoint{x - kx, 0}, PathPoint{x + kx, 0}},
             {{width, y}, PathPoint{width, y - ky}, PathPoint{width, y + ky}},
             {{x, height}, PathPoint{x + kx, height}, PathPoint{x - kx, height}},
             {{0, y}, PathPoint{0, y + ky}, PathPoint{0, y - ky}}},
            true};
}
Path polygonPath(double width, double height, std::uint32_t points, double radius) {
    if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0)
        return {};
    if (points < 3 || points > 64)
        return {};
    Path path;
    path.closed = true;
    for (std::uint32_t i = 0; i < points; ++i) {
        const double a = -std::numbers::pi / 2 + 2 * std::numbers::pi * i / points;
        path.anchors.push_back({{std::cos(a), std::sin(a)}, {}, {}});
    }
    fitBox(path, width, height);
    if (radius <= 0)
        return path;
    Path rounded;
    rounded.closed = true;
    for (std::size_t i = 0; i < path.anchors.size(); ++i) {
        const auto p = path.anchors[i].point,
                   before = path.anchors[(i + points - 1) % points].point,
                   after = path.anchors[(i + 1) % points].point;
        const auto u = sub(before, p), v = sub(after, p);
        const double lu = length(u), lv = length(v);
        if (lu == 0 || lv == 0)
            return path;
        const auto un = mul(u, 1 / lu), vn = mul(v, 1 / lv);
        const double angle = std::acos(std::clamp(un.x * vn.x + un.y * vn.y, -1.0, 1.0));
        const double tangent = std::tan(angle / 2);
        if (tangent <= 0 || !std::isfinite(tangent))
            return path;
        const double d = std::min({radius / tangent, lu / 2, lv / 2});
        const double k = 4.0 / 3.0 * std::tan((std::numbers::pi - angle) / 4) * d * tangent;
        const auto a = add(p, mul(un, d)), b = add(p, mul(vn, d));
        rounded.anchors.push_back({a, {}, sub(a, mul(un, k))});
        rounded.anchors.push_back({b, sub(b, mul(vn, k)), {}});
    }
    return rounded;
}
Path starPath(double width, double height, std::uint32_t points, double innerRatio) {
    if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0)
        return {};
    if (points < 3 || points > 64 || !std::isfinite(innerRatio) || innerRatio < 0 || innerRatio > 1)
        return {};
    Path path;
    path.closed = true;
    for (std::uint32_t i = 0; i < points * 2; ++i) {
        const double a = -std::numbers::pi / 2 + std::numbers::pi * i / points;
        const double r = i % 2 ? innerRatio : 1;
        path.anchors.push_back({{r * std::cos(a), r * std::sin(a)}, {}, {}});
    }
    fitBox(path, width, height);
    return path;
}
Path linePath(PathPoint start, PathPoint end) { return {{{start, {}, {}}, {end, {}, {}}}, false}; }
} // namespace bloom::render
