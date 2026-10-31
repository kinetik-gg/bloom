#include <bloom/document/keyframe_handles.hpp>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <variant>

namespace bloom::document {
namespace {

// Blender's `2.5614f` handle-length divisor (curve.cc, calchandleNurb_intern). It is the factor
// that makes a Catmull-Rom-like auto handle on a straight run land one third along its segment.
constexpr double kAutoHandleDivisor = 2.5614;
// Blender caps one side of a key at five times the other so a short segment next to a long one
// does not get a handle that dwarfs it.
constexpr double kAutoLengthRatio = 5.0;
// Two handles are "collinear" when the cross product of their vectors is within this fraction of
// the product of their lengths.
constexpr double kCollinearTolerance = 1e-9;

template <typename Keyframe> [[nodiscard]] double seconds(const Keyframe& key) noexcept {
    return key.time.toSeconds();
}

template <typename Keyframe>
[[nodiscard]] std::vector<AutoHandleKey> autoKeys(const std::vector<Keyframe>& keys) {
    std::vector<AutoHandleKey> result;
    result.reserve(keys.size());
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if constexpr (std::is_same_v<Keyframe, PathKeyframe>)
            result.push_back({seconds(keys[index]), static_cast<double>(index)});
        else
            result.push_back({seconds(keys[index]), keys[index].value});
    }
    return result;
}

template <typename Keyframe>
[[nodiscard]] const Keyframe* findKey(const std::vector<Keyframe>& keys, const KeyframeId id,
                                      std::size_t& index) noexcept {
    for (index = 0; index < keys.size(); ++index)
        if (keys[index].id == id)
            return &keys[index];
    return nullptr;
}

// The key's two handles as vectors in (seconds, value) space, both pointing away from the key.
struct HandleVectors {
    double outX, outY, inX, inY;
};

[[nodiscard]] bool collinear(const HandleVectors& vectors) noexcept {
    const double outLength = std::hypot(vectors.outX, vectors.outY);
    const double inLength = std::hypot(vectors.inX, vectors.inY);
    if (!(outLength > 0.0) || !(inLength > 0.0))
        return false;
    const double cross = vectors.outX * vectors.inY - vectors.outY * vectors.inX;
    const double dot = vectors.outX * vectors.inX + vectors.outY * vectors.inY;
    return dot < 0.0 && std::fabs(cross) <= kCollinearTolerance * outLength * inLength;
}

template <typename Keyframe>
[[nodiscard]] HandleVectors handleVectors(const std::vector<Keyframe>& keys, std::size_t index) {
    const double next = seconds(keys[index + 1]) - seconds(keys[index]);
    const double previous = seconds(keys[index]) - seconds(keys[index - 1]);
    const auto& key = keys[index];
    return {key.outgoingHandle.time * next, key.outgoingHandle.value,
            -key.incomingHandle.time * previous, key.incomingHandle.value};
}

template <typename Keyframe>
void settle(std::vector<Keyframe>& after, const std::vector<Keyframe>& before) {
    if (!hasNonFreeHandleMode(after))
        return;
    for (std::size_t index = 0; index < after.size(); ++index) {
        auto& key = after[index];
        std::size_t beforeIndex = 0;
        const Keyframe* old = findKey(before, key.id, beforeIndex);
        if (old == nullptr)
            continue;
        const bool outChanged = !(old->outgoingHandle == key.outgoingHandle);
        const bool inChanged = !(old->incomingHandle == key.incomingHandle);
        if (key.handleMode == KeyframeHandleMode::Auto &&
            old->handleMode == KeyframeHandleMode::Auto && (outChanged || inChanged))
            key.handleMode = KeyframeHandleMode::Aligned;
        if (key.handleMode != KeyframeHandleMode::Aligned || index == 0 ||
            index + 1 >= after.size())
            continue;
        const double next = seconds(after[index + 1]) - seconds(key);
        const double previous = seconds(key) - seconds(after[index - 1]);
        if (outChanged != inChanged) {
            if (outChanged)
                key.incomingHandle =
                    alignedHandle(key.outgoingHandle, next, key.incomingHandle, previous);
            else
                key.outgoingHandle =
                    alignedHandle(key.incomingHandle, previous, key.outgoingHandle, next);
        } else if (!outChanged && beforeIndex > 0 && beforeIndex + 1 < before.size()) {
            // A neighbor moved: the stored fractions now span different durations. A key that was
            // collinear stays collinear, following its outgoing handle.
            const double oldNext = seconds(before[beforeIndex + 1]) - seconds(*old);
            const double oldPrevious = seconds(*old) - seconds(before[beforeIndex - 1]);
            if ((oldNext != next || oldPrevious != previous) &&
                collinear(handleVectors(before, beforeIndex)))
                key.incomingHandle =
                    alignedHandle(key.outgoingHandle, next, key.incomingHandle, previous);
        }
    }
    const auto keys = autoKeys(after);
    bool computed = false;
    std::vector<AutoHandles> handles;
    for (std::size_t index = 0; index < after.size(); ++index) {
        if (after[index].handleMode != KeyframeHandleMode::Auto)
            continue;
        if (!computed) {
            handles = computeAutoClampedHandles(keys);
            computed = true;
        }
        if (handles[index].outgoing)
            after[index].outgoingHandle = *handles[index].outgoing;
        if (handles[index].incoming)
            after[index].incomingHandle = *handles[index].incoming;
    }
}

template <typename Curve> [[nodiscard]] const Curve* sameShape(const AnimationCurveRecord* before) {
    return before == nullptr ? nullptr : std::get_if<Curve>(before);
}

} // namespace

