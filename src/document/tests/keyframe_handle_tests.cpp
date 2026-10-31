// Keyframe handle modes (document 1.32): Blender's Auto Clamped handles against reference values
// produced by Blender itself, Aligned collinearity after edits, Auto recomputation when a neighbor
// changes, the Auto-to-Aligned conversion, Free sequences left bit-for-bit untouched, and the
// classification of the mode as presentation.
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/evaluation_inputs.hpp>
#include <bloom/document/keyframe_handles.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace bloom::document;
using bloom::core::RationalTime;

int failures = 0;
void expect(const bool condition, const std::string_view message,
            const std::source_location location = std::source_location::current()) {
    if (condition)
        return;
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
}

RationalTime at(const std::int64_t seconds) { return RationalTime::fromInteger(seconds); }

bool near(const double actual, const double expected, const double tolerance = 2e-5) {
    return std::fabs(actual - expected) <= tolerance * std::max(1.0, std::fabs(expected));
}

bool bitEqual(const KeyframeHandle& a, const KeyframeHandle& b) {
    return std::bit_cast<std::uint64_t>(a.time) == std::bit_cast<std::uint64_t>(b.time) &&
           std::bit_cast<std::uint64_t>(a.value) == std::bit_cast<std::uint64_t>(b.value);
}

#include "keyframe_handle_blender_reference.inc"

ScalarKeyframe key(const std::uint64_t id, const std::int64_t seconds, const double value,
                   const KeyframeHandleMode mode = KeyframeHandleMode::Free) {
    ScalarKeyframe result;
    result.id = KeyframeId::fromRaw(id);
    result.time = at(seconds);
    result.value = value;
    result.handleMode = mode;
    return result;
}

// A scalar curve at id 1 holding `layout`'s keys, every key in `mode`, built key by key through
// the store so the settle hook runs exactly as it does for a command.
AnimationCurveStore storeFor(const ReferenceLayout& layout, const KeyframeHandleMode mode) {
    AnimationCurveStore store;
    const auto curve = AnimationCurveId::fromRaw(1);
    for (std::size_t index = 0; index < layout.keys.size(); ++index) {
        const auto& reference = layout.keys[index];
        const auto added =
            key(index + 1, static_cast<std::int64_t>(reference.time), reference.value, mode);
        if (index == 0)
            expect(store.insert(ScalarAnimationCurve{curve, {added}}), "curve inserts");
        else
            expect(store.insertKeyframe(curve, added), "key inserts");
    }
    return store;
}

// The key's two handle vectors in absolute (seconds, value) space, pointing away from the key.
struct Vectors {
    double outX, outY, inX, inY;
};
Vectors vectorsOf(const std::vector<ScalarKeyframe>& keys, const std::size_t index) {
    const double next = keys[index + 1].time.toSeconds() - keys[index].time.toSeconds();
    const double previous = keys[index].time.toSeconds() - keys[index - 1].time.toSeconds();
    return {keys[index].outgoingHandle.time * next, keys[index].outgoingHandle.value,
            -keys[index].incomingHandle.time * previous, keys[index].incomingHandle.value};
}
bool collinearThrough(const Vectors& v) {
    const double cross = v.outX * v.inY - v.outY * v.inX;
    const double scale = std::hypot(v.outX, v.outY) * std::hypot(v.inX, v.inY);
    return scale == 0.0 || std::fabs(cross) <= 1e-9 * scale;
}

