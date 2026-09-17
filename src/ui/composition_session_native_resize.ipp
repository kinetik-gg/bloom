// NATIVE SIZE changes source coordinates, never the sampled LOCAL TRANSFORM Scale.
using Native = TransformInteraction::NativeKind;
if (delta.isNull())
    break;
if (!state.gesture.bounds || state.native.empty()) {
    cancelTransformInteraction();
    return;
}
const auto& bounds = state.gesture.bounds->local;
const auto handle = state.gesture.handle;
const bool xActive = handle != 1 && handle != 5;
const bool yActive = handle != 3 && handle != 7;
const bool left = handle == 0 || handle == 6 || handle == 7;
const bool top = handle == 0 || handle == 1 || handle == 2;
const double signX = left ? -1 : 1, signY = top ? -1 : 1;
const auto nativeDelta = linear.inverted().map(delta);
QPointF base(bounds.right - bounds.left, bounds.bottom - bounds.top);
if (state.nativeKind == Native::Size || state.nativeKind == Native::BoxText)
    base = transformPoint(std::get<document::Vec2d>(state.native[0].second));
else if (state.nativeKind == Native::Solid)
    base = {std::get<double>(state.native[0].second), std::get<double>(state.native[1].second)};
if (state.nativeKind == Native::Line) {
    const auto start = std::get<document::Vec2d>(state.native[0].second);
    const auto end = std::get<document::Vec2d>(state.native[1].second);
    base = {std::abs(end.x - start.x), std::abs(end.y - start.y)};
}
const double multiple = modifiers.alt ? 2 : 1;
QPointF next(base.x() + (xActive ? signX * nativeDelta.x() * multiple : 0),
             base.y() + (yActive ? signY * nativeDelta.y() * multiple : 0));
if (modifiers.shift || state.nativeKind == Native::PointText) {
    const QPointF arm(xActive ? signX * base.x() : 0, yActive ? signY * base.y() : 0);
    const auto length = QPointF::dotProduct(arm, arm);
    const auto factor =
        length > 0 ? 1 + multiple * QPointF::dotProduct(nativeDelta, arm) / length : 1;
    next = base * std::max(factor, 1.0 / std::max(base.x(), base.y()));
}
const double minimum = state.nativeKind == Native::Line ? 0.0 : 1.0;
next.setX(std::clamp(next.x(), minimum, 1'000'000.0));
next.setY(std::clamp(next.y(), minimum, 1'000'000.0));
// Staged exactly like put() above, never written straight into state.overrides: the whole update
// is offered or dropped as one unit at the end of updateTransformInteraction(), so a native
// override cannot outlive a dropped frame, accumulate across updates, or be overwritten by the
// staged assignment that follows this switch. It also puts the native geometry a gesture authors
// -- a size, a font size, line endpoints, path anchors -- behind the same finiteness check every
// other offered value passes.
const auto putNative = [&](std::size_t i, const auto& value) {
    staged.push_back({state.baseRevision, state.native[i].first, value});
};
if (state.nativeKind == Native::PointText) {
    const auto size = std::get<double>(state.native[0].second);
    const auto nextSize =
        std::clamp(size * next.x() / base.x(), 0.01, document::kMaximumTextSizePixels);
    next = base * (nextSize / size);
    putNative(0, nextSize);
} else if (state.nativeKind == Native::Size || state.nativeKind == Native::BoxText) {
    putNative(0, transformValue(next));
} else if (state.nativeKind == Native::Solid) {
    putNative(0, next.x());
    putNative(1, next.y());
} else {
    const auto map = [&](document::Vec2d point) -> document::Vec2d {
        return {bounds.left + (point.x - bounds.left) * next.x() / base.x(),
                bounds.top + (point.y - bounds.top) * next.y() / base.y()};
    };
    if (state.nativeKind == Native::Line) {
        const auto start = std::get<document::Vec2d>(state.native[0].second);
        const auto end = std::get<document::Vec2d>(state.native[1].second);
        const double x = std::min(start.x, end.x), y = std::min(start.y, end.y);
        putNative(0, document::Vec2d{x + (start.x > end.x ? next.x() : 0),
                                     y + (start.y > end.y ? next.y() : 0)});
        putNative(1, document::Vec2d{x + (start.x <= end.x ? next.x() : 0),
                                     y + (start.y <= end.y ? next.y() : 0)});
    } else {
        auto path = std::get<document::PathValue>(state.native[0].second);
        for (auto& anchor : path.anchors) {
            anchor.point = map(anchor.point);
            if (anchor.inHandle)
                anchor.inHandle = map(*anchor.inHandle);
            if (anchor.outHandle)
                anchor.outHandle = map(*anchor.outHandle);
        }
        putNative(0, path);
    }
}
const auto change = next - base;
const QPointF offset(modifiers.alt || !xActive ? 0 : signX* change.x() / 2,
                     modifiers.alt || !yActive ? 0 : signY* change.y() / 2);
put(0, transformValue(position + linear.map(offset)));
