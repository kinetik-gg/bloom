#ifndef BLOOM_CORE_RATIONAL_INTERVAL_HPP
#define BLOOM_CORE_RATIONAL_INTERVAL_HPP

#include <bloom/core/rational_time.hpp>

#include <cstdint>
#include <type_traits>

namespace bloom::core {

enum class RationalIntervalError : std::uint8_t {
    None,
    DegenerateInterval,
    PositionOutsideInterval,
    InternalRangeFailure,
};

class RationalIntervalFactorResult;

// Returns the correctly rounded binary64 factor (position - start) / (end - start). The interval
// is closed and must be strictly increasing. Calculation uses exact fixed-width integer arithmetic;
// it does not convert any RationalTime operand to seconds.
[[nodiscard]] RationalIntervalFactorResult
rationalIntervalFactor(RationalTime position, RationalTime start, RationalTime end) noexcept;

class [[nodiscard]] RationalIntervalFactorResult final {
  public:
    [[nodiscard]] constexpr bool hasValue() const noexcept {
        return error_ == RationalIntervalError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr const double* value() const noexcept {
        return hasValue() ? &value_ : nullptr;
    }
    [[nodiscard]] constexpr RationalIntervalError error() const noexcept { return error_; }

  private:
    friend RationalIntervalFactorResult rationalIntervalFactor(RationalTime, RationalTime,
                                                               RationalTime) noexcept;

    constexpr RationalIntervalFactorResult(const double value,
                                           const RationalIntervalError error) noexcept
        : value_(value), error_(error) {}

    double value_ = 0.0;
    RationalIntervalError error_ = RationalIntervalError::None;
};

static_assert(std::is_trivially_copyable_v<RationalIntervalFactorResult>);

} // namespace bloom::core

#endif // BLOOM_CORE_RATIONAL_INTERVAL_HPP
