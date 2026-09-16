#include "timeline_graph_math.hpp"

#include <algorithm>
#include <cmath>

namespace bloom::ui {

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
    if (span <= 0.0)
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

} // namespace bloom::ui