void testBlenderReference() {
    for (const auto& layout : kBlenderAutoClamped) {
        // The pure computation.
        std::vector<AutoHandleKey> keys;
        for (const auto& reference : layout.keys)
            keys.push_back({reference.time, reference.value});
        const auto computed = computeAutoClampedHandles(keys);
        // And through the store, which is how a command reaches it.
        const auto store = storeFor(layout, KeyframeHandleMode::Auto);
        const auto* curve = store.findScalar(AnimationCurveId::fromRaw(1));
        expect(curve != nullptr && curve->keyframes.size() == layout.keys.size(),
               "the reference curve is stored");
        if (curve == nullptr)
            continue;
        for (std::size_t index = 0; index < layout.keys.size(); ++index) {
            const auto& reference = layout.keys[index];
            const std::string where = std::string(layout.name) + " key " + std::to_string(index);
            expect(computed[index].outgoing.has_value() == reference.hasOut &&
                       computed[index].incoming.has_value() == reference.hasIn,
                   where + ": a side has a handle exactly when it has a segment");
            if (reference.hasOut && computed[index].outgoing) {
                expect(near(computed[index].outgoing->time, reference.outTime) &&
                           near(computed[index].outgoing->value, reference.outValue),
                       where + ": outgoing handle matches Blender");
                expect(bitEqual(curve->keyframes[index].outgoingHandle, *computed[index].outgoing),
                       where + ": the store settles to the computed outgoing handle");
            }
            if (reference.hasIn && computed[index].incoming) {
                expect(near(computed[index].incoming->time, reference.inTime) &&
                           near(computed[index].incoming->value, reference.inValue),
                       where + ": incoming handle matches Blender");
                expect(bitEqual(curve->keyframes[index].incomingHandle, *computed[index].incoming),
                       where + ": the store settles to the computed incoming handle");
            }
            expect(isValidKeyframeHandle(curve->keyframes[index].outgoingHandle) &&
                       isValidKeyframeHandle(curve->keyframes[index].incomingHandle),
                   where + ": computed handles are valid");
        }
    }
}

// The extrema never overshoot: sampled as the Bezier the sampler reads, an Auto curve through a
// peak stays within its keys' range.
void testClampingAtExtrema() {
    const auto peak =
        std::ranges::find(kBlenderAutoClamped, std::string_view("Peak"), &ReferenceLayout::name);
    const auto store = storeFor(*peak, KeyframeHandleMode::Auto);
    const auto& keys = store.findScalar(AnimationCurveId::fromRaw(1))->keyframes;
    expect(keys[1].outgoingHandle.value == 0.0 && keys[1].incomingHandle.value == 0.0,
           "the handles of a peak are horizontal");
    expect(keys[0].outgoingHandle.value == 0.0 && keys[2].incomingHandle.value == 0.0,
           "the first and last key are flat");
}

void testFreeIsUntouched() {
    AnimationCurveStore store;
    const auto curve = AnimationCurveId::fromRaw(1);
    auto first = key(1, 0, 0.0);
    first.outgoingHandle = {0.25, 0.5};
    auto second = key(2, 2, 1.0);
    second.incomingHandle = {0.5, -0.125};
    expect(store.insert(ScalarAnimationCurve{curve, {first, second}}), "free curve inserts");
    auto third = key(3, 4, 3.0);
    expect(store.insertKeyframe(curve, third), "third key inserts");
    auto moved = second;
    moved.value = 7.0;
    expect(store.updateKeyframe(curve, moved), "value edit");
    const auto& keys = store.findScalar(curve)->keyframes;
    expect(bitEqual(keys[0].outgoingHandle, {0.25, 0.5}) &&
               bitEqual(keys[1].incomingHandle, {0.5, -0.125}) &&
               isDefaultKeyframeHandle(keys[2].outgoingHandle),
           "edits to a Free sequence never touch a handle");
    expect(store.eraseKeyframe(curve, KeyframeId::fromRaw(3)), "erase");
    expect(bitEqual(store.findScalar(curve)->keyframes[0].outgoingHandle, {0.25, 0.5}),
           "nor does a delete");
}

