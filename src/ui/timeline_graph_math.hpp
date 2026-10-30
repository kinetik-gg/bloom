#pragma once

// The graph editor's arithmetic, with no widget and no painter anywhere in it: the value viewport
// and its auto-fit, the pixel/value mapping, the tick step, and a segment's cubic Bezier control
// points. Kept separate from timeline_graph_view.cpp precisely so a test can pin the numbers
// without a window -- the view then has nothing left to get wrong except where it puts the ink.

#include <bloom/document/animation.hpp>

#include <cstdint>
#include <optional>

#include <functional>
#include <span>
#include <utility>
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

// What the canvas plots for a curve (After Effects' graph types). The Value Graph is the curve
// itself; the Speed Graph is its rate of change in value units per second, read off the sampler
// rather than re-derived from the handles, so it stays right for every interpolation the engine
// evaluates.
enum class GraphType : std::uint8_t { Value, Speed };

// Blender's Normalize: a curve's plotted quantity drawn scaled into -1...1 by its own extent. It
// is a DISPLAY transform only -- edits are converted back to real units before anything is
// written -- and it is the identity when normalization is off.
struct GraphNormalization final {
    double center = 0.0;
    double halfSpan = 1.0;

    [[nodiscard]] double toDisplay(double quantity) const noexcept {
        return (quantity - center) / halfSpan;
    }
    [[nodiscard]] double fromDisplay(double display) const noexcept {
        return center + display * halfSpan;
    }
    friend bool operator==(const GraphNormalization&, const GraphNormalization&) = default;
};

// The normalization that maps the extent of `quantities` onto -1...1. Degenerate input (empty,
// or every value equal) keeps a unit half-span around that value, so a flat curve draws as a
// flat line at 0 instead of dividing by zero.
[[nodiscard]] GraphNormalization normalizationFor(std::span<const double> quantities);

// One curve's complete vertical mapping: the window it is viewed through (in display units), the
// normalization (identity when off), and the band the window maps into. Every pixel the view
// paints or hit-tests for a curve, and every pointer delta it converts into an edit, goes through
// exactly this, so a normalized and a real-unit curve cannot be edited by different rules.
struct GraphLens final {
    GraphValueViewport viewport;
    GraphNormalization normalization;
    double top = 0.0;
    double height = 1.0;

    [[nodiscard]] double pixelFor(double quantity) const;
    [[nodiscard]] double quantityFor(double pixelY) const;
    // Real units per pixel of vertical pointer travel (positive: a pixel UP adds this much).
    [[nodiscard]] double quantityPerPixel() const;
};

// The window that shows -1...1 with the same padding auto-fit gives a real-unit curve.
[[nodiscard]] GraphValueViewport normalizedViewport();

// Shift-drag (After Effects): a key locks to time-only or value-only, whichever axis dominates the
// whole displacement so far, decided per move from the total rather than the last step.
enum class GraphDragAxis : std::uint8_t { Both, TimeOnly, ValueOnly };
[[nodiscard]] GraphDragAxis dragAxis(double dx, double dy, bool locked) noexcept;

// Shift on a handle: the handle's screen-space vector snapped to the nearest 45 degree direction,
// keeping its projected length. `forwardSign` is +1 for an outgoing handle and -1 for an incoming
// one: a handle never points back across its own key, so the backward directions are not offered.
struct GraphPixelVector final {
    double dx = 0.0;
    double dy = 0.0;
    friend bool operator==(const GraphPixelVector&, const GraphPixelVector&) = default;
};
[[nodiscard]] GraphPixelVector snapHandleAngle(GraphPixelVector vector, double forwardSign);

// The speed of a curve at `seconds`, from its own sampler. `keyTimes` are the curve's key times in
// seconds, ascending, and `interpolations[i]` is the outgoing mode of key i. Inside a segment
// the speed is a central difference of the sampled value, both probes kept inside that segment so
// a neighbouring Hold step can never leak into it; a held segment and the time outside the keys
// (where the curve extrapolates constant) have zero speed. `outgoing` picks the side of a key the
// speed is read on when `seconds` lands exactly on one.
[[nodiscard]] double curveSpeed(const std::function<double(double)>& sample,
                                std::span<const double> keyTimes,
                                std::span<const document::KeyframeInterpolation> interpolations,
                                double seconds, bool outgoing = true);

// The Speed Graph's handle model, exact in the Bezier the sampler evaluates: a handle at time
// fraction `t` of a segment lasting `duration` seconds with value offset `v` leaves (outgoing) or
// enters (incoming) its key at v / (t * duration) value units per second, sign-adjusted for the
// incoming side. The inverse writes a speed back as a value offset at the same influence.
[[nodiscard]] std::optional<double> handleSpeed(document::KeyframeHandle handle, double duration,
                                                bool outgoing) noexcept;
[[nodiscard]] double handleValueForSpeed(double speed, double handleTime, double duration,
                                         bool outgoing) noexcept;
// The smallest influence a Speed Graph handle drag writes (After Effects' 0.1%): at zero a handle's
// speed is undefined, so the drag stops just short of it.
inline constexpr double kMinimumHandleInfluence = 0.001;

// The handle that keeps a segment's straight-line tangent at its own key: a Speed Graph drag that
// turns a Linear segment into a Bezier pins the segment's far end with it, so only the dragged
// key's speed changes.
[[nodiscard]] document::KeyframeHandle linearTangentHandle(double startValue, double endValue,
                                                           bool outgoing) noexcept;

// The time range (seconds) and value window (display units) that frame `points` with padding, for
// Fit Selection / Fit All. A single point or a zero-length range gets a unit window around it.
// The narrowest time range a fit frames, in frames: a single selected key still gets context.
inline constexpr int kGraphFitMinimumFrames = 10;
struct GraphFrame final {
    double t0 = 0.0;
    double t1 = 1.0;
    GraphValueViewport values;
};
[[nodiscard]] std::optional<GraphFrame> frameFor(std::span<const GraphPoint> points,
                                                 double minimumSeconds);

} // namespace bloom::ui
