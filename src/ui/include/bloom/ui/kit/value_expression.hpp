#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::ui::kit {

// What a numeric field accepts as typed text, kept free of Qt so it can be tested on its own and
// read by any surface that types a number (KValueField is the one user today).
//
//   number      42, -3.5, 0,5 (either decimal separator), 1e-3, .5
//   unit        the field's own unit after any number or after the whole entry: 50%, 10 px,
//               (10 + 5) px. Any other unit is refused rather than ignored, because "10 cm" in a
//               pixel field is a mistake the artist should see.
//   arithmetic  + - * / and parentheses, with the usual precedence: 1920/2, (3 + 4) * 2
//   relative    +=, -=, *= and /= apply the rest to the field's current value: += 10
//
// An integer evaluation is exact over the whole int64 range: literals are parsed digit by digit
// (never through a double, which stops being exact at 2^53), every operation is overflow checked,
// and a division must come out whole. A fractional literal is accepted only when its fraction is
// all zeroes (12.0).
enum class ValueExpressionError : std::uint8_t {
    None,
    Empty,
    Syntax,
    UnknownUnit,
    DivideByZero,
    NotFinite,
    NotInteger,
    Overflow,
};

struct ValueExpressionContext {
    // The value a relative entry (+= 10) applies to.
    double current = 0.0;
    std::int64_t currentInteger = 0;
    // The field's own unit, UTF-8; empty when the field has none. Compared exactly, or ignoring
    // ASCII case ("PX" for "px").
    std::string_view unit;
};

struct ValueExpressionResult {
    ValueExpressionError error = ValueExpressionError::None;
    double value = 0.0;
    std::int64_t integer = 0;
    // The unit text that was refused, for UnknownUnit.
    std::string unit;

    [[nodiscard]] bool ok() const noexcept { return error == ValueExpressionError::None; }
};

[[nodiscard]] ValueExpressionResult evaluateValueExpression(std::string_view text,
                                                            const ValueExpressionContext& context);
[[nodiscard]] ValueExpressionResult
evaluateIntegerExpression(std::string_view text, const ValueExpressionContext& context);

} // namespace bloom::ui::kit
