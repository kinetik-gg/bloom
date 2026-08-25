#include <bloom/project/unknown_json_number.hpp>

#include <cstdint>
#include <iostream>
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
using bloom::project::UnknownJsonNumberKind;

template <typename Value>
concept ExposesRvalueResultPointer =
    requires { std::declval<const bloom::project::CanonicalDecimalResult<Value>&&>().value(); };

template <typename Value>
concept ExposesRvalueTextView = requires { std::declval<const Value&&>().view(); };

template <typename Value>
concept ExposesRvalueTextData = requires { std::declval<const Value&&>().data(); };

static_assert(!std::is_default_constructible_v<bloom::project::UnknownJsonNumber>);
static_assert(!ExposesRvalueResultPointer<bloom::project::UnknownJsonNumber>);
static_assert(!ExposesRvalueTextView<bloom::project::UnknownJsonNumberText>);
static_assert(!ExposesRvalueTextData<bloom::project::UnknownJsonNumberText>);

void expectFailure(Expectations& expectations,
                   const bloom::project::UnknownJsonNumberResult& result,
                   const CanonicalDecimalError error, const std::string_view message) {
    expectations.expect(!result && result.value() == nullptr && result.error() == error &&
                            result.field() == CanonicalDecimalField::Value,
                        message);
}

void expectInteger(Expectations& expectations, const std::string_view token,
                   const std::int64_t expected) {
    const auto parsed = bloom::project::parseUnknownJsonNumber(token);
    expectations.expect(parsed && parsed.value()->kind() == UnknownJsonNumberKind::Integer &&
                            parsed.value()->integerValue() == expected &&
                            !parsed.value()->float64Bits().has_value(),
                        "a canonical int64 token is classified and owned as an integer");
    if (parsed) {
        const auto formatted = bloom::project::formatUnknownJsonNumber(*parsed.value());
        expectations.expect(formatted.view() == token,
                            "an unknown integer reproduces its exact canonical token");
    }
}

void expectFloat64(Expectations& expectations, const std::string_view token,
                   const std::uint64_t expectedBits) {
    const auto parsed = bloom::project::parseUnknownJsonNumber(token);
    expectations.expect(parsed && parsed.value()->kind() == UnknownJsonNumberKind::Float64 &&
                            parsed.value()->float64Bits() == expectedBits &&
                            !parsed.value()->integerValue().has_value(),
                        "a canonical Float64 token is classified and owned as exact bits");
    if (parsed) {
        const auto formatted = bloom::project::formatUnknownJsonNumber(*parsed.value());
        expectations.expect(formatted.view() == token,
                            "unknown Float64 bits reproduce the exact canonical token");
    }
}

void testIntegerSubsetAndPrecedence(Expectations& expectations) {
    expectInteger(expectations, "-9223372036854775808", INT64_MIN);
    expectInteger(expectations, "-1", -1);
    expectInteger(expectations, "0", 0);
    expectInteger(expectations, "1", 1);
    expectInteger(expectations, "9007199254740992", 9'007'199'254'740'992LL);
    expectInteger(expectations, "9223372036854775807", INT64_MAX);

    expectFailure(expectations, bloom::project::parseUnknownJsonNumber("9223372036854775808"),
                  CanonicalDecimalError::OutOfRange,
                  "a positive canonical integer outside int64 is not reclassified as Float64");
    expectFailure(expectations, bloom::project::parseUnknownJsonNumber("-9223372036854775809"),
                  CanonicalDecimalError::OutOfRange,
                  "a negative canonical integer outside int64 is not reclassified as Float64");
    expectFailure(expectations, bloom::project::parseUnknownJsonNumber("18446744073709551615"),
                  CanonicalDecimalError::OutOfRange,
                  "the uint64 maximum remains an unsupported out-of-range integer");

    const std::string hugeInteger(10'000, '9');
    expectFailure(expectations, bloom::project::parseUnknownJsonNumber(hugeInteger),
                  CanonicalDecimalError::OutOfRange,
                  "an arbitrarily long canonical integer is rejected without a float fallback");
}

void testFloat64GoldenTokens(Expectations& expectations) {
    struct Fixture final {
        std::string_view token;
        std::uint64_t bits;
    };
    constexpr Fixture fixtures[] = {
        {"0.0", 0x0000000000000000ULL},
        {"-0.0", 0x8000000000000000ULL},
        {"5e-324", 0x0000000000000001ULL},
        {"-5e-324", 0x8000000000000001ULL},
        {"2.225073858507201e-308", 0x000fffffffffffffULL},
        {"2.2250738585072014e-308", 0x0010000000000000ULL},
        {"1.7976931348623157e+308", 0x7fefffffffffffffULL},
        {"-1.7976931348623157e+308", 0xffefffffffffffffULL},
        {"9007199254740992.0", 0x4340000000000000ULL},
        {"9.999999999999997e-7", 0x3eb0c6f7a0b5ed8cULL},
        {"0.000001", 0x3eb0c6f7a0b5ed8dULL},
        {"999999999999999900000.0", 0x444b1ae4d6e2ef4fULL},
        {"1e+21", 0x444b1ae4d6e2ef50ULL},
    };

    for (const auto& fixture : fixtures) {
        expectFloat64(expectations, fixture.token, fixture.bits);
    }
}

void testNoncanonicalSpellings(Expectations& expectations) {
    for (const std::string_view token :
         {"-0", "1.00", "-0.00", "0.0000010", "0.10000000000000001", "4.9406564584124654e-324",
          "9007199254740993.0", "1.79769313486231570e+308", "1E+21", "1e21", "1e+021", "1.0e+21",
          "1e+20", "0.000001e+0"}) {
        expectFailure(expectations, bloom::project::parseUnknownJsonNumber(token),
                      CanonicalDecimalError::NonCanonical,
                      "a finite spelling that would normalize or round on rewrite is rejected");
    }

    for (const std::string_view token :
         {"", "+1", "00", "01", "-01", ".1", "1.", "1e", "1e+", "nan", " 1", "1 "}) {
        expectFailure(expectations, bloom::project::parseUnknownJsonNumber(token),
                      CanonicalDecimalError::InvalidLexicalForm,
                      "a non-RFC-8259 or noncanonical integer lexical form is rejected");
    }
}

void testFloat64RangeFailures(Expectations& expectations) {
    for (const std::string_view token :
         {"1e+309", "-1e+309", "1.7976931348623159e+308", "9e+999999"}) {
        expectFailure(expectations, bloom::project::parseUnknownJsonNumber(token),
                      CanonicalDecimalError::OutOfRange,
                      "a JSON number that overflows binary64 is rejected");
    }

    for (const std::string_view token : {"1e-324", "-1e-324", "1e-999999"}) {
        expectFailure(expectations, bloom::project::parseUnknownJsonNumber(token),
                      CanonicalDecimalError::NonCanonical,
                      "a decimal that underflows or normalizes to signed zero is rejected");
    }
}

} // namespace

int main() {
    Expectations expectations;
    testIntegerSubsetAndPrecedence(expectations);
    testFloat64GoldenTokens(expectations);
    testNoncanonicalSpellings(expectations);
    testFloat64RangeFailures(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
