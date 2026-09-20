// Shape and glyph parity for GpuPathCoverage: every case reconstructs the CPU
// coverageRow reference and compares it byte-for-byte with the resident GPU mask.

#include "gpu_path_coverage_test_support.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace bloom::render::gpu_path_coverage_test {
namespace {

[[nodiscard]] Path doubleRectangle() {
    auto path = rectanglePath(2, 2);
    const auto anchors = path.anchors;
    path.anchors.insert(path.anchors.end(), anchors.begin(), anchors.end());
    return path;
}

} // namespace

void testShapeMatrix(Expectations& expectations, GpuPathCoverage& producer) {
    const auto window = ImageWindow::create(-2, -2, 24, 16);
    const auto small = ImageWindow::create(0, 0, 4, 4);
    const auto wide = ImageWindow::create(0, 0, 20, 20);
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(small) &&
                            static_cast<bool>(wide),
                        "the coverage windows build");
    if (!window || !small || !wide) {
        return;
    }
    const auto& w = *window.value();
    const auto& s = *small.value();
    const auto& d = *wide.value();
    const double c = std::sqrt(0.5);

    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::transformed(std::array{rectanglePath(20.0, 12.0)}, {},
                                                         PathMatrix{1, 0, 0, 1, 0.3, 0.3}, 1, 1)),
                        w, PathFillRule::NonZero, false, "rect+translate coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(rectanglePath(18.0, 12.0, 3.5), {}, 1, 1)), w,
                        PathFillRule::NonZero, false, "rounded rect coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(ellipsePath(20.0, 12.0), {}, 1, 1)), w,
                        PathFillRule::NonZero, false, "ellipse coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(polygonPath(20.0, 12.0, 3), {}, 1, 1)), w,
                        PathFillRule::NonZero, false, "triangle coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(polygonPath(20.0, 12.0, 5, 2.0), {}, 1, 1)), w,
                        PathFillRule::NonZero, false, "rounded polygon coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(starPath(20.0, 12.0, 5, 0.5), {}, 1, 1)), w,
                        PathFillRule::NonZero, false, "star coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(starPath(20.0, 12.0, 5, 0.5), {}, 1, 1)), w,
                        PathFillRule::EvenOdd, false, "star even-odd coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(doubleRectangle(), {}, 1, 1)), w,
                        PathFillRule::NonZero, false, "nonzero double contour coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(doubleRectangle(), {}, 1, 1)), w,
                        PathFillRule::EvenOdd, false, "evenodd double contour coverage");
    const auto rotatedWindow = ImageWindow::create(0, 0, 16, 12);
    expectations.expect(static_cast<bool>(rotatedWindow), "the rotated window builds");
    if (rotatedWindow) {
        expectGeometryMatch(
            expectations, producer,
            rasterOf(PathRaster::transformed(std::array{rectanglePath(8.0, 8.0)}, {},
                                             PathMatrix{c, -c, c, c, 8.1, 0}, 1, 1)),
            *rotatedWindow.value(), PathFillRule::NonZero, false, "rotated coverage");
    }
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(rectanglePath(4.0, 4.0), {}, 0.5, 0.5)), d,
                        PathFillRule::NonZero, false, "proxy coverage");
    expectGeometryMatch(
        expectations, producer,
        rasterOf(PathRaster::transformed(std::array{rectanglePath(8.0, 8.0)}, {},
                                         PathMatrix{1.7, 0.3, -0.2, 1.3, 0.25, 0.75}, 2.0, 1.5)),
        d, PathFillRule::NonZero, false, "nonuniform scale coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::transformed(std::array{rectanglePath(20.0, 12.0)}, {},
                                                         PathMatrix{1, 0, 0, 1, 0.3, 0.3}, 1, 1, {},
                                                         PathBounds{4.0, 3.0, 16.0, 10.0})),
                        w, PathFillRule::NonZero, false, "clipped coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(rectanglePath(3.0, 3.0), {}, 1, 1)), s,
                        PathFillRule::NonZero, false, "edge-on-sample coverage");
    expectGeometryMatch(
        expectations, producer,
        rasterOf(PathRaster::transformed(std::array{rectanglePath(2.0, 2.0)}, {},
                                         PathMatrix{1, 0, 0, 1, 0.125, 0.125}, 1, 1)),
        s, PathFillRule::NonZero, false, "edge-shift-0.125 coverage");
    expectGeometryMatch(
        expectations, producer,
        rasterOf(PathRaster::transformed(std::array{rectanglePath(2.0, 2.0)}, {},
                                         PathMatrix{1, 0, 0, 1, 0.875, 0.875}, 1, 1)),
        s, PathFillRule::NonZero, false, "edge-shift-0.875 coverage");

    for (auto align :
         {PathStrokeAlign::Center, PathStrokeAlign::Inside, PathStrokeAlign::Outside}) {
        for (auto join : {PathStrokeJoin::Miter, PathStrokeJoin::Round, PathStrokeJoin::Bevel}) {
            const PathStroke stroke{2.0, align, join, PathStrokeCap::Butt};
            expectGeometryMatch(
                expectations, producer,
                rasterOf(PathRaster::create(rectanglePath(16.0, 10.0), stroke, 1, 1)), w,
                PathFillRule::NonZero, true, "stroke rect coverage");
        }
    }
    const auto capWindow = ImageWindow::create(0, 0, 6, 6);
    expectations.expect(static_cast<bool>(capWindow), "the cap window builds");
    for (auto cap : {PathStrokeCap::Butt, PathStrokeCap::Round, PathStrokeCap::Square}) {
        if (!capWindow) {
            break;
        }
        expectGeometryMatch(expectations, producer,
                            rasterOf(PathRaster::create(
                                linePath({1, 1}, {4, 1}),
                                {1.0, PathStrokeAlign::Outside, PathStrokeJoin::Miter, cap}, 1, 1)),
                            *capWindow.value(), PathFillRule::NonZero, true,
                            "stroke line cap coverage");
    }

    // Empty geometry: an anchorless path yields a zero mask.
    expectGeometryMatch(expectations, producer, rasterOf(PathRaster::create(Path{}, {}, 1, 1)), w,
                        PathFillRule::NonZero, false, "empty coverage");

    // Non-integer PAR metadata is preserved on the resident parameters.
    const auto par = PixelAspectRatio::create(4, 3);
    const auto parDisplay = ImageWindow::create(-5, 3, 12, 8);
    expectations.expect(par.has_value() && static_cast<bool>(parDisplay),
                        "the non-square pixel aspect and display window build");
    if (par.has_value() && parDisplay) {
        const auto parRaster = PathRaster::create(rectanglePath(6.0, 6.0), {}, 1, 1);
        expectations.expect(static_cast<bool>(parRaster), "the PAR coverage raster builds");
        if (!parRaster) {
            return;
        }
        const auto geometry =
            parRaster.value()->coverageGeometry(w.originX(), w.originY(), w.extent().width(),
                                                w.extent().height(), PathFillRule::NonZero, false);
        expectations.expect(static_cast<bool>(geometry), "the PAR coverage geometry builds");
        if (geometry) {
            const GpuPathCoverageParameters parameters{w, *parDisplay.value(), par.value()};
            const auto began = producer.begin(parameters, *geometry.value(), 1ULL << 34ULL);
            expectations.expect(began.code == GpuPathCoverageDiagnosticCode::None,
                                "the PAR coverage job is accepted");
            if (began.code == GpuPathCoverageDiagnosticCode::None) {
                expectations.expect(pollToCompletion(producer) == GpuPathCoveragePollResult::Ready,
                                    "the PAR coverage job completes");
            }
        }
    }
}

