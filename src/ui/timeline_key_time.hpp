#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/layer_time.hpp>

#include <optional>

// ADR 0024 key clocks, Qt-free. A layer's own parameter keys are stored in layer time tau and its
// upstream keys in content time (equal to tau, see the Time Remap note below); the Timeline lives
// in composition (placement) time t. tau = (t - startTime) * speed and t = startTime + tau / speed,
// both exact. A null clock is the identity: no layer owns the parameter or its timing is identity.
//
// Time Remap: upstream keys are deliberately kept on the layer clock (tau), not inverted through
// the remap curve, so a remapped layer's upstream keys are drawn and edited at their layer-time
// placement.
namespace bloom::ui {

using KeyClock = std::optional<document::LayerTiming>;

// Composition time of a stored key time. nullopt only on 64-bit overflow.
[[nodiscard]] std::optional<core::RationalTime> compositionTimeOfKey(const KeyClock& clock,
                                                                     core::RationalTime keyTime);

// Stored key time of a composition time. nullopt only on 64-bit overflow.
[[nodiscard]] std::optional<core::RationalTime> keyTimeOfCompositionTime(const KeyClock& clock,
                                                                         core::RationalTime time);

// compositionTimeOfKey falling back to the unconverted time on overflow, for drawing paths.
[[nodiscard]] core::RationalTime projectedKeyTime(const KeyClock& clock, core::RationalTime keyTime);

} // namespace bloom::ui
