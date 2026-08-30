#include <bloom/ui/timeline_frame_math.hpp>

#include <bloom/core/frame_time_mapping.hpp>

namespace bloom::ui {

std::optional<core::RationalTime> frameTimeForIndex(const document::FrameRate frameRate,
                                                    const core::RationalTime duration,
                                                    const std::uint64_t index) noexcept {
    const auto created =
        core::FrameTimeMapping::create(duration, frameRate.numerator(), frameRate.denominator());
    if (!created.hasValue()) {
        return std::nullopt;
    }
    const auto mapped = created.value()->timeForFrame(index);
    if (!mapped.hasValue()) {
        return std::nullopt;
    }
    return *mapped.value();
}

std::optional<std::uint64_t> maxFrameIndex(const document::FrameRate frameRate,
                                           const core::RationalTime duration) noexcept {
    const auto created =
        core::FrameTimeMapping::create(duration, frameRate.numerator(), frameRate.denominator());
    if (!created.hasValue()) {
        return std::nullopt;
    }
    return created.value()->maximumFrameIndex();
}

std::optional<std::uint64_t> nearestFrameIndexForTime(const document::FrameRate frameRate,
                                                      const core::RationalTime duration,
                                                      const core::RationalTime time) noexcept {
    const auto created =
        core::FrameTimeMapping::create(duration, frameRate.numerator(), frameRate.denominator());
    if (!created.hasValue()) {
        return std::nullopt;
    }
    return created.value()->nearestFrameIndex(time);
}

} // namespace bloom::ui
