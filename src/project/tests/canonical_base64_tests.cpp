#include <bloom/project/canonical_base64.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
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

using bloom::project::CanonicalBase64Error;

template <typename Value>
concept ExposesRvalueSizePointer = requires { std::declval<const Value&&>().value(); };

static_assert(!ExposesRvalueSizePointer<bloom::project::CanonicalBase64SizeResult>);

struct RfcVector final {
    std::string_view decoded;
    std::string_view encoded;
};

constexpr std::array rfcVectors{
    RfcVector{"", ""},
    RfcVector{"f", "Zg=="},
    RfcVector{"fo", "Zm8="},
    RfcVector{"foo", "Zm9v"},
    RfcVector{"foob", "Zm9vYg=="},
    RfcVector{"fooba", "Zm9vYmE="},
    RfcVector{"foobar", "Zm9vYmFy"},
};

constexpr std::string_view base64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] std::span<const std::byte> bytes(const std::string_view text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

void expectSizeFailure(Expectations& expectations,
                       const bloom::project::CanonicalBase64SizeResult& result,
                       const CanonicalBase64Error error, const std::string_view message) {
    expectations.expect(!result && result.value() == nullptr && result.error() == error, message);
}

void expectMalformed(Expectations& expectations, const std::string_view encoded,
                     const CanonicalBase64Error error, const std::string_view message) {
    expectSizeFailure(expectations, bloom::project::canonicalBase64DecodedSize(encoded), error,
                      message);

    constexpr auto sentinel = std::byte{0xA5};
    std::array<std::byte, 8> destination{};
    destination.fill(sentinel);
    const auto result = bloom::project::decodeCanonicalBase64(encoded, destination);
    expectations.expect(
        !result && result.error() == error && !result.requiredSize().has_value() &&
            result.bytesWritten() == 0 &&
            std::ranges::all_of(destination,
                                [](const std::byte value) { return value == sentinel; }),
        "malformed input is fully validated before any output write");
}

template <std::size_t Size>
void expectUntouched(Expectations& expectations, const std::array<std::byte, Size>& bytes,
                     const std::byte sentinel, const std::string_view message) {
    expectations.expect(
        std::ranges::all_of(bytes, [sentinel](const std::byte value) { return value == sentinel; }),
        message);
}

template <std::size_t Size>
void expectUntouched(Expectations& expectations, const std::array<char, Size>& characters,
                     const char sentinel, const std::string_view message) {
    expectations.expect(
        std::ranges::all_of(characters, [sentinel](const char value) { return value == sentinel; }),
        message);
}

void testRfcVectors(Expectations& expectations) {
    for (const auto& vector : rfcVectors) {
        const auto encodedSize = bloom::project::canonicalBase64EncodedSize(vector.decoded.size());
        const auto decodedSize = bloom::project::canonicalBase64DecodedSize(vector.encoded);
        expectations.expect(encodedSize && *encodedSize.value() == vector.encoded.size() &&
                                decodedSize && *decodedSize.value() == vector.decoded.size(),
                            "RFC vector preflight sizes are exact");

        std::array<char, 8> encodedBuffer{};
        const auto encodedDestination = std::span(encodedBuffer).first(vector.encoded.size());
        const auto encodeResult =
            bloom::project::encodeCanonicalBase64(bytes(vector.decoded), encodedDestination);
        expectations.expect(encodeResult && encodeResult.requiredSize() == vector.encoded.size() &&
                                encodeResult.bytesWritten() == vector.encoded.size() &&
                                std::string_view(encodedBuffer.data(), vector.encoded.size()) ==
                                    vector.encoded,
                            "RFC vector encoding produces the canonical spelling");

        std::array<std::byte, 6> decodedBuffer{};
        const auto decodedDestination = std::span(decodedBuffer).first(vector.decoded.size());
        const auto decodeResult =
            bloom::project::decodeCanonicalBase64(vector.encoded, decodedDestination);
        expectations.expect(decodeResult && decodeResult.requiredSize() == vector.decoded.size() &&
                                decodeResult.bytesWritten() == vector.decoded.size() &&
                                std::equal(decodedDestination.begin(), decodedDestination.end(),
                                           bytes(vector.decoded).begin(),
                                           bytes(vector.decoded).end()),
                            "RFC vector decoding preserves the exact source bytes");
    }
}

void testEveryByteValue(Expectations& expectations) {
    std::array<std::byte, 256> source{};
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<std::byte>(index);
    }
    std::array<char, 344> encoded{};
    std::array<std::byte, 256> decoded{};
    const auto encodeResult = bloom::project::encodeCanonicalBase64(source, encoded);
    const auto decodeResult = bloom::project::decodeCanonicalBase64(
        std::string_view(encoded.data(), encoded.size()), decoded);
    expectations.expect(encodeResult && decodeResult && source == decoded,
                        "one round trip preserves all 256 possible byte values");

    for (std::size_t value = 0; value <= 255; ++value) {
        const std::array oneByte{static_cast<std::byte>(value)};
        std::array<char, 4> oneEncoded{};
        std::array<std::byte, 1> oneDecoded{};
        const auto encodedResult = bloom::project::encodeCanonicalBase64(oneByte, oneEncoded);
        const auto decodedResult = bloom::project::decodeCanonicalBase64(
            std::string_view(oneEncoded.data(), oneEncoded.size()), oneDecoded);
        expectations.expect(encodedResult && decodedResult && oneDecoded == oneByte,
                            "each individual byte value round-trips canonically");
    }
}

