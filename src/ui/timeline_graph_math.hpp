#pragma once

// The graph editor's arithmetic, with no widget and no painter anywhere in it: the value viewport
// and its auto-fit, the pixel/value mapping, the tick step, and a segment's cubic Bezier control
// points. Kept separate from timeline_graph_view.cpp precisely so a test can pin the numbers
// without a window -- the view then has nothing left to get wrong except where it puts the ink.

#include <bloom/document/animation.hpp>

#include <span>
#include <vector>

namespace bloom::ui {

// A curve's vertical window, in the curve's own value units. Every curve on the canvas has its
// own, because a rotation in degrees and an opacity in [0, 1] cannot share one.
struct GraphValueViewport final {
    double minimum = -1.0;
    double maximum = 1.0;

    [[nodiscard]] double span() const noexcept { return maximum - minimum; }
    friend bool operator==(const GraphValueViewport&, const GraphValueViewport&) = default;
};

// One point of a curve in graph space: seconds along the shared time axis, and the curve's value.
struct GraphPoint final {
    double time = 0.0;
    double value = 0.0;

    friend bool operator==(const GraphPoint&, const GraphPoint&) = default;
};

// A segment's four cubic Bezier control points, in graph space.
struct GraphSegmentControls final {
    GraphPoint start;
    GraphPoint startHandle;
    GraphPoint endHandle;
    GraphPoint end;

    friend bool operator==(const GraphSegmentControls&, const GraphSegmentControls&) = default;
};

// The fraction of a viewport's height by which auto-fit pads its content, top and bottom, so a
// curve's extremes do not sit exactly on the canvas edge.
inline constexpr double kGraphViewportPadding = 0.1;

// The window that shows every one of `values` with padding. An empty input, or one whose values
// are all equal, is degenerate and gets a unit window around that value (or around zero), because
// a zero-height window has no pixel mapping at all.
[[nodiscard]] GraphValueViewport fitValueViewport(std::span<const double> values);

// The "nice" tick step for `span` at AT MOST `targetCount` intervals: 1, 2 or 5 times a power of
// ten, rounded UP from span/targetCount, so labels read as round numbers at every zoom and the
// gutter never has to fit more of them than it was sized for.
[[nodiscard]] double niceTickStep(double span, int targetCount);

// The tick values inside `viewport` at `step`, lowest first.
[[nodiscard]] std::vector<double> valueTicks(const GraphValueViewport& viewport, double step);

// The pixel/value mapping. `top` is the y of the viewport's MAXIMUM and `height` its extent, so y
// grows downward as values fall -- the orientation every curve editor uses.
[[nodiscard]] double pixelForValue(const GraphValueViewport& viewport, double value, double top,
                                   double height);
[[nodiscard]] double valueForPixel(const GraphValueViewport& viewport, double pixelY, double top,
                                   double height);

// Zoom about a fixed value: `anchor` keeps its pixel, and the window's span scales by `factor`.
[[nodiscard]] GraphValueViewport zoomValueViewport(const GraphValueViewport& viewport,
                                                   double factor, double anchor);
// Pan by a value offset.
[[nodiscard]] GraphValueViewport panValueViewport(const GraphValueViewport& viewport,
                                                  double offset);

// The segment between two keys, as the same cubic Bezier the SAMPLER evaluates: handle times are
// fractions of the segment measured from their own key, handle values are offsets from their own
// key's value. The view draws control points from this and the sampler reads the identical
// handles, so the drawn tangent and the sampled curve cannot describe different shapes.
[[nodiscard]] GraphSegmentControls bezierControls(GraphPoint start, GraphPoint end,
                                                  document::KeyframeHandle outgoing,
                                                  document::KeyframeHandle incoming);

} // namespace bloom::ui
