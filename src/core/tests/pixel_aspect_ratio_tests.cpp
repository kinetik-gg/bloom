#include <bloom/core/pixel_aspect_ratio.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <source_location>
#include <string_view>
#include <type_traits>

namespace {

using bloom::core::PixelAspectRatio;

static_assert(!std::is_default_constructible_v<PixelAspectRatio>);

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

void testCanonicalConstruction(ExpectationContext& expectations) {
    constexpr auto square = PixelAspectRatio::square();
    constexpr auto normalizedSquare = PixelAspectRatio::create(2, 2);
    constexpr auto nonSquare = PixelAspectRatio::create(8, 6);
    constexpr auto threeQuarters = PixelAspectRatio::create(3, 4);
    constexpr auto two = PixelAspectRatio::create(2, 1);
    constexpr auto maximumTerm = std::numeric_limits<std::uint32_t>::max();
    constexpr auto nearlyOneBelow = PixelAspectRatio::create(maximumTerm - 1, maximumTerm);
    constexpr auto nearlyOneAbove = PixelAspectRatio::create(maximumTerm, maximumTerm - 1);

    expectations.expect(normalizedSquare.has_value() && *normalizedSquare == square,
                        "equivalent square ratios have one canonical value");
    expectations.expect(nonSquare.has_value() && nonSquare->numerator() == 4 &&
                            nonSquare->denominator() == 3,
                        "non-square ratios are reduced to canonical terms");
    expectations.expect(nonSquare.has_value() && *nonSquare > square && threeQuarters.has_value() &&
                            two.has_value() && *threeQuarters < *two,
                        "ratio comparison follows numeric value rather than stored-term order");
    expectations.expect(nearlyOneBelow.has_value() && nearlyOneAbove.has_value() &&
                            *nearlyOneBelow < *nearlyOneAbove,
                        "ratio comparison remains ordered at the maximum cross-product boundary");
}

void testValidationBoundaries(ExpectationContext& expectations) {
    expectations.expect(!PixelAspectRatio::create(0, 1).has_value() &&
                            !PixelAspectRatio::create(1, 0).has_value(),
                        "zero terms cannot construct a pixel aspect ratio");

    constexpr auto largeFactor = std::uint64_t{1} << 32U;
    constexpr auto largeReducible = PixelAspectRatio::create(largeFactor * 4, largeFactor * 3);
    expectations.expect(largeReducible.has_value() && largeReducible->numerator() == 4 &&
                            largeReducible->denominator() == 3,
                        "wide input terms reduce before canonical storage bounds are checked");

    constexpr auto beyondCanonicalTerm =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
    expectations.expect(!PixelAspectRatio::create(beyondCanonicalTerm, 1).has_value() &&
                            !PixelAspectRatio::create(1, beyondCanonicalTerm).has_value(),
                        "reduced terms outside canonical 32-bit storage are rejected");
}

} // namespace

int main() {
    ExpectationContext expectations;
    testCanonicalConstruction(expectations);
    testValidationBoundaries(expectations);
    return expectations.ok() ? 0 : 1;
}