void testMalformedInput(Expectations& expectations) {
    for (const std::string_view invalid : {"A", "AA", "AAA", "Zg", "Zg=", "Zm8", "Zg==="}) {
        expectMalformed(expectations, invalid, CanonicalBase64Error::InvalidEncodedLength,
                        "non-quartet input rejects missing or excess padding");
    }
    for (const std::string_view invalid : {"=AAA", "A=AA", "AA=A", "A===", "====", "Zg==Zg=="}) {
        expectMalformed(expectations, invalid, CanonicalBase64Error::InvalidPadding,
                        "padding is restricted to the canonical final positions");
    }
    for (const std::string_view invalid : {"Zm$v", "Zm-v", "Zm_v", "Zm v", "Zm\nv", "Zm\tv"}) {
        expectMalformed(expectations, invalid, CanonicalBase64Error::InvalidAlphabet,
                        "only the standard alphabet is accepted without whitespace");
    }
    const std::array embeddedNul{'Z', 'm', '\0', 'v'};
    expectMalformed(expectations, std::string_view(embeddedNul.data(), embeddedNul.size()),
                    CanonicalBase64Error::InvalidAlphabet, "embedded NUL is not an alphabet byte");
    const std::array nonAscii{'Z', 'm', static_cast<char>(0xFF), 'v'};
    expectMalformed(expectations, std::string_view(nonAscii.data(), nonAscii.size()),
                    CanonicalBase64Error::InvalidAlphabet,
                    "non-ASCII input is not an alphabet byte");

    for (const std::string_view invalid : {"Zh==", "Zm9="}) {
        expectMalformed(expectations, invalid, CanonicalBase64Error::NonZeroTailBits,
                        "unused bits in a padded final sextet must be zero");
    }
}

void testTailBitCanonicality(Expectations& expectations) {
    for (std::size_t value = 0; value < base64Alphabet.size(); ++value) {
        const std::array twoPadding{'A', base64Alphabet[value], '=', '='};
        const auto twoPaddingResult = bloom::project::canonicalBase64DecodedSize(
            std::string_view(twoPadding.data(), twoPadding.size()));
        expectations.expect(
            value % 16 == 0 ? twoPaddingResult && *twoPaddingResult.value() == 1
                            : !twoPaddingResult &&
                                  twoPaddingResult.error() == CanonicalBase64Error::NonZeroTailBits,
            "two-padding spellings accept exactly the sextets with four zero tail bits");

        const std::array onePadding{'A', 'A', base64Alphabet[value], '='};
        const auto onePaddingResult = bloom::project::canonicalBase64DecodedSize(
            std::string_view(onePadding.data(), onePadding.size()));
        expectations.expect(
            value % 4 == 0 ? onePaddingResult && *onePaddingResult.value() == 2
                           : !onePaddingResult &&
                                 onePaddingResult.error() == CanonicalBase64Error::NonZeroTailBits,
            "one-padding spellings accept exactly the sextets with two zero tail bits");
    }
}

