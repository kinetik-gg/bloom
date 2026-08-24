#include <bloom/core/rational_interval.hpp>

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>
#include <string_view>

namespace {

using bloom::core::RationalIntervalError;
using bloom::core::rationalIntervalFactor;
using bloom::core::RationalIntervalFactorResult;
using bloom::core::RationalTime;

template <typename Result>
concept ExposesValueFromLvalue = requires(Result& result) { result.value(); };

template <typename Result>
concept ExposesValueFromConstLvalue = requires(const Result& result) { result.value(); };

template <typename Result>
concept ExposesValueFromRvalue = requires(Result result) { static_cast<Result&&>(result).value(); };

template <typename Result>
concept ExposesValueFromConstRvalue =
    requires(Result result) { static_cast<const Result&&>(result).value(); };

static_assert(ExposesValueFromLvalue<RationalIntervalFactorResult>);
static_assert(ExposesValueFromConstLvalue<RationalIntervalFactorResult>);
static_assert(!ExposesValueFromRvalue<RationalIntervalFactorResult>);
static_assert(!ExposesValueFromConstRvalue<RationalIntervalFactorResult>);

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] RationalTime time(const std::int64_t numerator, const std::int64_t denominator = 1) {
    const auto value = RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] bool hasValue(const bloom::core::RationalIntervalFactorResult& result,
                            const double expected) noexcept {
    return result && result.value() != nullptr && *result.value() == expected;
}

void testDomain(Expectations& expectations) {
    expectations.expect(rationalIntervalFactor(time(0), time(1), time(1)).error() ==
                                RationalIntervalError::DegenerateInterval &&
                            rationalIntervalFactor(time(0), time(2), time(1)).error() ==
                                RationalIntervalError::DegenerateInterval,
                        "empty and reversed intervals are rejected");
    expectations.expect(rationalIntervalFactor(time(-1), time(0), time(1)).error() ==
                                RationalIntervalError::PositionOutsideInterval &&
                            rationalIntervalFactor(time(2), time(0), time(1)).error() ==
                                RationalIntervalError::PositionOutsideInterval,
                        "positions outside the closed interval are rejected");
    expectations.expect(hasValue(rationalIntervalFactor(time(-3), time(-3), time(9)), 0.0) &&
                            hasValue(rationalIntervalFactor(time(9), time(-3), time(9)), 1.0),
                        "closed interval endpoints return exact factors");
}

void testKnownFactors(Expectations& expectations) {
    expectations.expect(
        hasValue(rationalIntervalFactor(time(1), time(0), time(4)), 0.25) &&
            hasValue(rationalIntervalFactor(time(-1), time(-2), time(2)), 0.25) &&
            hasValue(rationalIntervalFactor(time(1), time(0), time(3)), 0x1.5555555555555p-2) &&
            hasValue(rationalIntervalFactor(time(2), time(0), time(3)), 0x1.5555555555555p-1),
        "ordinary and signed intervals round to their specified binary64 values");
}

void testHalfwayTies(Expectations& expectations) {
    constexpr std::int64_t denominator = std::int64_t{1} << 54U;
    constexpr std::int64_t halfSignificand = std::int64_t{1} << 53U;
    const auto evenDown =
        rationalIntervalFactor(time(halfSignificand + 1, denominator), time(0), time(1));
    const auto evenUp =
        rationalIntervalFactor(time(halfSignificand + 3, denominator), time(0), time(1));

    expectations.expect(hasValue(evenDown, 0.5),
                        "a halfway value rounds toward the even lower significand");
    expectations.expect(evenUp && evenUp.value() != nullptr &&
                            std::bit_cast<std::uint64_t>(*evenUp.value()) ==
                                std::bit_cast<std::uint64_t>(0.5) + 2U,
                        "a halfway value rounds toward the even upper significand");
}

void testExtremeRatio(Expectations& expectations) {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto result = rationalIntervalFactor(time(1, maximum), time(0), time(maximum));
    expectations.expect(
        hasValue(result, 0x1.0000000000000p-126),
        "an extreme valid rational factor is rounded without conversion through seconds");
}

void testGeneralRationalIntervals(Expectations& expectations) {
    expectations.expect(
        hasValue(rationalIntervalFactor(time(7, 11), time(-13, 17), time(19, 23)),
                 0x1.c2efe94047d6bp-1) &&
            hasValue(rationalIntervalFactor(time(-5, 7), time(-101, 103), time(11, 13)),
                     0x1.2a8d526205ae8p-3) &&
            hasValue(rationalIntervalFactor(time(123456789, 1000000007),
                                            time(-987654321, 2147483647),
                                            time(777777777, 1000000009)),
                     0x1.e2a62b07b2d1cp-2),
        "unrelated denominators preserve exact cross-rational rounding");

    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    expectations.expect(
        hasValue(rationalIntervalFactor(time(1, maximum), time(-maximum), time(maximum)), 0.5) &&
            hasValue(rationalIntervalFactor(time(-1, maximum), time(-maximum), time(1, maximum)),
                     1.0),
        "extreme signed intervals may correctly round an interior factor to an endpoint");
}

} // namespace

int main() {
    Expectations expectations;
    testDomain(expectations);
    testKnownFactors(expectations);
    testHalfwayTies(expectations);
    testExtremeRatio(expectations);
    testGeneralRationalIntervals(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
