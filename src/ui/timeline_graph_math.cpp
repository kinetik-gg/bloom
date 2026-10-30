#include "timeline_graph_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace bloom::ui {
namespace {

// An extent this thin relative to its magnitude is floating-point noise around one value (a linear
// segment's sampled speed, say), not a range: treating it as a range would blow the noise up to
// fill the canvas.
[[nodiscard]] bool flatExtent(const double minimum, const double maximum) noexcept {
    constexpr double kRelativeFlatness = 1e-9;
    return !(maximum - minimum >
             kRelativeFlatness * std::max({1.0, std::abs(minimum), std::abs(maximum)}));
}

} // namespace

GraphValueViewport fitValueViewport(const std::span<const double> values) {
    double minimum = 0.0;
    double maximum = 0.0;
    bool seen = false;
    for (const double value : values) {
        if (!std::isfinite(value))
            continue;
        minimum = seen ? std::min(minimum, value) : value;
        maximum = seen ? std::max(maximum, value) : value;
        seen = true;
    }
    if (!seen)
        return {-1.0, 1.0};
    const double span = maximum - minimum;
    if (flatExtent(minimum, maximum))
        return {minimum - 1.0, maximum + 1.0};
    const double padding = span * kGraphViewportPadding;
    return {minimum - padding, maximum + padding};
}

double niceTickStep(const double span, const int targetCount) {
    if (!(span > 0.0) || targetCount <= 0)
        return 1.0;
    const double rough = span / targetCount;
    const double magnitude = std::pow(10.0, std::floor(std::log10(rough)));
    const double normalized = rough / magnitude;
    const double step = normalized <= 1.0   ? 1.0
                        : normalized <= 2.0 ? 2.0
                        : normalized <= 5.0 ? 5.0
                                            : 10.0;
    return step * magnitude;
}

std::vector<double> valueTicks(const GraphValueViewport& viewport, const double step) {
    std::vector<double> ticks;
    if (!(step > 0.0) || !(viewport.span() > 0.0))
        return ticks;
    const double first = std::ceil(viewport.minimum / step) * step;
    // A bounded loop rather than a while: a pathological step could otherwise spin forever on a
    // viewport an interaction made very wide.
    for (int index = 0; index < 1024; ++index) {
        const double value = first + index * step;
        if (value > viewport.maximum)
            break;
        ticks.push_back(value);
    }
    return ticks;
}

double pixelForValue(const GraphValueViewport& viewport, const double value, const double top,
                     const double height) {
    const double span = viewport.span();
    if (!(span > 0.0) || !(height > 0.0))
        return top;
    return top + ((viewport.maximum - value) / span) * height;
}

double valueForPixel(const GraphValueViewport& viewport, const double pixelY, const double top,
                     const double height) {
    const double span = viewport.span();
    if (!(span > 0.0) || !(height > 0.0))
        return viewport.maximum;
    return viewport.maximum - ((pixelY - top) / height) * span;
}

GraphValueViewport zoomValueViewport(const GraphValueViewport& viewport, const double factor,
                                     const double anchor) {
    if (!(factor > 0.0) || !std::isfinite(factor) || !std::isfinite(anchor))
        return viewport;
    const GraphValueViewport zoomed{anchor + (viewport.minimum - anchor) * factor,
                                    anchor + (viewport.maximum - anchor) * factor};
    return zoomed.span() > 0.0 && std::isfinite(zoomed.span()) ? zoomed : viewport;
}

GraphValueViewport panValueViewport(const GraphValueViewport& viewport, const double offset) {
    if (!std::isfinite(offset))
        return viewport;
    return {viewport.minimum + offset, viewport.maximum + offset};
}

GraphSegmentControls bezierControls(const GraphPoint start, const GraphPoint end,
                                    const document::KeyframeHandle outgoing,
                                    const document::KeyframeHandle incoming) {
    const double duration = end.time - start.time;
    return {start,
            {start.time + outgoing.time * duration, start.value + outgoing.value},
            {end.time - incoming.time * duration, end.value + incoming.value},
            end};
}

GraphNormalization normalizationFor(const std::span<const double> quantities) {
    // The bare extent, unpadded, so the curve's own extremes land exactly on -1 and 1.
    bool seen = false;
    double minimum = 0.0;
    double maximum = 0.0;
    for (const double value : quantities) {
        if (!std::isfinite(value))
            continue;
        minimum = seen ? std::min(minimum, value) : value;
        maximum = seen ? std::max(maximum, value) : value;
        seen = true;
    }
    if (!seen || flatExtent(minimum, maximum))
        return {seen ? (minimum + maximum) / 2.0 : 0.0, 1.0};
    return {(minimum + maximum) / 2.0, (maximum - minimum) / 2.0};
}

double GraphLens::pixelFor(const double quantity) const {
    return pixelForValue(viewport, normalization.toDisplay(quantity), top, height);
}

double GraphLens::quantityFor(const double pixelY) const {
    return normalization.fromDisplay(valueForPixel(viewport, pixelY, top, height));
}