void testNoPartialWrites(Expectations& expectations) {
    constexpr auto byteSentinel = std::byte{0xA5};
    std::array<std::byte, 3> invalidDestination{};
    invalidDestination.fill(byteSentinel);
    const auto invalid = bloom::project::decodeCanonicalBase64("Zm$v", invalidDestination);
    expectations.expect(!invalid && invalid.error() == CanonicalBase64Error::InvalidAlphabet &&
                            !invalid.requiredSize().has_value() && invalid.bytesWritten() == 0,
                        "invalid input reports no exact decoded size and zero writes");
    expectUntouched(expectations, invalidDestination, byteSentinel,
                    "invalid input leaves the entire destination untouched");

    std::array<std::byte, 2> tooSmall{};
    tooSmall.fill(byteSentinel);
    const auto smallResult = bloom::project::decodeCanonicalBase64("Zm9v", tooSmall);
    expectations.expect(!smallResult &&
                            smallResult.error() == CanonicalBase64Error::OutputSizeMismatch &&
                            smallResult.requiredSize() == 3 && smallResult.bytesWritten() == 0,
                        "a small decode span reports the exact required size without writing");
    expectUntouched(expectations, tooSmall, byteSentinel, "a small decode span remains untouched");

    std::array<std::byte, 4> tooLarge{};
    tooLarge.fill(byteSentinel);
    const auto largeResult = bloom::project::decodeCanonicalBase64("Zm9v", tooLarge);
    expectations.expect(!largeResult &&
                            largeResult.error() == CanonicalBase64Error::OutputSizeMismatch &&
                            largeResult.requiredSize() == 3 && largeResult.bytesWritten() == 0,
                        "a large decode span is also an exact-size mismatch");
    expectUntouched(expectations, tooLarge, byteSentinel, "a large decode span remains untouched");

    constexpr std::array source{std::byte{0x66}};
    std::array<char, 3> shortEncoding{};
    shortEncoding.fill('?');
    const auto shortEncode = bloom::project::encodeCanonicalBase64(source, shortEncoding);
    expectations.expect(!shortEncode &&
                            shortEncode.error() == CanonicalBase64Error::OutputSizeMismatch &&
                            shortEncode.requiredSize() == 4 && shortEncode.bytesWritten() == 0,
                        "a small encode span reports the exact required size without writing");
    expectUntouched(expectations, shortEncoding, '?', "a small encode span remains untouched");

    std::array<char, 5> longEncoding{};
    longEncoding.fill('?');
    const auto longEncode = bloom::project::encodeCanonicalBase64(source, longEncoding);
    expectations.expect(!longEncode &&
                            longEncode.error() == CanonicalBase64Error::OutputSizeMismatch &&
                            longEncode.requiredSize() == 4 && longEncode.bytesWritten() == 0,
                        "a large encode span is also an exact-size mismatch");
    expectUntouched(expectations, longEncoding, '?', "a large encode span remains untouched");
}

void testSizeBoundaries(Expectations& expectations) {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    constexpr auto maximumCompleteGroups = maximum / 4;
    constexpr auto largestEncodableInput = maximumCompleteGroups * 3;
    const auto largest = bloom::project::canonicalBase64EncodedSize(largestEncodableInput);
    expectations.expect(largest && *largest.value() == maximumCompleteGroups * 4,
                        "the largest exactly representable encoded size succeeds");
    expectSizeFailure(expectations,
                      bloom::project::canonicalBase64EncodedSize(largestEncodableInput + 1),
                      CanonicalBase64Error::SizeOverflow,
                      "the first unrepresentable encoded size fails before arithmetic wraps");
    expectSizeFailure(expectations, bloom::project::canonicalBase64EncodedSize(maximum),
                      CanonicalBase64Error::SizeOverflow,
                      "the maximum size_t input fails without wrapped group arithmetic");

    const auto emptyEncoded = bloom::project::canonicalBase64EncodedSize(0);
    const auto emptyDecoded = bloom::project::canonicalBase64DecodedSize("");
    expectations.expect(emptyEncoded && *emptyEncoded.value() == 0 && emptyDecoded &&
                            *emptyDecoded.value() == 0,
                        "empty payload preflights to empty output in both directions");
    std::span<const std::byte> emptyInput;
    std::span<char> emptyEncodedOutput;
    std::span<std::byte> emptyDecodedOutput;
    const auto encoded = bloom::project::encodeCanonicalBase64(emptyInput, emptyEncodedOutput);
    const auto decoded = bloom::project::decodeCanonicalBase64("", emptyDecodedOutput);
    expectations.expect(encoded && encoded.bytesWritten() == 0 && decoded &&
                            decoded.bytesWritten() == 0,
                        "empty payload operations succeed without touching memory");
}

} // namespace

int main() {
    Expectations expectations;
    testRfcVectors(expectations);
    testEveryByteValue(expectations);
    testMalformedInput(expectations);
    testTailBitCanonicality(expectations);
    testNoPartialWrites(expectations);
    testSizeBoundaries(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
