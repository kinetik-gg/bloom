#include <bloom/core/color.hpp>
#include <bloom/core/safe_parse.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>

namespace {

using namespace bloom;

class ExpectationContext final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

// The accepted decimal forms, each with the exact double it must answer. Table-driven because the
// contract IS a table: a form either appears here or is not a form Bloom reads.
void testScalarForms(ExpectationContext& expectations) {
    struct Accepted final {
        std::string_view text;
        double value;
    };
    static constexpr Accepted kAccepted[]{
        {"0", 0.0},       {"1", 1.0},      {"-1", -1.0},    {"+1", 1.0},      {"1.5", 1.5},
        {"-1.5", -1.5},   {".5", 0.5},     {"-.5", -0.5},   {"5.", 5.0},      {"  2.25  ", 2.25},
        {"\t-3\n", -3.0}, {"1e3", 1000.0}, {"1E3", 1000.0}, {"1e+3", 1000.0}, {"1e-3", 0.001},
        {"1.5e2", 150.0}, {"0.0", 0.0},    {"00012", 12.0}, {"-0.0", -0.0},
    };
    for (const auto& accepted : kAccepted) {
        const auto parsed = core::parseScalar(accepted.text);
        expectations.expect(parsed.has_value() && *parsed == accepted.value,
                            "an accepted decimal form parses to its documented value");
    }
    // Negative zero keeps its sign: it is a representable finite double and the author typed it.
    const auto negativeZero = core::parseScalar("-0");
    expectations.expect(negativeZero.has_value() && *negativeZero == 0.0 &&
                            std::signbit(*negativeZero),
                        "-0 parses as negative zero");

    static constexpr std::string_view kRejected[]{
        "",     " ",     "\t\n", "abc", "1abc", "12 34", "1,234",    "0x10",   "inf",
        "-inf", "Inf",   "nan",  "NaN", "1e",   "1e+",   "e5",       ".",      "-",
        "+",    "1.2.3", "--1",  "1 2", "١٢٣",  "1_000", "1e999999", "1e-400", "1d5",
    };
    for (const auto text : kRejected) {
        expectations.expect(!core::parseScalar(text).has_value(),
                            "an undocumented form is refused rather than half-read");
    }
}

void testIntegerForms(ExpectationContext& expectations) {
    struct Accepted final {
        std::string_view text;
        std::int32_t radix;
        std::int64_t value;
    };
    static constexpr Accepted kAccepted[]{
        {"0", 10, 0},     {"42", 10, 42},    {"-42", 10, -42}, {"+42", 10, 42},
        {"  7  ", 10, 7}, {"1010", 2, 10},   {"777", 8, 511},  {"ff", 16, 255},
        {"FF", 16, 255},  {"-ff", 16, -255}, {"z", 36, 35},    {"007", 10, 7},
    };
    for (const auto& accepted : kAccepted) {
        const auto parsed = core::parseInteger(accepted.text, accepted.radix);
        expectations.expect(parsed.has_value() && *parsed == accepted.value,
                            "an accepted integer form parses at its radix");
    }
    // The signed boundary in both directions: the most negative value parses exactly, and one past
    // it is refused rather than wrapping.
    const auto lowest = core::parseInteger("-9223372036854775808", 10);
    const auto highest = core::parseInteger("9223372036854775807", 10);
    expectations.expect(lowest.has_value() && *lowest == std::numeric_limits<std::int64_t>::min(),
                        "the most negative integer parses exactly");
    expectations.expect(highest.has_value() && *highest == std::numeric_limits<std::int64_t>::max(),
                        "the largest integer parses exactly");
    expectations.expect(!core::parseInteger("9223372036854775808", 10).has_value() &&
                            !core::parseInteger("-9223372036854775809", 10).has_value(),
                        "an overflowing magnitude is refused rather than wrapped");

    struct Rejected final {
        std::string_view text;
        std::int32_t radix;
    };
    static constexpr Rejected kRejected[]{
        {"", 10},  {" ", 10},    {"-", 10}, {"+", 10},  {"1.5", 10}, {"2", 2},
        {"g", 16}, {"0x10", 16}, {"12", 1}, {"12", 37}, {"1 2", 10}, {"١٢", 10},
    };
    for (const auto& rejected : kRejected) {
        expectations.expect(!core::parseInteger(rejected.text, rejected.radix).has_value(),
                            "an undocumented integer form or radix is refused");
    }
}

void testBooleanAndColorForms(ExpectationContext& expectations) {
    for (const auto* const text : {"true", "TRUE", "True", "yes", "on", "1", "  true  "}) {
        const auto parsed = core::parseBoolean(text);
        expectations.expect(parsed.has_value() && *parsed, "every documented true spelling parses");
    }
    for (const auto* const text : {"false", "FALSE", "no", "off", "0", "\tfalse\n"}) {
        const auto parsed = core::parseBoolean(text);
        expectations.expect(parsed.has_value() && !*parsed,
                            "every documented false spelling parses");
    }
    for (const auto* const text : {"", " ", "maybe", "t", "f", "2", "-1", "truee", "oui"}) {
        expectations.expect(!core::parseBoolean(text).has_value(),
                            "an undocumented boolean spelling is refused");
    }

    const auto opaque = core::parseHexColor("#ffffff");
    expectations.expect(opaque.has_value() && opaque->red == 1.0 && opaque->green == 1.0 &&
                            opaque->blue == 1.0 && opaque->alpha == 1.0,
                        "#RRGGBB reads full channels as exactly one and alpha as opaque");
    const auto alpha = core::parseHexColor("  #00FF0080  ");
    expectations.expect(alpha.has_value() && alpha->red == 0.0 && alpha->green == 1.0 &&
                            alpha->blue == 0.0 && alpha->alpha == 128.0 / 255.0,
                        "#RRGGBBAA reads its fourth pair as alpha");
    for (const auto* const text :
         {"", "#", "ffffff", "#fff", "#ffffg0", "#ffffff00ff", "# ffffff"}) {
        expectations.expect(!core::parseHexColor(text).has_value(),
                            "an undocumented colour spelling is refused");
    }
}

void testFormatting(ExpectationContext& expectations) {
    struct Case final {
        double value;
        std::int32_t decimals;
        bool trim;
        std::int32_t width;
        std::string_view text;
    };
    static constexpr Case kCases[]{
        {1.5, 2, false, 0, "1.50"},  {1.5, 2, true, 0, "1.5"},      {2.0, 2, true, 0, "2"},
        {-1.5, 1, false, 0, "-1.5"}, {0.0, 0, false, 0, "0"},       {3.14159, 3, false, 0, "3.142"},
        {7.0, 0, false, 4, "0007"},  {-7.0, 0, false, 4, "-007"},   {0.0, 2, true, 3, "000"},
        {-0.004, 2, true, 0, "-0"},  {1234.0, 0, false, 2, "1234"},
    };
    for (const auto& item : kCases) {
        const auto text = core::formatScalar(item.value, item.decimals, item.trim, item.width);
        expectations.expect(text == item.text, "a scalar formats deterministically");
    }
    expectations.expect(
        core::formatScalar(std::numeric_limits<double>::infinity(), 2, false, 0).empty() &&
            core::formatScalar(std::numeric_limits<double>::quiet_NaN(), 2, false, 0).empty(),
        "a non-finite value has no decimal spelling and formats as empty");

    struct IntegerCase final {
        std::int64_t value;
        std::int32_t radix;
        std::int32_t width;
        std::string_view text;
    };
    static constexpr IntegerCase kIntegerCases[]{
        {0, 10, 0, "0"},    {255, 16, 0, "ff"},    {255, 16, 4, "00ff"}, {10, 2, 0, "1010"},
        {511, 8, 0, "777"}, {-42, 10, 5, "-0042"}, {35, 36, 0, "z"},     {7, 99, 0, "7"},
    };
    for (const auto& item : kIntegerCases) {
        expectations.expect(core::formatInteger(item.value, item.radix, item.width) == item.text,
                            "an integer formats deterministically at its radix");
    }
    expectations.expect(core::formatInteger(std::numeric_limits<std::int64_t>::min(), 10, 0) ==
                            "-9223372036854775808",
                        "the most negative integer formats without overflowing its own negation");

    expectations.expect(core::formatHexColor(1.0, 0.0, 0.5, 1.0, false, false) == "#ff0080",
                        "a colour formats as #RRGGBB by default");
    expectations.expect(core::formatHexColor(1.0, 0.0, 0.5, 0.25, true, true) == "#FF008040",
                        "alpha joins the spelling on request, in the requested case");
    expectations.expect(core::formatHexColor(-1.0, 2.0, 0.0, 1.0, false, false) == "#00ff00",
                        "channels outside the unit range are clamped rather than wrapped");
}

// Every parsed spelling must format back to something that parses to the same value, which is the
// only property that makes a conversion pair useful in a graph.
void testRoundTrips(ExpectationContext& expectations) {
    for (const double value : {0.0, 1.0, -1.0, 0.125, -12345.678, 1e12}) {
        const auto text = core::formatScalar(value, 6, false, 0);
        const auto parsed = core::parseScalar(text);
        expectations.expect(parsed.has_value() && std::fabs(*parsed - value) <= 1e-6,
                            "a formatted scalar parses back to itself");
    }
    for (const std::int64_t value :
         {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1}, std::int64_t{987654321},
          std::numeric_limits<std::int64_t>::min()}) {
        for (const std::int32_t radix : {2, 8, 10, 16, 36}) {
            const auto text = core::formatInteger(value, radix, 0);
            const auto parsed = core::parseInteger(text, radix);
            expectations.expect(parsed.has_value() && *parsed == value,
                                "a formatted integer parses back to itself at its radix");
        }
    }
    for (const auto* const text : {"#000000", "#ffffff", "#123456", "#12345678"}) {
        const auto parsed = core::parseHexColor(text);
        expectations.expect(parsed.has_value(), "a documented colour spelling parses");
        if (!parsed.has_value()) {
            continue;
        }
        const auto formatted =
            core::formatHexColor(parsed->red, parsed->green, parsed->blue, parsed->alpha,
                                 std::string_view(text).size() == 9, false);
        expectations.expect(formatted == text, "a parsed colour formats back to its own spelling");
    }
}