KeyframeHandle alignedHandle(const KeyframeHandle& anchor, const double anchorSegmentSeconds,
                             const KeyframeHandle& target,
                             const double targetSegmentSeconds) noexcept {
    const double anchorX = anchor.time * anchorSegmentSeconds;
    const double anchorLength = std::hypot(anchorX, anchor.value);
    if (!(anchorLength > 0.0) || !(targetSegmentSeconds > 0.0) || !std::isfinite(anchorLength))
        return target;
    const double targetLength = std::hypot(target.time * targetSegmentSeconds, target.value);
    const double scale = targetLength / anchorLength;
    KeyframeHandle result{scale * anchorX / targetSegmentSeconds, -scale * anchor.value};
    if (result.time > 1.0) {
        result.value /= result.time;
        result.time = 1.0;
    }
    return std::isfinite(result.time) && std::isfinite(result.value) ? result : target;
}

std::vector<AutoHandles> computeAutoClampedHandles(const std::span<const AutoHandleKey> keys) {
    std::vector<AutoHandles> result(keys.size());
    if (keys.size() < 2)
        return result;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const bool hasPrevious = index > 0;
        const bool hasNext = index + 1 < keys.size();
        const double x2 = keys[index].time;
        const double y2 = keys[index].value;
        // A missing neighbor is the mirror of the present one, as in Blender.
        const double x3 = hasNext ? keys[index + 1].time : 2.0 * x2 - keys[index - 1].time;
        const double y3 = hasNext ? keys[index + 1].value : 2.0 * y2 - keys[index - 1].value;
        const double x1 = hasPrevious ? keys[index - 1].time : 2.0 * x2 - x3;
        const double y1 = hasPrevious ? keys[index - 1].value : 2.0 * y2 - y3;

        const double dxA = x2 - x1;
        const double dyA = y2 - y1;
        const double dxB = x3 - x2;
        const double dyB = y3 - y2;
        double lengthA = dxA == 0.0 ? 1.0 : dxA;
        double lengthB = dxB == 0.0 ? 1.0 : dxB;
        const double tangentX = dxB / lengthB + dxA / lengthA;
        const double tangentY = dyB / lengthB + dyA / lengthA;
        const double length = tangentX * kAutoHandleDivisor;
        if (length == 0.0)
            continue;
        if (lengthA > kAutoLengthRatio * lengthB)
            lengthA = kAutoLengthRatio * lengthB;
        if (lengthB > kAutoLengthRatio * lengthA)
            lengthB = kAutoLengthRatio * lengthA;

        // Handle offsets from the key: left is (-a, d), right is (b, e).
        lengthA /= length;
        lengthB /= length;
        const double leftX = -tangentX * lengthA;
        double leftY = -tangentY * lengthA;
        const double rightX = tangentX * lengthB;
        double rightY = tangentY * lengthB;

        if (hasPrevious && hasNext) {
            bool leftViolates = false;
            bool rightViolates = false;
            const double below = keys[index - 1].value - y2;
            const double above = keys[index + 1].value - y2;
            if ((below <= 0.0 && above <= 0.0) || (below >= 0.0 && above >= 0.0)) {
                // An extremum (or a flat run): both handles go horizontal.
                leftY = 0.0;
                rightY = 0.0;
                leftViolates = true;
                rightViolates = true;
            } else {
                // A handle must not pass the value of the key on its own side.
                const double previousOffset = keys[index - 1].value - y2;
                const double nextOffset = keys[index + 1].value - y2;
                if (below <= 0.0) {
                    if (previousOffset > leftY) {
                        leftY = previousOffset;
                        leftViolates = true;
                    }
                    if (nextOffset < rightY) {
                        rightY = nextOffset;
                        rightViolates = true;
                    }
                } else {
                    if (previousOffset < leftY) {
                        leftY = previousOffset;
                        leftViolates = true;
                    }
                    if (nextOffset > rightY) {
                        rightY = nextOffset;
                        rightViolates = true;
                    }
                }
            }
            // One clamped side drags the other collinear, keeping its horizontal extent.
            if (leftViolates && !rightViolates && leftX != 0.0)
                rightY = leftY * rightX / leftX;
            else if (!leftViolates && rightViolates && rightX != 0.0)
                leftY = rightY * leftX / rightX;
        } else {
            // First and last key: constant extrapolation holds the value, so both are flat.
            leftY = 0.0;
            rightY = 0.0;
        }

        if (hasNext)
            result[index].outgoing = KeyframeHandle{rightX / (keys[index + 1].time - x2), rightY};
        if (hasPrevious)
            result[index].incoming = KeyframeHandle{-leftX / (x2 - keys[index - 1].time), leftY};
    }
    return result;
}