double GraphLens::quantityPerPixel() const {
    return height > 0.0 ? viewport.span() / height * normalization.halfSpan : 0.0;
}

GraphValueViewport normalizedViewport() {
    constexpr std::array<double, 2> unit{-1.0, 1.0};
    return fitValueViewport(unit);
}

GraphDragAxis dragAxis(const double dx, const double dy, const bool locked) noexcept {
    if (!locked)
        return GraphDragAxis::Both;
    return std::abs(dx) >= std::abs(dy) ? GraphDragAxis::TimeOnly : GraphDragAxis::ValueOnly;
}

GraphPixelVector snapHandleAngle(const GraphPixelVector vector, const double forwardSign) {
    const double forward = forwardSign < 0.0 ? -1.0 : 1.0;
    const double diagonal = std::numbers::sqrt2 / 2.0;
    // The five directions a handle can take on its own side of the key, 45 degrees apart.
    const std::array<GraphPixelVector, 5> directions{{{forward, 0.0},
                                                      {forward * diagonal, -diagonal},
                                                      {forward * diagonal, diagonal},
                                                      {0.0, -1.0},
                                                      {0.0, 1.0}}};
    GraphPixelVector best{};
    double bestDot = 0.0;
    for (const auto& direction : directions) {
        const double dot = vector.dx * direction.dx + vector.dy * direction.dy;
        if (dot > bestDot) {
            bestDot = dot;
            best = {direction.dx * dot, direction.dy * dot};
        }
    }
    return best;
}

double curveSpeed(const std::function<double(double)>& sample,
                  const std::span<const double> keyTimes,
                  const std::span<const document::KeyframeInterpolation> interpolations,
                  const double seconds, const bool outgoing) {
    if (keyTimes.size() < 2 || !std::isfinite(seconds))
        return 0.0;
    // The segment that owns `seconds`: the last key at or before it (outgoing), or the last key
    // strictly before it (incoming), so a key's two sides can be read separately.
    std::size_t index = keyTimes.size();
    for (std::size_t candidate = 0; candidate + 1 < keyTimes.size(); ++candidate) {
        const bool starts =
            outgoing ? keyTimes[candidate] <= seconds : keyTimes[candidate] < seconds;
        const bool ends =
            outgoing ? seconds < keyTimes[candidate + 1] : seconds <= keyTimes[candidate + 1];
        if (starts && ends) {
            index = candidate;
            break;
        }
    }
    if (index >= keyTimes.size() || (index < interpolations.size() &&
                                     interpolations[index] == document::KeyframeInterpolation::Hold))
        return 0.0;
    const double start = keyTimes[index];
    const double end = keyTimes[index + 1];
    const double duration = end - start;
    if (!(duration > 0.0))
        return 0.0;
    // A probe a ten-thousandth of the segment either side, clamped into the segment: both probes
    // read this segment's own interpolation and nothing beyond it.
    const double step = duration * 1e-4;
    const double before = std::max(start, seconds - step);
    const double after = std::min(end, seconds + step);
    if (!(after > before))
        return 0.0;
    const double speed = (sample(after) - sample(before)) / (after - before);
    return std::isfinite(speed) ? speed : 0.0;
}

std::optional<double> handleSpeed(const document::KeyframeHandle handle, const double duration,
                                  const bool outgoing) noexcept {
    const double run = handle.time * duration;
    if (!(run > 0.0) || !std::isfinite(run))
        return std::nullopt;
    const double speed = (outgoing ? handle.value : -handle.value) / run;
    return std::isfinite(speed) ? std::optional<double>(speed) : std::nullopt;
}

double handleValueForSpeed(const double speed, const double handleTime, const double duration,
                           const bool outgoing) noexcept {
    const double value = speed * handleTime * duration;
    return outgoing ? value : -value;
}

document::KeyframeHandle linearTangentHandle(const double startValue, const double endValue,
                                             const bool outgoing) noexcept {
    const double third = (endValue - startValue) * document::kDefaultKeyframeHandleTime;
    return {document::kDefaultKeyframeHandleTime, outgoing ? third : -third};
}

std::optional<GraphFrame> frameFor(const std::span<const GraphPoint> points,
                                   const double minimumSeconds) {
    if (points.empty())
        return std::nullopt;
    double t0 = points.front().time;
    double t1 = t0;
    std::vector<double> values;
    values.reserve(points.size());
    for (const auto& point : points) {
        t0 = std::min(t0, point.time);
        t1 = std::max(t1, point.time);
        values.push_back(point.value);
    }
    if (!std::isfinite(t0) || !std::isfinite(t1))
        return std::nullopt;
    if (t1 - t0 < minimumSeconds) {
        const double middle = (t0 + t1) / 2.0;
        t0 = middle - minimumSeconds / 2.0;
        t1 = middle + minimumSeconds / 2.0;
    }
    const double padding = (t1 - t0) * kGraphViewportPadding;
    return GraphFrame{t0 - padding, t1 + padding, fitValueViewport(values)};
}

} // namespace bloom::ui