void testColorMath(ExpectationContext& expectations) {
    expectations.expect(core::rec709Luminance({1.0, 1.0, 1.0, 1.0}) ==
                            core::kRec709RedLuminanceWeight + core::kRec709GreenLuminanceWeight +
                                core::kRec709BlueLuminanceWeight,
                        "white's Rec.709 luminance is the sum of the weights");
    expectations.expect(core::rec709Luminance({0.0, 1.0, 0.0, 0.0}) ==
                            core::kRec709GreenLuminanceWeight,
                        "a primary's luminance is its own weight, and alpha takes no part");

    const auto grey = core::toHsva({0.5, 0.5, 0.5, 1.0});
    expectations.expect(grey.hue == 0.0 && grey.saturation == 0.0 && grey.value == 0.5,
                        "a grey has no hue and no saturation");
    const auto red = core::toHsva({1.0, 0.0, 0.0, 0.25});
    expectations.expect(red.hue == 0.0 && red.saturation == 1.0 && red.value == 1.0 &&
                            red.alpha == 0.25,
                        "red sits at hue zero with full saturation, carrying its alpha");
    const auto green = core::toHsva({0.0, 1.0, 0.0, 1.0});
    const auto blue = core::toHsva({0.0, 0.0, 1.0, 1.0});
    expectations.expect(green.hue == 120.0 && blue.hue == 240.0,
                        "the other two primaries sit a third of the circle apart");

    for (const auto& color :
         {core::Color4d{1.0, 0.0, 0.0, 1.0}, core::Color4d{0.2, 0.4, 0.8, 0.5},
          core::Color4d{0.0, 0.0, 0.0, 1.0}, core::Color4d{1.0, 1.0, 1.0, 1.0}}) {
        const auto restored = core::fromHsva(core::toHsva(color));
        const bool same = std::fabs(restored.red - color.red) <= 1e-12 &&
                          std::fabs(restored.green - color.green) <= 1e-12 &&
                          std::fabs(restored.blue - color.blue) <= 1e-12 &&
                          restored.alpha == color.alpha;
        expectations.expect(same, "HSV round-trips a straight RGBA triple");
    }
    const auto wrapped = core::fromHsva({400.0, 1.0, 1.0, 1.0});
    const auto equivalent = core::fromHsva({40.0, 1.0, 1.0, 1.0});
    expectations.expect(wrapped == equivalent, "hue wraps rather than clamping");
}

} // namespace

int main() {
    ExpectationContext expectations;
    testScalarForms(expectations);
    testIntegerForms(expectations);
    testBooleanAndColorForms(expectations);
    testFormatting(expectations);
    testRoundTrips(expectations);
    testColorMath(expectations);
    return expectations.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}
