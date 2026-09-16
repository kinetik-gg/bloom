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
    [[nodiscard]] PathBounds bounds(bool fill, bool stroke) const noexcept;
    [[nodiscard]] bool coverageRow(std::int64_t x, std::int64_t y, std::span<std::uint8_t> row,
                                   PathFillRule rule, bool stroke,
                                   const PathCancellation& cancelled = {}) const;

  private:
    std::vector<PathPoint> fill_;
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