void testAlignedCollinearity() {
    AnimationCurveStore store;
    const auto curve = AnimationCurveId::fromRaw(1);
    expect(store.insert(
               ScalarAnimationCurve{curve, {key(1, 0, 0.0), key(2, 4, 2.0), key(3, 10, 1.0)}}),
           "curve inserts");
    // Switch the middle key to Aligned: nothing moves.
    auto middle = store.findScalar(curve)->keyframes[1];
    const auto freeOut = middle.outgoingHandle;
    const auto freeIn = middle.incomingHandle;
    middle.handleMode = KeyframeHandleMode::Aligned;
    expect(store.updateKeyframe(curve, middle), "mode edit");
    {
        const auto& stored = store.findScalar(curve)->keyframes[1];
        expect(bitEqual(stored.outgoingHandle, freeOut) && bitEqual(stored.incomingHandle, freeIn),
               "switching to Aligned moves no handle");
    }
    // Drag the outgoing handle: the incoming one follows, collinear, keeping its length.
    const auto before = vectorsOf(store.findScalar(curve)->keyframes, 1);
    middle = store.findScalar(curve)->keyframes[1];
    middle.outgoingHandle = {0.4, 0.75};
    expect(store.updateKeyframe(curve, middle), "outgoing drag");
    auto keys = store.findScalar(curve)->keyframes;
    auto after = vectorsOf(keys, 1);
    expect(collinearThrough(after) && after.outX * after.inX + after.outY * after.inY < 0.0,
           "the incoming handle is collinear and opposite after an outgoing edit");
    expect(near(std::hypot(after.inX, after.inY), std::hypot(before.inX, before.inY), 1e-12),
           "the incoming handle keeps its length");
    expect(bitEqual(keys[1].outgoingHandle, {0.4, 0.75}), "the dragged handle is exactly as set");
    // Drag the incoming handle: the outgoing one follows.
    middle = keys[1];
    middle.incomingHandle = {0.2, -0.9};
    expect(store.updateKeyframe(curve, middle), "incoming drag");
    keys = store.findScalar(curve)->keyframes;
    const auto dragged = vectorsOf(keys, 1);
    expect(collinearThrough(dragged), "the outgoing handle follows an incoming edit");
    expect(bitEqual(keys[1].incomingHandle, {0.2, -0.9}), "the dragged incoming handle is as set");
    expect(near(std::hypot(dragged.outX, dragged.outY), std::hypot(after.outX, after.outY), 1e-12),
           "the outgoing handle keeps its length");
    // Moving the next key changes the segment duration; the collinear key stays collinear.
    auto last = keys[2];
    last.time = at(14);
    expect(store.updateKeyframe(curve, last), "neighbor move");
    keys = store.findScalar(curve)->keyframes;
    expect(collinearThrough(vectorsOf(keys, 1)),
           "a collinear Aligned key stays collinear when a neighbor moves");
    expect(keys[1].handleMode == KeyframeHandleMode::Aligned, "and stays Aligned");
}

void testAutoFollowsNeighbors() {
    const auto monotone = std::ranges::find(kBlenderAutoClamped, std::string_view("Monotone"),
                                            &ReferenceLayout::name);
    auto store = storeFor(*monotone, KeyframeHandleMode::Auto);
    const auto curve = AnimationCurveId::fromRaw(1);
    const auto beforeHandle = store.findScalar(curve)->keyframes[1].outgoingHandle;
    // Raise the last key: the third key's handles must change, and the result must equal a curve
    // built with that value from scratch.
    auto last = store.findScalar(curve)->keyframes[3];
    last.value = 30.0;
    expect(store.updateKeyframe(curve, last), "neighbor value edit");
    ReferenceLayout changed = *monotone;
    changed.keys[3].value = 30.0;
    const auto expected = storeFor(changed, KeyframeHandleMode::Auto);
    expect(store.findScalar(curve)->keyframes == expected.findScalar(curve)->keyframes,
           "an Auto key follows a neighbor's value change, matching a fresh computation");
    expect(!bitEqual(store.findScalar(curve)->keyframes[2].outgoingHandle, beforeHandle),
           "the recomputed handle differs");
    // Move a key in time.
    auto second = store.findScalar(curve)->keyframes[1];
    second.time = at(8);
    expect(store.updateKeyframe(curve, second), "neighbor time edit");
    changed.keys[1].time = 8.0;
    expect(store.findScalar(curve)->keyframes ==
               storeFor(changed, KeyframeHandleMode::Auto).findScalar(curve)->keyframes,
           "an Auto key follows a neighbor's time change");
    // Delete a key.
    expect(store.eraseKeyframe(curve, KeyframeId::fromRaw(2)), "erase");
    ReferenceLayout shorter = changed;
    shorter.keys.erase(shorter.keys.begin() + 1);
    const auto freshShorter = storeFor(shorter, KeyframeHandleMode::Auto);
    const auto& survivors = store.findScalar(curve)->keyframes;
    const auto& fresh = freshShorter.findScalar(curve)->keyframes;
    bool same = survivors.size() == fresh.size();
    for (std::size_t index = 0; same && index < fresh.size(); ++index)
        same = survivors[index].value == fresh[index].value &&
               bitEqual(survivors[index].outgoingHandle, fresh[index].outgoingHandle) &&
               bitEqual(survivors[index].incomingHandle, fresh[index].incomingHandle);
    expect(same, "an Auto key follows a deleted neighbor");
}

