#pragma once
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/layer_time.hpp>
#include <optional>

namespace bloom::runtime {
// The one exact parent-to-child time mapping core (ADR 0024). Composition sources and layer-owned
// time both use it; arithmetic is checked rational with a nanosecond fallback where binary64
// operands overflow an exact intermediate, and a result outside representable time is nullopt.

// A content interval [0, duration) and the frame grid whose last frame start Hold and PingPong
// use. A one-frame extent holds its only frame.
struct TimeExtent final {
    core::RationalTime duration;
    document::FrameRate frameRate = document::FrameRate::framesPerSecond24();
    friend bool operator==(const TimeExtent&, const TimeExtent&) = default;
};

// The result of mapping a time into an extent: `present == false` is a None mode's absent content
// (no pixels, silence), never an arithmetic failure.
struct MappedTime final {
    bool present = true;
    core::RationalTime time{};
    friend bool operator==(const MappedTime&, const MappedTime&) = default;
};

// Applies the out-of-extent modes to an already-mapped time r. Before the extent (r < 0):
// None -> absent, Hold -> 0, Loop -> r mod E, PingPong -> reflection with period 2L. After it
// (Hold: r > L; the others: r >= E): None -> absent, Hold -> L, Loop -> r mod E, PingPong ->
// reflection. PingPong and Loop apply their wrap to every non-negative r, which is the historical
// composition-source rule and leaves times inside the extent unchanged.
[[nodiscard]] std::optional<MappedTime> wrapToExtent(core::RationalTime time,
                                                     const TimeExtent& extent,
                                                     document::LayerTimeMode before,
                                                     document::LayerTimeMode after);

// Layer time tau = (placementTime - startTime) * speed.
[[nodiscard]] std::optional<core::RationalTime>
mapLayerTime(core::RationalTime placementTime, core::RationalTime startTime,
             document::LayerSpeed speed);

// A finite binary64 seconds value (a Time Remap sample) as an exact rational time, or its
// nanosecond rounding when the exact value needs more than an int64 denominator.
[[nodiscard]] std::optional<core::RationalTime> secondsToTime(double seconds);

// A Time Remap result within one nanosecond of a frame start of `rate` is that frame start, so a
// binary64 remap that means "frame n" (an identity remap, 1/24 s) never selects frame n - 1.
[[nodiscard]] core::RationalTime snapRemappedTime(core::RationalTime time,
                                                  document::FrameRate rate);

// Source time is (parent time - offset) * scale. Negative times hold zero. Hold and
// PingPong use the last frame start; Loop wraps at the half-open duration. This is wrapToExtent
// with before = Hold and after = the stored loop mode.
[[nodiscard]] std::optional<core::RationalTime>
mapCompositionTime(core::RationalTime time, double offset, double scale,
                   core::RationalTime duration, document::FrameRate rate, std::int64_t loopMode);
} // namespace bloom::runtime
