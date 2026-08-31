#pragma once

#include <bloom/core/rational_time.hpp>

#include <cstdint>
#include <optional>
#include <type_traits>

namespace bloom::core {

enum class FrameTimeMappingError : std::uint8_t {
    None,
    InvalidRate,
    NonPositiveDuration,
    FrameIndexOverflow,
};

enum class FrameTimeError : std::uint8_t {
    None,
    FrameOutsideRange,
    TimeRepresentationOverflow,
};

class FrameTimeResult;
class FrameTimeMappingCreateResult;

// Maps a non-empty half-open composition interval [0, duration) onto frame indices using an exact
// positive rational frame rate. Creation fails when the complete valid index range cannot be
// represented by uint64_t. No calculation converts time to floating point.
class FrameTimeMapping final {
  public:
    [[nodiscard]] static FrameTimeMappingCreateResult
    create(RationalTime duration, std::uint32_t rateNumerator,
           std::uint32_t rateDenominator) noexcept;

    [[nodiscard]] constexpr RationalTime duration() const noexcept { return duration_; }
    [[nodiscard]] constexpr std::uint32_t rateNumerator() const noexcept { return rateNumerator_; }
    [[nodiscard]] constexpr std::uint32_t rateDenominator() const noexcept {
        return rateDenominator_;
    }
    [[nodiscard]] constexpr std::uint64_t maximumFrameIndex() const noexcept {
        return maximumFrameIndex_;
    }

    [[nodiscard]] FrameTimeResult timeForFrame(std::uint64_t frameIndex) const noexcept;

    // Clamps to the valid frame range and rounds to nearest; an exact halfway result selects the
    // greater frame index.
    [[nodiscard]] std::uint64_t nearestFrameIndex(RationalTime time) const noexcept;

    // Playback transport support (issue #105): the exact non-negative frame OFFSET covered by
    // `elapsedNanoseconds` nanoseconds of continuous playback measured from a fixed start, computed
    // as floor(elapsedNanoseconds / 1e9 * rateNumerator / rateDenominator) using the same checked
    // multiword arithmetic as timeForFrame()/nearestFrameIndex() -- never floating point. Unlike
    // nearestFrameIndex()'s round-to-nearest tie-to-greater SCRUB semantics, this FLOORS: at
    // elapsed E since a playback start, the frame showing is the one that began at-or-before E,
    // matching ordinary real-time playback (frame N spans the half-open interval
    // [N/rate, (N+1)/rate)). The result is a relative offset, not clamped to maximumFrameIndex() --
    // a playback controller adds it to a start frame index and wraps (modulo the frame count) for
    // looping; recomputing this from a fixed elapsed-since-start each tick (rather than
    // accumulating a running time) is what keeps repeated calls exact and drift-free.
    // std::nullopt only if the checked product cannot be represented in 64 bits -- unreachable for
    // realistic elapsed durations and frame rates, but kept checked rather than UB.
    [[nodiscard]] std::optional<std::uint64_t>
    frameOffsetForElapsedNanoseconds(std::uint64_t elapsedNanoseconds) const noexcept;

  private:
    friend class FrameTimeMappingCreateResult;

    constexpr FrameTimeMapping(const RationalTime duration, const std::uint32_t rateNumerator,
                               const std::uint32_t rateDenominator,
                               const std::uint64_t maximumFrameIndex) noexcept
        : duration_(duration), rateNumerator_(rateNumerator), rateDenominator_(rateDenominator),
          maximumFrameIndex_(maximumFrameIndex) {}

    RationalTime duration_{};
    std::uint32_t rateNumerator_ = 1;
    std::uint32_t rateDenominator_ = 1;
    std::uint64_t maximumFrameIndex_ = 0;
};

class [[nodiscard]] FrameTimeMappingCreateResult final {
  public:
    [[nodiscard]] constexpr bool hasValue() const noexcept {
        return error_ == FrameTimeMappingError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr const FrameTimeMapping* value() const& noexcept {
        return hasValue() ? &value_ : nullptr;
    }
    [[nodiscard]] constexpr const FrameTimeMapping* value() const&& = delete;
    [[nodiscard]] constexpr FrameTimeMappingError error() const noexcept { return error_; }

  private:
    friend FrameTimeMappingCreateResult FrameTimeMapping::create(RationalTime, std::uint32_t,
                                                                 std::uint32_t) noexcept;

    constexpr FrameTimeMappingCreateResult(const FrameTimeMapping value,
                                           const FrameTimeMappingError error) noexcept
        : value_(value), error_(error) {}

    FrameTimeMapping value_{RationalTime{}, 1, 1, 0};
    FrameTimeMappingError error_ = FrameTimeMappingError::None;
};

class [[nodiscard]] FrameTimeResult final {
  public:
    [[nodiscard]] constexpr bool hasValue() const noexcept {
        return error_ == FrameTimeError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr const RationalTime* value() const& noexcept {
        return hasValue() ? &value_ : nullptr;
    }
    [[nodiscard]] constexpr const RationalTime* value() const&& = delete;
    [[nodiscard]] constexpr FrameTimeError error() const noexcept { return error_; }

  private:
    friend FrameTimeResult FrameTimeMapping::timeForFrame(std::uint64_t) const noexcept;

    constexpr FrameTimeResult(const RationalTime value, const FrameTimeError error) noexcept
        : value_(value), error_(error) {}

    RationalTime value_{};
    FrameTimeError error_ = FrameTimeError::None;
};

static_assert(std::is_trivially_copyable_v<FrameTimeMapping>);
static_assert(std::is_trivially_copyable_v<FrameTimeMappingCreateResult>);
static_assert(std::is_trivially_copyable_v<FrameTimeResult>);

} // namespace bloom::core