void testAutoDragConvertsToAligned() {
    const auto peak = std::ranges::find(kBlenderAutoClamped, std::string_view("Monotone"),
                                        &ReferenceLayout::name);
    auto store = storeFor(*peak, KeyframeHandleMode::Auto);
    const auto curve = AnimationCurveId::fromRaw(1);
    auto middle = store.findScalar(curve)->keyframes[1];
    middle.outgoingHandle = {0.5, 3.0};
    expect(store.updateKeyframe(curve, middle), "drag an Auto handle");
    const auto keys = store.findScalar(curve)->keyframes;
    expect(keys[1].handleMode == KeyframeHandleMode::Aligned,
           "dragging an Auto handle converts the key to Aligned");
    expect(bitEqual(keys[1].outgoingHandle, {0.5, 3.0}) && collinearThrough(vectorsOf(keys, 1)),
           "the dragged handle holds and the other follows");
    // The neighbor edit no longer moves the converted key's handles.
    auto last = keys[3];
    last.value = 99.0;
    const auto keptOut = keys[1].outgoingHandle;
    expect(store.updateKeyframe(curve, last), "neighbor edit");
    expect(bitEqual(store.findScalar(curve)->keyframes[1].outgoingHandle, keptOut),
           "an Aligned key no longer follows its neighbors");
}

void testPathKeys() {
    AnimationCurveStore store;
    const auto curve = AnimationCurveId::fromRaw(1);
    PathValue path;
    PathKeyframe first;
    first.id = KeyframeId::fromRaw(1);
    first.time = at(0);
    first.value = path;
    PathKeyframe second = first;
    second.id = KeyframeId::fromRaw(2);
    second.time = at(2);
    second.handleMode = KeyframeHandleMode::Auto;
    PathKeyframe third = first;
    third.id = KeyframeId::fromRaw(3);
    third.time = at(4);
    expect(store.insert(PathAnimationCurve{curve, {first, second}}), "path curve inserts");
    expect(store.insertKeyframe(curve, third), "path key inserts");
    const auto& keys = store.findPath(curve)->keyframes;
    // Progress runs 0 -> 1 per segment, so an equal-spaced interior key's handle runs along the
    // chord: time and value offset are both 1 / 2.5614.
    expect(near(keys[1].outgoingHandle.time, 1.0 / 2.5614) &&
               near(keys[1].outgoingHandle.value, 1.0 / 2.5614) &&
               near(keys[1].incomingHandle.value, -1.0 / 2.5614),
           "an Auto Path key runs its handles along the chord");
}

void testPresentationClassification() {
    ScalarKeyframe free = key(1, 0, 1.0);
    ScalarKeyframe aligned = free;
    aligned.handleMode = KeyframeHandleMode::Aligned;
    expect(!(free == aligned) && evaluationView(free) == evaluationView(aligned),
           "the mode is the only difference evaluationView() removes");
    aligned.outgoingHandle = {0.5, 0.0};
    expect(evaluationView(free) != evaluationView(aligned),
           "a stored handle value stays an evaluation input");
    expect(keyframeHandleModeToken(KeyframeHandleMode::Auto) == "auto" &&
               keyframeHandleModeFromToken("aligned") == KeyframeHandleMode::Aligned &&
               !keyframeHandleModeFromToken("smooth") &&
               isValidKeyframeHandleMode(KeyframeHandleMode::Auto) &&
               !isValidKeyframeHandleMode(static_cast<KeyframeHandleMode>(3)),
           "tokens and validity");
    AnimationCurveStore store;
    auto invalid = key(1, 0, 0.0);
    invalid.handleMode = static_cast<KeyframeHandleMode>(7);
    expect(!store.insert(ScalarAnimationCurve{AnimationCurveId::fromRaw(1), {invalid}}),
           "an unknown mode cannot enter the store");
}

} // namespace

int main() {
    testBlenderReference();
    testClampingAtExtrema();
    testFreeIsUntouched();
    testAlignedCollinearity();
    testAutoFollowsNeighbors();
    testAutoDragConvertsToAligned();
    testPathKeys();
    testPresentationClassification();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
