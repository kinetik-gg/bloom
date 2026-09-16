#include <algorithm>
#include <array>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/path_raster.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
using namespace bloom::render;
namespace {
int failures = 0;
void expect(bool ok, const char* message) {
    if (!ok) {
        ++failures;
        std::cerr << message << '\n';
    }
}
template <class Predicate>
void oracle(const Path& path, int width, int height, Predicate contains) {
    const auto raster = PathRaster::create(path, {}, 1, 1);
    expect(static_cast<bool>(raster), "oracle raster created");
    if (!raster)
        return;
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width));
    for (int y = 0; y < height; ++y) {
        expect(raster.value()->coverageRow(0, y, row, PathFillRule::NonZero, false),
               "oracle row produced");
        for (int x = 0; x < width; ++x) {
            unsigned count = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                    if (contains(x + (sx + 0.5) / 4, y + (sy + 0.5) / 4))
                        ++count;
            const auto expected = (count * 255 + 8) / 16;
            if (row[static_cast<std::size_t>(x)] != expected) {
                std::cerr << "coverage " << x << ',' << y << " expected " << expected << " got "
                          << static_cast<int>(row[static_cast<std::size_t>(x)]) << '\n';
                ++failures;
            }
        }
    }
}
// Independent half-plane triangle test; the star is the union of the ten triangles between
// adjacent tips/notches and its centre. No scanline intersections or renderer vertices are read.
bool triangle(double x, double y, PathPoint a, PathPoint b, PathPoint c) {
    const auto side = [x, y](PathPoint p, PathPoint q) {
        return (q.x - p.x) * (y - p.y) - (q.y - p.y) * (x - p.x);
    };
    return side(a, b) >= 0 && side(b, c) >= 0 && side(c, a) >= 0;
}
void referenceOracles() {
    oracle(rectanglePath(1, 1), 2, 2, [](double x, double y) { return x < 1 && y < 1; });
    oracle(ellipsePath(4, 4), 4, 4,
           [](double x, double y) { return (x - 2) * (x - 2) + (y - 2) * (y - 2) < 4; });
    std::array<PathPoint, 10> vertices{};
    double left = 1, right = -1, top = 1, bottom = -1;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const double a = -std::numbers::pi / 2 + static_cast<double>(i) * std::numbers::pi / 5;
        const double r = i % 2 ? 0.5 : 1;
        vertices[i] = {r * std::cos(a), r * std::sin(a)};
        left = std::min(left, vertices[i].x);
        right = std::max(right, vertices[i].x);
        top = std::min(top, vertices[i].y);
        bottom = std::max(bottom, vertices[i].y);
    }
    for (auto& p : vertices)
        p = {(p.x - left) * 8 / (right - left), (p.y - top) * 8 / (bottom - top)};
    const PathPoint center{-left * 8 / (right - left), -top * 8 / (bottom - top)};
    oracle(starPath(8, 8, 5, 0.5), 8, 8, [&](double x, double y) {
        for (std::size_t i = 0; i < vertices.size(); ++i)
            if (triangle(x, y, center, vertices[i], vertices[(i + 1) % vertices.size()]))
                return true;
        return false;
    });
}
void strokeAndRules() {
    for (auto cap : {PathStrokeCap::Butt, PathStrokeCap::Round, PathStrokeCap::Square}) {
        const auto raster =
            PathRaster::create(linePath({1, 1}, {4, 1}),
                               {1, PathStrokeAlign::Outside, PathStrokeJoin::Miter, cap}, 1, 1);
        expect(static_cast<bool>(raster), "line cap rasterized");
        std::array<std::uint8_t, 6> row{};
        expect(raster.value()->coverageRow(0, 0, row, PathFillRule::NonZero, true), "stroke row");
        expect(row[1] == 128 && row[3] == 128, "one pixel centred line has half coverage");
        expect(row[0] == (cap == PathStrokeCap::Butt    ? 0
                          : cap == PathStrokeCap::Round ? 48
                                                        : 64),
               "cap has independent known coverage");
    }
    // Traverse the same square twice: winding two fills, parity two cancels.
    auto path = rectanglePath(2, 2);
    const auto anchors = path.anchors;
    path.anchors.insert(path.anchors.end(), anchors.begin(), anchors.end());
    const auto raster = PathRaster::create(path, {}, 1, 1);
    std::array<std::uint8_t, 2> row{};
    expect(raster.value()->coverageRow(0, 0, row, PathFillRule::NonZero, false) && row[0] == 255,
           "nonzero double contour");
    expect(raster.value()->coverageRow(0, 0, row, PathFillRule::EvenOdd, false) && row[0] == 0,
           "evenodd double contour");
    for (auto join : {PathStrokeJoin::Miter, PathStrokeJoin::Round, PathStrokeJoin::Bevel})
        for (auto align :
             {PathStrokeAlign::Center, PathStrokeAlign::Inside, PathStrokeAlign::Outside}) {
            const auto shape = PathRaster::create(rectanglePath(4, 4),
                                                  {1, align, join, PathStrokeCap::Butt}, 1, 1);
            expect(static_cast<bool>(shape), "all joins and alignments");
            expect(shape.value()->coverageRow(0, 0, row, PathFillRule::NonZero, true),
                   "alignment row");
            expect((align != PathStrokeAlign::Inside || row[0] == 255) &&
                       (align != PathStrokeAlign::Outside || row[0] == 0),
                   "inside/outside coverage");
        }
    const auto proxy = PathRaster::create(rectanglePath(2, 2), {}, 0.5, 0.5);
    expect(proxy.value()->coverageRow(0, 0, row, PathFillRule::NonZero, false) && row[0] == 255 &&
               row[1] == 0,
           "proxy preserves author-space geometry");
    expect(!PathRaster::create(ellipsePath(4, 4), {}, 1, 1, [] { return true; }),
           "flatten cancellation");
    expect(
        !proxy.value()->coverageRow(0, 0, row, PathFillRule::NonZero, false, [] { return true; }),
        "row cancellation");
    expect(!PathRaster::create(linePath({0, 0}, {std::numeric_limits<double>::infinity(), 0}), {},
                               1, 1),
           "nonfinite geometry refused");
}
} // namespace
int main() {
    referenceOracles();
    strokeAndRules();
    return failures == 0 ? 0 : 1;
}
