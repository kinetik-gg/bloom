#include <bloom/project/canonical_decimal.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using bloom::project::CanonicalDecimalError;
using bloom::project::CanonicalDecimalField;

template <typename Result>
void expectFailure(Expectations& expectations, const Result& result,
                   const CanonicalDecimalError error, const CanonicalDecimalField field,
                   const std::string_view message) {
    expectations.expect(!result && result.value() == nullptr && result.error() == error &&
                            result.field() == field,
                        message);
}

template <typename Value>
concept ExposesRvalueResultPointer =
    requires { std::declval<const bloom::project::CanonicalDecimalResult<Value>&&>().value(); };

template <typename Value>
concept ExposesRvalueTextView = requires { std::declval<const Value&&>().view(); };

template <typename Value>
concept ExposesRvalueTextData = requires { std::declval<const Value&&>().data(); };

static_assert(!ExposesRvalueResultPointer<std::uint64_t>);
static_assert(!ExposesRvalueResultPointer<bloom::core::RationalTime>);
static_assert(!ExposesRvalueTextView<bloom::project::CanonicalDecimalText>);
static_assert(!ExposesRvalueTextData<bloom::project::CanonicalDecimalText>);
static_assert(!ExposesRvalueTextView<bloom::project::CanonicalFloat64Text>);
static_assert(!ExposesRvalueTextData<bloom::project::CanonicalFloat64Text>);

