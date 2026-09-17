#pragma once
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <optional>

namespace bloom::runtime {
// Source time is (parent time - offset) * scale. Negative times hold zero. Hold and
// PingPong use the last frame start; Loop wraps at the half-open duration.
[[nodiscard]] std::optional<core::RationalTime>
mapCompositionTime(core::RationalTime time, double offset, double scale,
                   core::RationalTime duration, document::FrameRate rate, std::int64_t loopMode);
} // namespace bloom::runtime
