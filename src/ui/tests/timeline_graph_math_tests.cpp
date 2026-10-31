// The graph editor's arithmetic, with no window anywhere near it: the auto-fit rule, the tick
// step, the pixel/value round trip, zoom about a value, and the Bezier control points a segment's
// handles produce. These are the numbers the view paints and the numbers a drag reads back, so
// pinning them here is what leaves the view with nothing to get wrong but where it puts the ink.

#include <timeline_graph_math.hpp>

#include <array>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string_view>

namespace {

using namespace bloom;

int failures = 0;

void expect(const bool condition, const std::string_view message,
            const std::source_location location = std::source_location::current()) {
    if (condition)
        return;
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
}

[[nodiscard]] bool near(const double left, const double right) {
    return std::abs(left - right) <= 1e-9;
}

void testAutoFit() {
    const std::array values{-2.0, 6.0};
    const auto fitted = ui::fitValueViewport(values);
    expect(near(fitted.minimum, -2.8) && near(fitted.maximum, 6.8),
           "auto-fit pads its content by a tenth of the span on each side");

    const std::array flat{3.0, 3.0};
    const auto degenerate = ui::fitValueViewport(flat);
    expect(near(degenerate.minimum, 2.0) && near(degenerate.maximum, 4.0),
           "a curve whose keys share one value gets a unit window rather than no window at all");
    expect(ui::fitValueViewport({}) == ui::GraphValueViewport{-1.0, 1.0},
           "and so does a curve with nothing in it");
}

void testTicks() {
    expect(near(ui::niceTickStep(1.0, 4), 0.5),
           "a unit span at four intervals rounds 0.25 UP to the nice 0.5, never past the budget");
    expect(near(ui::niceTickStep(370.0, 4), 100.0), "a wide span rounds up to a power of ten");
    expect(near(ui::niceTickStep(0.0, 4), 1.0) && near(ui::niceTickStep(1.0, 0), 1.0),
           "a degenerate request still answers with a usable step");
    const auto ticks = ui::valueTicks({-0.5, 1.0}, 0.5);
    expect(ticks.size() == 4 && near(ticks.front(), -0.5) && near(ticks.back(), 1.0),
           "ticks start at the first multiple inside the window and stop at the last");
}

void testPixelMapping() {
    const ui::GraphValueViewport viewport{0.0, 1.0};
    expect(near(ui::pixelForValue(viewport, 1.0, 10.0, 100.0), 10.0) &&
               near(ui::pixelForValue(viewport, 0.0, 10.0, 100.0), 110.0),
           "the maximum sits at the band's top and y grows downward as the value falls");
    expect(near(ui::valueForPixel(viewport, 60.0, 10.0, 100.0), 0.5),
           "and the inverse lands back on the same value");
    expect(near(ui::pixelForValue({1.0, 1.0}, 1.0, 10.0, 100.0), 10.0),
           "a zero-height window maps to the band's top rather than dividing by zero");
}

void testZoomAndPan() {
    const ui::GraphValueViewport viewport{0.0, 1.0};
    const auto zoomed = ui::zoomValueViewport(viewport, 0.5, 0.5);
    expect(near(zoomed.minimum, 0.25) && near(zoomed.maximum, 0.75),
           "zoom keeps its anchor value exactly where it was");
    expect(ui::zoomValueViewport(viewport, 0.0, 0.5) == viewport &&
               ui::zoomValueViewport(viewport, std::nan(""), 0.5) == viewport,
           "a degenerate zoom is refused rather than collapsing the window");
    expect(ui::panValueViewport(viewport, 0.25) == ui::GraphValueViewport{0.25, 1.25},
           "pan moves both edges by the same offset");
}

void testBezierControls() {
    const auto controls = ui::bezierControls({1.0, 0.0}, {3.0, 1.0}, {0.25, 0.5}, {0.75, -0.5});
    expect(near(controls.startHandle.time, 1.5) && near(controls.startHandle.value, 0.5),
           "the outgoing handle's time is a fraction of the SEGMENT, measured from its own key");
    expect(near(controls.endHandle.time, 1.5) && near(controls.endHandle.value, 0.5),
           "and the incoming handle measures backwards from the right key");
    const auto defaults = ui::bezierControls({0.0, 0.0}, {3.0, 3.0}, {}, {});
    expect(near(defaults.startHandle.time, 1.0) && near(defaults.startHandle.value, 0.0) &&
               near(defaults.endHandle.time, 2.0) && near(defaults.endHandle.value, 3.0),
           "default handles put the control points at the segment's thirds, flat in value -- the "
           "exact shape the pre-handle Ease In-Out always had");
}

// T1 item 3: Normalize maps a curve's own extent onto -1...1, and the lens turns pointer travel back
// into REAL units, so a normalized drag writes the same value an unnormalized one would.
void testNormalizeAndLens() {
    const std::array values{2.0, 10.0, 6.0};
    const auto normalization = ui::normalizationFor(values);
    expect(near(normalization.toDisplay(2.0), -1.0) && near(normalization.toDisplay(10.0), 1.0),
           "the curve's minimum and maximum land exactly on -1 and 1");
    expect(near(normalization.fromDisplay(normalization.toDisplay(7.25)), 7.25),
           "the display transform round-trips");
    const std::array flat{4.0, 4.0};
    expect(near(ui::normalizationFor(flat).toDisplay(4.0), 0.0),
           "a flat curve normalizes to the zero line instead of dividing by zero");
    const std::array noisy{0.15, 0.15 + 3e-13, 0.15 - 2e-13};
    expect(near(ui::normalizationFor(noisy).halfSpan, 1.0) &&
               ui::fitValueViewport(noisy).span() > 1.0,
           "floating-point noise around one value is flat, never a range blown up to fill the canvas");

    const ui::GraphLens real{{0.0, 10.0}, {}, 0.0, 100.0};
    const ui::GraphLens scaled{ui::normalizedViewport(), normalization, 0.0, 100.0};
    expect(near(real.quantityPerPixel(), 0.1), "a real-unit lens moves span/height per pixel");
    expect(near(scaled.quantityFor(scaled.pixelFor(8.0)), 8.0),
           "a normalized lens maps a real value to a pixel and back");
    expect(near(scaled.quantityPerPixel(), ui::normalizedViewport().span() / 100.0 * 4.0),
           "and its pointer travel is in REAL units: display span per pixel times the half-span");
}

// T1 item 7: Shift locks the dominant axis, and a Shift-dragged handle takes 45 degree steps on its
// own side of the key.
void testShiftConstraints() {
    expect(ui::dragAxis(10.0, 3.0, true) == ui::GraphDragAxis::TimeOnly &&
               ui::dragAxis(2.0, -9.0, true) == ui::GraphDragAxis::ValueOnly &&
               ui::dragAxis(2.0, -9.0, false) == ui::GraphDragAxis::Both,
           "the dominant axis of the whole displacement wins, and only when locked");
    const auto diagonal = ui::snapHandleAngle({10.0, -8.0}, 1.0);
    expect(near(diagonal.dx, -diagonal.dy) && diagonal.dx > 0.0,
           "a handle near 45 degrees snaps onto the diagonal");
    expect(near(diagonal.dx, 9.0) && near(diagonal.dy, -9.0),
           "keeping its length along the snapped direction (the projection onto it)");
    const auto flat = ui::snapHandleAngle({-20.0, 3.0}, -1.0);
    expect(near(flat.dy, 0.0) && near(flat.dx, -20.0),
           "an incoming handle snaps to the horizontal on ITS side of the key");
    const auto backwards = ui::snapHandleAngle({-20.0, 0.5}, 1.0);
    expect(near(backwards.dx, 0.0),
           "an outgoing handle is never offered a direction pointing back across its key");
}

// T1 item 2: the Speed Graph reads the sampler, never a second shape of the curve.
void testSpeed() {
    const std::array times{0.0, 2.0, 4.0};
    const std::array modes{document::KeyframeInterpolation::Linear,
                           document::KeyframeInterpolation::Hold,
                           document::KeyframeInterpolation::Linear};
    // 0 -> 6 linearly over [0, 2], then held at 6 until 4.
    const auto sample = [](const double seconds) {
        return seconds <= 2.0 ? 3.0 * seconds : 6.0;
    };
    expect(std::abs(ui::curveSpeed(sample, times, modes, 1.0) - 3.0) < 1e-6,
           "a linear segment's speed is its slope");
    expect(ui::curveSpeed(sample, times, modes, 3.0) == 0.0, "a held segment has zero speed");
    expect(ui::curveSpeed(sample, times, modes, -1.0) == 0.0 &&
               ui::curveSpeed(sample, times, modes, 5.0) == 0.0,
           "constant extrapolation outside the keys has zero speed");
    expect(std::abs(ui::curveSpeed(sample, times, modes, 2.0, false) - 3.0) < 1e-6 &&
               ui::curveSpeed(sample, times, modes, 2.0, true) == 0.0,
           "a key's two sides are read separately: arriving at 3/s, leaving held");

    const document::KeyframeHandle handle{0.25, 1.5};
    const auto speed = ui::handleSpeed(handle, 2.0, true);
    expect(speed.has_value() && near(*speed, 3.0),
           "a handle's speed is its value offset over its own run");
    expect(near(ui::handleValueForSpeed(3.0, 0.25, 2.0, true), 1.5) &&
               near(ui::handleValueForSpeed(-3.0, 0.25, 2.0, false), 1.5),
           "and writing a speed back is the exact inverse, sign-adjusted on the incoming side");
    expect(!ui::handleSpeed({0.0, 1.0}, 2.0, true).has_value(),
           "a zero-influence handle has no defined speed");
}

// T1 item 2: a Speed Graph edit pins the far end of a segment to its straight-line tangent.
void testLinearTangentHandles() {
    const auto outgoing = ui::linearTangentHandle(1.0, 4.0, true);
    const auto incoming = ui::linearTangentHandle(1.0, 4.0, false);
    expect(near(outgoing.value, 1.0) && near(incoming.value, -1.0) &&
               near(outgoing.time, document::kDefaultKeyframeHandleTime),
           "the straight-line tangent's control points sit on the line through both keys");
}

// T1 item 11: Fit frames points on both axes with padding, and never below its minimum span.
void testFrame() {
    const std::array points{ui::GraphPoint{1.0, 2.0}, ui::GraphPoint{3.0, 4.0}};
    const auto frame = ui::frameFor(points, 0.5);
    expect(frame.has_value() && frame->t0 < 1.0 && frame->t1 > 3.0 &&
               frame->values.minimum < 2.0 && frame->values.maximum > 4.0,
           "fit frames every point with padding on both axes");
    const std::array single{ui::GraphPoint{2.0, 1.0}};
    const auto one = ui::frameFor(single, 0.5);
    expect(one.has_value() && one->t1 - one->t0 >= 0.5,
           "a single point still gets the minimum time span around it");
    expect(!ui::frameFor(std::span<const ui::GraphPoint>{}, 0.5).has_value(),
           "nothing to frame frames nothing");
}

} // namespace

int main() {
    testNormalizeAndLens();
    testShiftConstraints();
    testSpeed();
    testLinearTangentHandles();
    testFrame();
    testAutoFit();
    testTicks();
    testPixelMapping();
    testZoomAndPan();
    testBezierControls();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