// Glyph contours with counters (holes), rendered by the GPU producer.
void testGlyphs(Expectations& expectations, GpuPathCoverage& producer) {
    const auto parameters = TextRasterParameters::create(40.0, 40.0);
    expectations.expect(static_cast<bool>(parameters), "the glyph parameters build");
    if (!parameters) {
        return;
    }
    const auto outlines = textOutlines(TextFont{EmbeddedFace::DejaVuSans}, "Bo8ge",
                                       *parameters.value(), TextLayoutOptions{});
    expectations.expect(static_cast<bool>(outlines) && !outlines.value()->empty(),
                        "the glyph outlines build");
    if (!outlines || outlines.value()->empty()) {
        return;
    }
    const auto raster = PathRaster::transformed(std::span<const Path>(*outlines.value()), {},
                                                PathMatrix{1, 0, 0, 1, 0, 0}, 1, 1);
    expectations.expect(static_cast<bool>(raster), "the glyph raster builds");
    if (!raster) {
        return;
    }
    const auto bounds = raster.value()->bounds(true, false);
    const auto x0 = static_cast<std::int64_t>(std::floor(bounds.left)) - 1;
    const auto y0 = static_cast<std::int64_t>(std::floor(bounds.top)) - 1;
    const auto width =
        static_cast<std::uint32_t>(std::ceil(bounds.right - static_cast<double>(x0))) + 2U;
    const auto height =
        static_cast<std::uint32_t>(std::ceil(bounds.bottom - static_cast<double>(y0))) + 2U;
    const auto window = ImageWindow::create(x0, y0, width, height);
    expectations.expect(static_cast<bool>(window), "the glyph window builds");
    if (!window) {
        return;
    }
    expectGeometryMatch(expectations, producer, *raster.value(), *window.value(),
                        PathFillRule::NonZero, false, "glyph contour coverage");
    expectGeometryMatch(expectations, producer, *raster.value(), *window.value(),
                        PathFillRule::EvenOdd, false, "glyph contour even-odd coverage");
}

} // namespace bloom::render::gpu_path_coverage_test
