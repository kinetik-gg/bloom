// The graph editor's arithmetic, with no window anywhere near it: the auto-fit rule, the tick
// step, the pixel/value round trip, zoom about a value, and the Bezier control points a segment's
// handles produce. These are the numbers the view paints and the numbers a drag reads back, so
// pinning them here is what leaves the view with nothing to get wrong but where it puts the ink.

#include <timeline_graph_math.hpp>

#include <array>
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

} // namespace

int main() {
    testAutoFit();
    testTicks();
    testPixelMapping();
    testZoomAndPan();
    testBezierControls();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