bool hasNonFreeHandleMode(const std::span<const ScalarKeyframe> keys) noexcept {
    return std::ranges::any_of(
        keys, [](const auto& key) { return key.handleMode != KeyframeHandleMode::Free; });
}

bool hasNonFreeHandleMode(const std::span<const PathKeyframe> keys) noexcept {
    return std::ranges::any_of(
        keys, [](const auto& key) { return key.handleMode != KeyframeHandleMode::Free; });
}

void settleKeyframeHandles(std::vector<ScalarKeyframe>& after,
                           const std::vector<ScalarKeyframe>& before) {
    settle(after, before);
}

void settleKeyframeHandles(std::vector<PathKeyframe>& after,
                           const std::vector<PathKeyframe>& before) {
    settle(after, before);
}

void settleAnimationCurveHandles(AnimationCurveRecord& after, const AnimationCurveRecord* before) {
    std::visit(
        [&](auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            const auto* old = sameShape<Curve>(before);
            if constexpr (kWholeValueAnimationCurve<Curve>) {
                static const decltype(curve.keyframes) none;
                settle(curve.keyframes, old != nullptr ? old->keyframes : none);
            } else {
                static const std::vector<ScalarKeyframe> none;
                for (std::size_t index = 0; index < curve.components.size(); ++index)
                    settle(curve.components[index].keyframes,
                           old != nullptr ? old->components[index].keyframes : none);
            }
        },
        after);
}

} // namespace bloom::document
