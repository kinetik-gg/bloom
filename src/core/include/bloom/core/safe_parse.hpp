#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::core {

// The SAFE PARSE contract (task UTIL-1). One implementation of "read a number, a boolean or a
// colour out of text", shared by every value-graph node that parses, so the forms Bloom accepts are
// decided once rather than per node.
//
// Three rules make this different from the obvious `std::stod` call:
//
//   NOTHING THROWS AND NOTHING IS UNDEFINED. Every entry point answers std::nullopt for text it
//   does not accept -- empty, whitespace only, trailing rubbish, an overflowing magnitude. There is
//   no exception to catch and no errno to read, so a parsing node can promise its caller that
//   evaluation never fails.
//
//   THE WHOLE STRING IS CONSUMED. `std::stod("12abc")` answers 12 and reports the rubbish only
//   through an out parameter callers routinely drop. Here, text that is not entirely a number is
//   not a number.
//
//   THE PROCESS LOCALE IS NOT CONSULTED. `.` is the decimal separator, ASCII `0`-`9` are the only
//   digits (a non-ASCII digit is invalid, never silently folded), and there are no thousands
//   separators. A project that parses "1.5" must read 1.5 on every machine that opens it, which a
//   locale-sensitive parse cannot promise.
//
// Formatting obeys the mirror of the same rules: precision and padding are explicit arguments, the
// output carries `.` whatever the process locale says, and the same inputs produce the same bytes
// everywhere.

// Leading and trailing ASCII whitespace, removed. Exactly the six characters C calls whitespace;
// no Unicode space separators, because accepting those would make the trim depend on a Unicode
// table version.
[[nodiscard]] std::string_view trimAsciiWhitespace(std::string_view text) noexcept;

// A decimal scalar, in the ONE form documented here and no other:
//
//     [+-]? ( digits ( '.' digits? )? | '.' digits ) ( [eE] [+-]? digits )?
//
// Surrounding whitespace is trimmed first. `inf`, `infinity`, `nan` and hexadecimal floats are
// deliberately NOT accepted -- they are spellings of a non-finite answer, and a value graph that
// silently acquired one would poison every downstream operand. A magnitude OUTSIDE binary64's
// representable range is refused for the same reason, in both directions: `1e999999` would answer
// an infinity and `1e-400` would answer a zero the author did not write. A subnormal is inside the
// range and parses.
//
// `-0` parses, and answers negative zero: it is a representable finite double, and the sign is
// information the author typed.
[[nodiscard]] std::optional<double> parseScalar(std::string_view text) noexcept;

// The radices an Integer conversion offers. The stored value IS the radix, which is why this is a
// plain integer vocabulary rather than an enumeration: 16 means base sixteen in the document, in
// the parser and on the card alike.
inline constexpr std::int32_t kMinimumRadix = 2;
inline constexpr std::int32_t kMaximumRadix = 36;
inline constexpr std::int32_t kDefaultRadix = 10;

// A signed integer in `radix`, with surrounding whitespace trimmed:
//
//     [+-]? digit+
//
// where a digit is `0`-`9` and, above radix 10, `a`-`z` or `A`-`Z` for 10 through 35. No `0x` or
// `0b` prefix: a radix is chosen on the node, so a prefix would be a second, contradictory way to
// say the same thing. An out-of-range radix, an empty digit run, a digit the radix does not have,
// and a magnitude past the std::int64_t range are all refused.
[[nodiscard]] std::optional<std::int64_t> parseInteger(std::string_view text,
                                                       std::int32_t radix) noexcept;

// A boolean, case-insensitively over ASCII, after trimming: `true`, `false`, `yes`, `no`, `on`,
// `off`, `1`, `0`. Nothing else -- an empty string is not false, and "maybe" is not a boolean.
[[nodiscard]] std::optional<bool> parseBoolean(std::string_view text) noexcept;

// One RGBA colour in hexadecimal, after trimming: `#RRGGBB` or `#RRGGBBAA`, with the leading `#`
// REQUIRED and hexadecimal digits accepted in either case. Each pair is read as 0-255 and divided
// by 255, so `#FFFFFF` is exactly 1.0 in every channel. `#RGB` shorthand is not accepted: it is a
// CSS convenience whose expansion rule is a second contract, and the node documents one.
struct ParsedColorChannels final {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 1.0;

    friend bool operator==(const ParsedColorChannels&, const ParsedColorChannels&) = default;
};

[[nodiscard]] std::optional<ParsedColorChannels> parseHexColor(std::string_view text) noexcept;

// The deterministic formatters. `decimals` is clamped into [0, kMaximumFormattedDecimals] and
// `padWidth` into [0, kMaximumFormattedWidth] rather than refused: a formatter has no failure the
// artist could act on, and a clamped precision still prints the number they asked about.
inline constexpr std::int32_t kMaximumFormattedDecimals = 17;
inline constexpr std::int32_t kMaximumFormattedWidth = 256;

// Fixed-point decimal, `decimals` digits after the point, `.` whatever the locale says.
// `trimTrailingZeros` removes trailing fractional zeros and then a bare trailing point, so 2.50
// prints as "2.5" and 2.00 as "2". Zero padding applies to the NUMBER including its sign, the way
// printf's `%0*d` does: the sign stays leftmost and the zeros follow it.
//
// Trimming keeps the SIGN: -0.004 at two decimals rounds to a zero magnitude and prints "-0",
// which is the same spelling formatScalar(-0.0, 0, ...) gives and the honest one -- the value is
// negative and rounded away, not positive.
//
// A non-finite value has no decimal spelling this contract admits, so it formats as the empty
// string -- the same answer parseScalar() gives for text that is not a number, in the other
// direction.
[[nodiscard]] std::string formatScalar(double value, std::int32_t decimals, bool trimTrailingZeros,
                                       std::int32_t padWidth);

// `value` in `radix`, lowercase above 9, zero-padded to `padWidth` with the sign kept leftmost. An
// out-of-range radix formats in base ten rather than refusing, for the same reason the clamps
// above exist.
[[nodiscard]] std::string formatInteger(std::int64_t value, std::int32_t radix,
                                        std::int32_t padWidth);

// `#RRGGBB`, or `#RRGGBBAA` when `includeAlpha`. Each channel is clamped into [0, 1] and rounded
// to nearest, ties away from zero, so 1.0 is `ff` and 0.5 is `80`.
[[nodiscard]] std::string formatHexColor(double red, double green, double blue, double alpha,
                                         bool includeAlpha, bool uppercase);

} // namespace bloom::core
