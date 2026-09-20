#pragma once
#include <bloom/render/image_types.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>
namespace bloom::render {
struct PathPoint {
    double x = 0, y = 0;
    friend bool operator==(const PathPoint&, const PathPoint&) = default;
};
struct PathMatrix {
    double a = 1, b = 0, c = 0, d = 1, x = 0, y = 0;
    [[nodiscard]] PathPoint map(PathPoint p) const noexcept {
        return {a * p.x + b * p.y + x, c * p.x + d * p.y + y};
    }
};
struct PathAnchor {
    PathPoint point;
    std::optional<PathPoint> inHandle, outHandle;
};
struct Path {
    std::vector<PathAnchor> anchors;
    bool closed = false;
};
enum class PathFillRule { NonZero, EvenOdd };
enum class PathStrokeAlign { Center, Inside, Outside };
enum class PathStrokeJoin { Miter, Round, Bevel };
enum class PathStrokeCap { Butt, Round, Square };
struct PathStroke {
    double width = 0;
    PathStrokeAlign align = PathStrokeAlign::Center;
    PathStrokeJoin join = PathStrokeJoin::Miter;
    PathStrokeCap cap = PathStrokeCap::Butt;
};
struct PathBounds {
    double left = 0, top = 0, right = 0, bottom = 0;
};
// Immutable, bounded scanline coverage geometry: for each output row and each of
// the four sub-scanlines, the inclusive sample-index spans where a 4 x 4 centre
// sample is covered. A sample index is `pixel * 4 + sx` within one row, so every
// span is an integer grid interval and the consumer needs no Float64/Int64 to
// count coverage. The CPU builds only geometry (O(edges * rows), no per-pixel
// scan); the exact CPU `coverageRow` boundary comparison is preserved by
// resolving each crossing through a bounded binary search over the real
// quarter-sample positions.
struct PathRasterCoverageRange {
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
    friend bool operator==(const PathRasterCoverageRange&,
                           const PathRasterCoverageRange&) = default;
};
struct PathRasterCoverageSpan {
    std::uint32_t first = 0; // inclusive sample index within the row
    std::uint32_t last = 0;  // inclusive sample index within the row
    friend bool operator==(const PathRasterCoverageSpan&, const PathRasterCoverageSpan&) = default;
};
struct PathRasterCoverageGeometry {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Exactly height * 4 entries; the row-major (row * 4 + sy) sub-scanline range.
    std::vector<PathRasterCoverageRange> rows;
    // Concatenated spans; a range's [offset, offset + count) selects its own spans.
    std::vector<PathRasterCoverageSpan> spans;
};
// Cancellation is checked during flattening, outline construction and every coverage subscanline.
using PathCancellation = std::function<bool()>;
// Coordinates remain in author space. Flattening error is at most 1/32 output pixel at the
// larger proxy scale. Coverage uses a fixed 4 x 4 centre-sample grid, rounded to 8-bit linear area.
// The stroke is a union of consistently wound outline polygons. Non-centred closed strokes
// intersect that outline with the fill's inside/outside at each sample, including self crossings.
class PathRaster final {
  public:
    [[nodiscard]] static ImageResult<PathRaster> create(const Path& path, PathStroke stroke,
                                                        double scaleX, double scaleY,
                                                        PathCancellation cancelled = {});
    // Flatten and construct strokes in native units, then transform their geometry before coverage.
    [[nodiscard]] static ImageResult<PathRaster>
    transformed(std::span<const Path> paths, PathStroke stroke, PathMatrix matrix, double scaleX,
                double scaleY, const PathCancellation& cancelled = {},
                std::optional<PathBounds> clip = std::nullopt);
    [[nodiscard]] PathBounds bounds(bool fill, bool stroke) const noexcept;
    [[nodiscard]] bool coverageRow(std::int64_t x, std::int64_t y, std::span<std::uint8_t> row,
                                   PathFillRule rule, bool stroke,
                                   const PathCancellation& cancelled = {}) const;
    // The bounded scanline geometry behind coverageRow for one output window. It
    // uses the identical crossings, fill rule, stroke alignment and clip rule as
    // coverageRow but emits integer sample spans instead of a per-pixel mask.
    [[nodiscard]] ImageResult<PathRasterCoverageGeometry>
    coverageGeometry(std::int64_t x, std::int64_t y, std::uint32_t width, std::uint32_t height,
                     PathFillRule rule, bool stroke, const PathCancellation& cancelled = {}) const;

  private:
    std::vector<PathPoint> fill_;
    std::vector<std::vector<PathPoint>> contours_;
    std::vector<PathPoint> clip_;
    std::vector<std::vector<PathPoint>> outlines_;
    PathStrokeAlign align_ = PathStrokeAlign::Center;
    double scaleX_ = 1, scaleY_ = 1;
};
[[nodiscard]] Path rectanglePath(double width, double height, double radius = 0);
[[nodiscard]] Path ellipsePath(double width, double height);
[[nodiscard]] Path polygonPath(double width, double height, std::uint32_t points,
                               double radius = 0);
[[nodiscard]] Path starPath(double width, double height, std::uint32_t points, double innerRatio);
[[nodiscard]] Path linePath(PathPoint start, PathPoint end);
} // namespace bloom::render
