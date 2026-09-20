// CPU-only exactness test for the bounded coverage geometry. It reconstructs
// the 8-bit coverage mask from PathRasterCoverageGeometry using exactly the
// integer sample-overlap arithmetic the GPU shader performs, then asserts it is
// byte-identical to the unchanged PathRaster::coverageRow reference over every
// shape, fill rule, stroke alignment/join/cap, clip, transform, proxy and
// adversarial sample-on-edge case. No device is required; the native producer
// test replays the same reconstruction on hardware.

#include <bloom/render/path_raster.hpp>
#include <bloom/render/text_raster.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

using namespace bloom::render;

namespace {

int failures = 0;

struct Window final {
    std::int64_t x0;
    std::int64_t y0;
    std::uint32_t w;
    std::uint32_t h;
};

[[nodiscard]] std::vector<std::uint8_t> simulate(const PathRaster& raster, const Window& window,
                                                 const PathFillRule rule, const bool stroke) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(window.w) * window.h, 0);
    const auto geometry =
        raster.coverageGeometry(window.x0, window.y0, window.w, window.h, rule, stroke);
    if (!geometry) {
        std::cerr << "geometry build failed\n";
        ++failures;
        return out;
    }
    const auto& g = *geometry.value();
    for (std::uint32_t row = 0; row < window.h; ++row) {
        for (std::uint32_t px = 0; px < window.w; ++px) {
            std::uint32_t count = 0;
            const std::uint32_t lo = px * 4U;
            const std::uint32_t hi = lo + 3U;
            for (std::uint32_t sy = 0; sy < 4; ++sy) {
                const auto range = g.rows[static_cast<std::size_t>(row) * 4U + sy];
                for (std::uint32_t i = 0; i < range.count; ++i) {
                    const auto span = g.spans[range.offset + i];
                    const std::uint32_t a = std::max(span.first, lo);
                    const std::uint32_t b = std::min(span.last, hi);
                    if (a <= b) {
                        count += b - a + 1U;
                    }
                }
            }
            out[static_cast<std::size_t>(row) * window.w + px] =
                static_cast<std::uint8_t>((count * 255U + 8U) / 16U);
        }
    }
    return out;
}

void compare(const char* label, const PathRaster& raster, const Window& window,
             const PathFillRule rule, const bool stroke) {
    std::vector<std::uint8_t> direct(static_cast<std::size_t>(window.w) * window.h, 0);
    for (std::uint32_t row = 0; row < window.h; ++row) {
        const auto offset = static_cast<std::size_t>(row) * window.w;
        if (!raster.coverageRow(window.x0, window.y0 + static_cast<std::int64_t>(row),
                                std::span<std::uint8_t>(direct.data() + offset, window.w), rule,
                                stroke)) {
            std::cerr << label << ": coverageRow failed\n";
            ++failures;
            return;
        }
    }
    const auto reconstructed = simulate(raster, window, rule, stroke);
    if (direct != reconstructed) {
        for (std::size_t i = 0; i < direct.size(); ++i) {
            if (direct[i] != reconstructed[i]) {
                std::cerr << label << ": mismatch at " << (i % window.w) << ','
                          << (i / window.w) << " cpu " << static_cast<int>(direct[i]) << " geom "
                          << static_cast<int>(reconstructed[i]) << '\n';
            }
        }
        std::cerr << label << ": FAILED\n";
        ++failures;
    }
    (void)label;
}

// Takes an rvalue/lvalue ImageResult and unwraps it before comparing.
void compareResult(const char* label, const ImageResult<PathRaster>& result, const Window& window,
                   const PathFillRule rule, const bool stroke) {
    if (!result) {
        std::cerr << label << ": raster failed\n";
        ++failures;
        return;
    }
    compare(label, *result.value(), window, rule, stroke);
}

[[nodiscard]] Path doubleRectangle() {
    auto path = rectanglePath(2, 2);
    const auto anchors = path.anchors;
    path.anchors.insert(path.anchors.end(), anchors.begin(), anchors.end());
    return path;
}

void testShapes() {
    const Window window{0, 0, 24, 16};
    const Window negWindow{-2, -2, 24, 16};
    compareResult("rect+translate",
                  PathRaster::transformed(std::array{rectanglePath(20.0, 12.0)}, {},
                                          PathMatrix{1, 0, 0, 1, 0.3, 0.3}, 1, 1),
                  negWindow, PathFillRule::NonZero, false);
    compareResult("rounded-rect", PathRaster::create(rectanglePath(18.0, 12.0, 3.5), {}, 1, 1),
                  window, PathFillRule::NonZero, false);
    compareResult("ellipse", PathRaster::create(ellipsePath(20.0, 12.0), {}, 1, 1), window,
                  PathFillRule::NonZero, false);
    compareResult("triangle", PathRaster::create(polygonPath(20.0, 12.0, 3), {}, 1, 1), window,
                  PathFillRule::NonZero, false);
    compareResult("rounded-polygon", PathRaster::create(polygonPath(20.0, 12.0, 5, 2.0), {}, 1, 1),
                  window, PathFillRule::NonZero, false);
    compareResult("star", PathRaster::create(starPath(20.0, 12.0, 5, 0.5), {}, 1, 1), window,
                  PathFillRule::NonZero, false);
    compareResult("star-evenodd", PathRaster::create(starPath(20.0, 12.0, 5, 0.5), {}, 1, 1),
                  window, PathFillRule::EvenOdd, false);
    compareResult("nonzero-double", PathRaster::create(doubleRectangle(), {}, 1, 1),
                  Window{0, 0, 3, 3}, PathFillRule::NonZero, false);
    compareResult("evenodd-double", PathRaster::create(doubleRectangle(), {}, 1, 1),
                  Window{0, 0, 3, 3}, PathFillRule::EvenOdd, false);
}