void testObjectIdsAndHighWater(Expectations& expectations) {
    const auto one = bloom::project::parseCanonicalObjectId("1");
    const auto maximum = bloom::project::parseCanonicalObjectId("18446744073709551615");
    expectations.expect(one && *one.value() == 1 && maximum &&
                            *maximum.value() == std::numeric_limits<std::uint64_t>::max(),
                        "object IDs accept their exact nonzero uint64 boundaries");
    expectFailure(expectations, bloom::project::parseCanonicalObjectId("0"),
                  CanonicalDecimalError::ZeroNotAllowed, CanonicalDecimalField::Value,
                  "an object ID reports its forbidden zero separately");
    expectFailure(expectations, bloom::project::parseCanonicalObjectId("18446744073709551616"),
                  CanonicalDecimalError::OutOfRange, CanonicalDecimalField::Value,
                  "an object ID rejects uint64 overflow");

    const auto fresh = bloom::project::parseCanonicalAllocatorHighWater("0");
    expectations.expect(fresh && *fresh.value() == 0,
                        "an allocator high-water accepts canonical zero");
    expectFailure(expectations, bloom::project::parseCanonicalAllocatorHighWater("00"),
                  CanonicalDecimalError::InvalidLexicalForm, CanonicalDecimalField::Value,
                  "an allocator high-water rejects leading zeroes");

    for (const std::string_view invalid :
         {"", "+1", "-1", "01", " 1", "1 ", "1.0", "1e0", "a", "1a"}) {
        expectFailure(expectations, bloom::project::parseCanonicalAllocatorHighWater(invalid),
                      CanonicalDecimalError::InvalidLexicalForm, CanonicalDecimalField::Value,
                      "unsigned decimal syntax rejects noncanonical spelling");
    }

    const std::string huge(10'000, '9');
    expectFailure(expectations, bloom::project::parseCanonicalAllocatorHighWater(huge),
                  CanonicalDecimalError::OutOfRange, CanonicalDecimalField::Value,
                  "a huge all-digit input reports range without allocating in the parser");
}

void testSignedIntegers(Expectations& expectations) {
    const auto minimum = bloom::project::parseCanonicalInt64("-9223372036854775808");
    const auto maximum = bloom::project::parseCanonicalInt64("9223372036854775807");
    const auto zero = bloom::project::parseCanonicalInt64("0");
    expectations.expect(minimum && *minimum.value() == std::numeric_limits<std::int64_t>::min() &&
                            maximum &&
                            *maximum.value() == std::numeric_limits<std::int64_t>::max() && zero &&
                            *zero.value() == 0,
                        "signed decimal parsing accepts int64 extrema and canonical zero");
    expectFailure(expectations, bloom::project::parseCanonicalInt64("-9223372036854775809"),
                  CanonicalDecimalError::OutOfRange, CanonicalDecimalField::Value,
                  "signed decimal parsing rejects negative overflow");
    expectFailure(expectations, bloom::project::parseCanonicalInt64("9223372036854775808"),
                  CanonicalDecimalError::OutOfRange, CanonicalDecimalField::Value,
                  "signed decimal parsing rejects positive overflow");

    for (const std::string_view invalid :
         {"", "+0", "+1", "-0", "00", "-00", "01", "-01", "--1", " 0", "0 ", "1.0", "1e0"}) {
        expectFailure(expectations, bloom::project::parseCanonicalInt64(invalid),
                      CanonicalDecimalError::InvalidLexicalForm, CanonicalDecimalField::Value,
                      "signed decimal syntax rejects invalid signs and leading zeroes");
    }
}

void testJsonIntegers(Expectations& expectations) {
    const auto zero = bloom::project::parseCanonicalJsonUInt32("0", 10);
    const auto ceiling = bloom::project::parseCanonicalJsonUInt32("10", 10);
    expectations.expect(zero && *zero.value() == 0 && ceiling && *ceiling.value() == 10,
                        "JSON uint32 parsing honors an inclusive caller ceiling");
    expectFailure(expectations, bloom::project::parseCanonicalJsonUInt32("11", 10),
                  CanonicalDecimalError::OutOfRange, CanonicalDecimalField::Value,
                  "JSON uint32 parsing rejects a value above the caller ceiling");
    const auto maximum = bloom::project::parseCanonicalJsonUInt32("4294967295", UINT32_MAX);
    expectations.expect(maximum && *maximum.value() == UINT32_MAX,
                        "JSON uint32 parsing accepts the type maximum");
    expectFailure(expectations, bloom::project::parseCanonicalJsonUInt32("4294967296", UINT32_MAX),
                  CanonicalDecimalError::OutOfRange, CanonicalDecimalField::Value,
                  "JSON uint32 parsing rejects a value above the type maximum");
    for (const std::string_view invalid : {"-0", "-1", "+1", "01", "1.0", "1e0"}) {
        expectFailure(
            expectations, bloom::project::parseCanonicalJsonUInt32(invalid, UINT32_MAX),
            CanonicalDecimalError::InvalidLexicalForm, CanonicalDecimalField::Value,
            "JSON integer parsing rejects signs, fractions, exponents, and leading zeroes");
    }
}

void testRationalTime(Expectations& expectations) {
    const auto negative = bloom::project::parseCanonicalRationalTime("-2", "3");
    const auto minimum = bloom::project::parseCanonicalRationalTime("-9223372036854775808", "1");
    const auto extrema =
        bloom::project::parseCanonicalRationalTime("9223372036854775807", "9223372036854775806");
    const auto zero = bloom::project::parseCanonicalRationalTime("0", "1");
    expectations.expect(
        negative && negative.value()->numerator() == -2 && negative.value()->denominator() == 3 &&
            minimum && minimum.value()->numerator() == std::numeric_limits<std::int64_t>::min() &&
            extrema && zero && *zero.value() == bloom::core::RationalTime{},
        "normalized rational parsing preserves signed extrema and canonical zero");

    expectFailure(expectations, bloom::project::parseCanonicalRationalTime("2", "4"),
                  CanonicalDecimalError::NotReduced, CanonicalDecimalField::Value,
                  "a rational must already be reduced");
    expectFailure(expectations, bloom::project::parseCanonicalRationalTime("0", "2"),
                  CanonicalDecimalError::NonCanonicalZero, CanonicalDecimalField::Denominator,
                  "a zero rational requires denominator one");
    expectFailure(expectations, bloom::project::parseCanonicalRationalTime("1", "0"),
                  CanonicalDecimalError::ZeroNotAllowed, CanonicalDecimalField::Denominator,
                  "a rational reports a zero denominator precisely");
    expectFailure(expectations, bloom::project::parseCanonicalRationalTime("1", "-1"),
                  CanonicalDecimalError::OutOfRange, CanonicalDecimalField::Denominator,
                  "a rational rejects a negative denominator as outside its domain");
    expectFailure(expectations, bloom::project::parseCanonicalRationalTime("01", "1"),
                  CanonicalDecimalError::InvalidLexicalForm, CanonicalDecimalField::Numerator,
                  "a rational identifies a malformed numerator");
    expectFailure(expectations,
                  bloom::project::parseCanonicalRationalTime("1", "9223372036854775808"),
                  CanonicalDecimalError::OutOfRange, CanonicalDecimalField::Denominator,
                  "a rational identifies denominator overflow");
    expectFailure(expectations,
                  bloom::project::parseCanonicalRationalTime("-9223372036854775808", "2"),
                  CanonicalDecimalError::NotReduced, CanonicalDecimalField::Value,
                  "the minimum int64 numerator is checked for gcd without overflow");
}

void testPositiveRatios(Expectations& expectations) {
    const auto ratio = bloom::project::parseCanonicalPositiveRatio("4294967295", "4294967294");
    expectations.expect(ratio && ratio.value()->numerator == UINT32_MAX &&
                            ratio.value()->denominator == UINT32_MAX - 1,
                        "a positive ratio accepts reduced uint32 extrema");
    const auto pixelAspect = bloom::project::parseCanonicalPixelAspectRatio("4", "3");
    expectations.expect(pixelAspect && pixelAspect.value()->numerator() == 4 &&
                            pixelAspect.value()->denominator() == 3,
                        "a canonical positive ratio constructs the core pixel-aspect value");

    expectFailure(expectations, bloom::project::parseCanonicalPositiveRatio("0", "1"),
                  CanonicalDecimalError::ZeroNotAllowed, CanonicalDecimalField::Numerator,
                  "a positive ratio rejects a zero numerator");
    expectFailure(expectations, bloom::project::parseCanonicalPositiveRatio("1", "0"),
                  CanonicalDecimalError::ZeroNotAllowed, CanonicalDecimalField::Denominator,
                  "a positive ratio rejects a zero denominator");
    expectFailure(expectations, bloom::project::parseCanonicalPositiveRatio("-1", "1"),
                  CanonicalDecimalError::OutOfRange, CanonicalDecimalField::Numerator,
                  "a positive ratio identifies a negative numerator");
    expectFailure(expectations, bloom::project::parseCanonicalPositiveRatio("1", "-1"),
                  CanonicalDecimalError::OutOfRange, CanonicalDecimalField::Denominator,
                  "a positive ratio identifies a negative denominator");
    expectFailure(expectations, bloom::project::parseCanonicalPositiveRatio("4294967296", "1"),
                  CanonicalDecimalError::OutOfRange, CanonicalDecimalField::Numerator,
                  "a positive ratio rejects a numerator above uint32");
    expectFailure(expectations, bloom::project::parseCanonicalPositiveRatio("1", "4294967296"),
                  CanonicalDecimalError::OutOfRange, CanonicalDecimalField::Denominator,
                  "a positive ratio rejects a denominator above uint32");
    expectFailure(expectations, bloom::project::parseCanonicalPositiveRatio("6", "4"),
                  CanonicalDecimalError::NotReduced, CanonicalDecimalField::Value,
                  "a positive ratio must already be reduced");
}

void testFormattingAndRoundTrips(Expectations& expectations) {
    const auto unsignedZero = bloom::project::formatCanonicalUInt64(std::uint64_t{0});
    const auto unsignedMaximum = bloom::project::formatCanonicalUInt64(UINT64_MAX);
    const auto signedMinimum = bloom::project::formatCanonicalInt64(INT64_MIN);
    const auto signedMaximum = bloom::project::formatCanonicalInt64(INT64_MAX);
    expectations.expect(unsignedZero.view() == "0" &&
                            unsignedMaximum.view() == "18446744073709551615" &&
                            signedMinimum.view() == "-9223372036854775808" &&
                            signedMaximum.view() == "9223372036854775807",
                        "fixed owned decimal text formats every integer boundary exactly");
    expectations.expect(unsignedMaximum.size() == 20 && signedMinimum.size() == 20 &&
                            !unsignedMaximum.empty(),
                        "fixed decimal text reports the exact non-NUL payload size");

    for (const std::uint64_t value :
         {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{9}, std::uint64_t{10}, UINT64_MAX}) {
        const auto text = bloom::project::formatCanonicalUInt64(value);
        const auto parsed = bloom::project::parseCanonicalAllocatorHighWater(text.view());
        expectations.expect(parsed && *parsed.value() == value,
                            "unsigned formatting round-trips through canonical parsing");
    }
    for (const std::int64_t value :
         {INT64_MIN, std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}, INT64_MAX}) {
        const auto text = bloom::project::formatCanonicalInt64(value);
        const auto parsed = bloom::project::parseCanonicalInt64(text.view());
        expectations.expect(parsed && *parsed.value() == value,
                            "signed formatting round-trips through canonical parsing");
    }
}

void testCanonicalFloat64(Expectations& expectations) {
    struct Fixture final {
        std::uint64_t bits;
        std::string_view text;
    };
    constexpr Fixture fixtures[] = {
        {0x0000000000000000ULL, "0.0"},
        {0x8000000000000000ULL, "-0.0"},
        {0x0000000000000001ULL, "5e-324"},
        {0x8000000000000001ULL, "-5e-324"},
        {0x7fefffffffffffffULL, "1.7976931348623157e+308"},
        {0xffefffffffffffffULL, "-1.7976931348623157e+308"},
        {0x4340000000000000ULL, "9007199254740992.0"},
        {0xc340000000000000ULL, "-9007199254740992.0"},
        {0x4430000000000000ULL, "295147905179352830000.0"},
        {0x44b52d02c7e14af5ULL, "9.999999999999997e+22"},
        {0x44b52d02c7e14af6ULL, "1e+23"},
        {0x44b52d02c7e14af7ULL, "1.0000000000000001e+23"},
        {0x444b1ae4d6e2ef4eULL, "999999999999999700000.0"},
        {0x444b1ae4d6e2ef4fULL, "999999999999999900000.0"},
        {0x444b1ae4d6e2ef50ULL, "1e+21"},
        {0x3eb0c6f7a0b5ed8cULL, "9.999999999999997e-7"},
        {0x3eb0c6f7a0b5ed8dULL, "0.000001"},
        {0x41b3de4355555553ULL, "333333333.3333332"},
        {0x41b3de4355555554ULL, "333333333.33333325"},
        {0x41b3de4355555555ULL, "333333333.3333333"},
        {0x41b3de4355555556ULL, "333333333.3333334"},
        {0x41b3de4355555557ULL, "333333333.33333343"},
        {0xbecbf647612f3696ULL, "-0.0000033333333333333333"},
        {0x43143ff3c1cb0959ULL, "1424953923781206.2"},
    };

    for (const auto& fixture : fixtures) {
        const auto value = std::bit_cast<double>(fixture.bits);
        const auto formatted = bloom::project::formatCanonicalFloat64(value);
        expectations.expect(formatted && formatted.value()->view() == fixture.text,
                            "Float64 formatting matches the frozen binary64 golden");

        const auto parsed = bloom::project::parseCanonicalFloat64(fixture.text);
        expectations.expect(parsed && std::bit_cast<std::uint64_t>(*parsed.value()) == fixture.bits,
                            "canonical Float64 text preserves its exact binary64 bits");
    }

    for (const std::string_view noncanonical :
         {"0", "-0", "1", "1.", ".1", "+1.0", "01.0", "1.00", "1e-6", "1E+21", "1e21", "1.0e+21",
          "1e+021", " 1.0", "1.0 "}) {
        const auto parsed = bloom::project::parseCanonicalFloat64(noncanonical);
        expectations.expect(!parsed &&
                                (parsed.error() == CanonicalDecimalError::InvalidLexicalForm ||
                                 parsed.error() == CanonicalDecimalError::NonCanonical),
                            "canonical Float64 parsing rejects alternate JSON spellings");
    }

    expectFailure(expectations, bloom::project::parseCanonicalFloat64("1e+9999"),
                  CanonicalDecimalError::OutOfRange, CanonicalDecimalField::Value,
                  "Float64 parsing rejects overflow");
    const auto infinity =
        bloom::project::formatCanonicalFloat64(std::numeric_limits<double>::infinity());
    const auto nan =
        bloom::project::formatCanonicalFloat64(std::numeric_limits<double>::quiet_NaN());
    expectations.expect(!infinity && infinity.error() == CanonicalDecimalError::NonFinite && !nan &&
                            nan.error() == CanonicalDecimalError::NonFinite,
                        "Float64 formatting rejects infinity and NaN");
}

} // namespace

int main() {
    Expectations expectations;
    testObjectIdsAndHighWater(expectations);
    testSignedIntegers(expectations);
    testJsonIntegers(expectations);
    testRationalTime(expectations);
    testPositiveRatios(expectations);
    testFormattingAndRoundTrips(expectations);
    testCanonicalFloat64(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