void testTransforms() {
    const double c = std::sqrt(0.5);
    compareResult("rotated-rect",
                  PathRaster::transformed(std::array{rectanglePath(8.0, 8.0)}, {},
                                          PathMatrix{c, -c, c, c, 8.1, 0}, 1, 1),
                  Window{0, 0, 16, 12}, PathFillRule::NonZero, false);
    compareResult("proxy-0.5", PathRaster::create(rectanglePath(4.0, 4.0), {}, 0.5, 0.5),
                  Window{0, 0, 4, 4}, PathFillRule::NonZero, false);
    compareResult("nonuniform-scale",
                  PathRaster::transformed(std::array{rectanglePath(8.0, 8.0)}, {},
                                          PathMatrix{1.7, 0.3, -0.2, 1.3, 0.25, 0.75}, 2.0, 1.5),
                  Window{0, 0, 20, 20}, PathFillRule::NonZero, false);
    compareResult("clipped",
                  PathRaster::transformed(std::array{rectanglePath(20.0, 12.0)}, {},
                                          PathMatrix{1, 0, 0, 1, 0.3, 0.3}, 1, 1, {},
                                          PathBounds{4.0, 3.0, 16.0, 10.0}),
                  Window{-2, -2, 24, 16}, PathFillRule::NonZero, false);
}

void testStrokes() {
    const Window window{0, 0, 24, 16};
    for (auto align : {PathStrokeAlign::Center, PathStrokeAlign::Inside, PathStrokeAlign::Outside}) {
        for (auto join : {PathStrokeJoin::Miter, PathStrokeJoin::Round, PathStrokeJoin::Bevel}) {
            const PathStroke stroke{2.0, align, join, PathStrokeCap::Butt};
            compareResult("stroke-rect",
                          PathRaster::create(rectanglePath(16.0, 10.0), stroke, 1, 1), window,
                          PathFillRule::NonZero, true);
        }
    }
    for (auto cap : {PathStrokeCap::Butt, PathStrokeCap::Round, PathStrokeCap::Square}) {
        compareResult("stroke-line-cap",
                      PathRaster::create(linePath({1, 1}, {4, 1}),
                                         {1.0, PathStrokeAlign::Outside, PathStrokeJoin::Miter, cap},
                                         1, 1),
                      Window{0, 0, 6, 6}, PathFillRule::NonZero, true);
    }
    const double c = std::sqrt(0.5);
    compareResult("stroke-rotated",
                  PathRaster::transformed(std::array{rectanglePath(10.0, 6.0)},
                                          {2.5, PathStrokeAlign::Center, PathStrokeJoin::Round,
                                           PathStrokeCap::Round},
                                          PathMatrix{c, -c, c, c, 10.0, 4.0}, 1, 1),
                  Window{0, 0, 24, 24}, PathFillRule::NonZero, true);
}

// Glyph contours with counters (holes): a real text outline is a set of closed
// paths whose NonZero winding must reproduce exactly, including the holes.
void testGlyphs() {
    const auto parameters = TextRasterParameters::create(40.0, 40.0);
    if (!parameters) {
        std::cerr << "glyph parameters failed\n";
        ++failures;
        return;
    }
    const auto outlines = textOutlines(TextFont{EmbeddedFace::DejaVuSans}, "Bo8ge",
                                       *parameters.value(), TextLayoutOptions{});
    if (!outlines || outlines.value()->empty()) {
        std::cerr << "glyph outlines failed\n";
        ++failures;
        return;
    }
    auto raster = PathRaster::transformed(std::span<const Path>(*outlines.value()), {},
                                          PathMatrix{1, 0, 0, 1, 0, 0}, 1, 1);
    if (!raster) {
        std::cerr << "glyph raster failed\n";
        ++failures;
        return;
    }
    const auto bounds = raster.value()->bounds(true, false);
    const auto x0 = static_cast<std::int64_t>(std::floor(bounds.left)) - 1;
    const auto y0 = static_cast<std::int64_t>(std::floor(bounds.top)) - 1;
    const auto width = static_cast<std::uint32_t>(
                           std::ceil(bounds.right - static_cast<double>(x0))) + 2U;
    const auto height = static_cast<std::uint32_t>(
                            std::ceil(bounds.bottom - static_cast<double>(y0))) + 2U;
    compare("glyph-contours", *raster.value(), Window{x0, y0, width, height},
            PathFillRule::NonZero, false);
    compare("glyph-contours-evenodd", *raster.value(), Window{x0, y0, width, height},
            PathFillRule::EvenOdd, false);
}

void testAdversarial() {
    compareResult("edge-on-sample", PathRaster::create(rectanglePath(3.0, 3.0), {}, 1, 1),
                  Window{0, 0, 4, 4}, PathFillRule::NonZero, false);
    compareResult("edge-shift-0.125",
                  PathRaster::transformed(std::array{rectanglePath(2.0, 2.0)}, {},
                                          PathMatrix{1, 0, 0, 1, 0.125, 0.125}, 1, 1),
                  Window{0, 0, 4, 4}, PathFillRule::NonZero, false);
    compareResult("edge-shift-0.875",
                  PathRaster::transformed(std::array{rectanglePath(2.0, 2.0)}, {},
                                          PathMatrix{1, 0, 0, 1, 0.875, 0.875}, 1, 1),
                  Window{0, 0, 4, 4}, PathFillRule::NonZero, false);
}

} // namespace

int main() {
    testShapes();
    testTransforms();
    testStrokes();
    testGlyphs();
    testAdversarial();
    if (failures != 0) {
        std::cerr << "FAIL " << failures << '\n';
        return 1;
    }
    std::cout << "PASS: coverage geometry matches coverageRow\n";
    return 0;
}
