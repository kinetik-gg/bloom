#include <bloom/color/display_processor_identity.hpp>

#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace color = bloom::color;

using Error = color::DisplayProcessorIdentityError;
using Input = color::DisplayProcessorIdentityV1InputView;
using LookMode = color::DisplayProcessorLookModeV1;

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

constexpr std::string_view kGoldenHex =
    "426c6f6f6d446973706c617950726f636573736f724964656e746974790000010001020304050607"
    "08090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f00010000000141000000014200000010"
    "6c696e5f7265633730395f7363656e6500000001440000000156010002000000014c000000014d00"
    "000013737267625f7265633730395f646973706c6179000000097265666572656e63650000001f626c"
    "6f6f6d2e636f6c6f722e6f63696f2d6370752d646973706c61792e76310000000e73747261696768"
    "742d7267626138";

[[nodiscard]] consteval std::uint8_t hexNibble(const char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    return static_cast<std::uint8_t>(value - 'a' + 10);
}

template <std::size_t Size>
[[nodiscard]] consteval std::array<std::byte, Size> decodeHex(const std::string_view text) {
    std::array<std::byte, Size> bytes{};
    for (std::size_t index = 0; index < Size; ++index) {
        bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>(
            (hexNibble(text[index * 2]) << 4U) | hexNibble(text[index * 2 + 1])));
    }
    return bytes;
}

constexpr auto kGoldenBytes = decodeHex<208>(kGoldenHex);
constexpr bloom::core::Sha256Digest::Bytes kRevisionBytes{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};
constexpr bloom::core::Sha256Digest::Bytes kGoldenDigestBytes{
    0x2A, 0x90, 0x14, 0x4F, 0xC6, 0x71, 0x6C, 0x56, 0x30, 0x57, 0x97, 0x0B, 0x05, 0x30, 0xD2, 0x99,
    0xAD, 0x1C, 0x66, 0x93, 0x54, 0x30, 0xEA, 0x09, 0xD5, 0x02, 0x65, 0xEC, 0xA9, 0xA6, 0xED, 0xB7,
};

static_assert(kGoldenHex.size() == kGoldenBytes.size() * 2);
static_assert(color::kDisplayProcessorIdentityVersion == 1);
static_assert(static_cast<std::uint8_t>(LookMode::Bypass) == 0);
static_assert(static_cast<std::uint8_t>(LookMode::Ordered) == 1);
static_assert(!std::is_constructible_v<color::DisplayProcessorIdentityV1View,
                                       std::span<const std::byte>, bloom::core::Sha256Digest>);
static_assert(!std::is_constructible_v<color::DisplayProcessorIdentityV1WriteResult, std::size_t>);

template <typename Value>
concept HasRvalueCanonicalBytes = requires(Value&& value) { std::move(value).canonicalBytes(); };

template <typename Value>
concept HasRvalueIdentityPointer = requires(Value&& value) { std::move(value).identity(); };

template <typename Value>
concept HasRvalueExpectedRevision =
    requires(Value&& value) { std::move(value).expectedOcioRevision(); };

template <typename Value>
concept HasRvalueBorrowedView = requires(Value&& value) { std::move(value).borrowedView(); };

template <typename Range>
concept CanParseIdentityRange =
    requires(Range&& range) { color::parseDisplayProcessorIdentityV1(std::forward<Range>(range)); };

template <typename Range>
concept CanAdoptIdentityRange =
    requires(Range&& range) { color::adoptDisplayProcessorIdentityV1(std::forward<Range>(range)); };

static_assert(!HasRvalueCanonicalBytes<color::DisplayProcessorIdentityV1View>);
static_assert(!HasRvalueIdentityPointer<color::DisplayProcessorIdentityV1ParseResult>);
static_assert(!HasRvalueIdentityPointer<color::DisplayProcessorIdentityV1AdoptionResult>);
static_assert(!HasRvalueExpectedRevision<color::DisplayProcessorIdentityV1View>);
static_assert(!HasRvalueCanonicalBytes<color::DisplayProcessorIdentityV1>);
static_assert(!HasRvalueExpectedRevision<color::DisplayProcessorIdentityV1>);
static_assert(!HasRvalueBorrowedView<color::DisplayProcessorIdentityV1>);
static_assert(!CanParseIdentityRange<std::vector<std::byte>>);
static_assert(CanParseIdentityRange<std::vector<std::byte>&>);
static_assert(CanParseIdentityRange<std::span<const std::byte>>);
static_assert(CanAdoptIdentityRange<std::vector<std::byte>>);
static_assert(!CanAdoptIdentityRange<std::vector<std::byte>&>);
static_assert(!std::is_default_constructible_v<color::DisplayProcessorIdentityV1>);
static_assert(!std::is_constructible_v<color::DisplayProcessorIdentityV1, std::vector<std::byte>&&,
                                       bloom::core::Sha256Digest>);
static_assert(!std::is_copy_constructible_v<color::DisplayProcessorIdentityV1>);
static_assert(std::is_nothrow_move_constructible_v<color::DisplayProcessorIdentityV1>);
static_assert(!std::is_default_constructible_v<color::DisplayProcessorIdentityV1AdoptionResult>);
static_assert(!std::is_copy_constructible_v<color::DisplayProcessorIdentityV1AdoptionResult>);

[[nodiscard]] Input goldenInput() noexcept {
    static constexpr std::array contextVariables{
        color::DisplayProcessorContextVariableV1View{"A", "B"},
    };
    static constexpr std::array<std::string_view, 2> looks{"L", "M"};
    return Input{
        .expectedOcioRevision = bloom::core::Sha256Digest::fromBytes(kRevisionBytes),
        .contextVariables = contextVariables,
        .sourceColorSpaceId = color::kDisplayProcessorIdentitySourceColorSpaceId,
        .displayName = "D",
        .viewName = "V",
        .lookMode = LookMode::Ordered,
        .lookNames = looks,
        .outputColorSpaceId = color::kDisplayProcessorIdentityOutputColorSpaceId,
        .qualityId = color::kDisplayProcessorIdentityQualityId,
        .semanticsProfileId = color::kDisplayProcessorIdentitySemanticsProfileId,
        .packingId = color::kDisplayProcessorIdentityPackingId,
    };
}

template <typename Enum> [[nodiscard]] Enum enumWithBits(const std::uint8_t bits) noexcept {
    static_assert(sizeof(Enum) == sizeof(bits));
    Enum result{};
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

[[nodiscard]] bool parseHasError(const std::span<const std::byte> bytes, const Error error,
                                 const std::size_t offset) noexcept {
    const auto result = color::parseDisplayProcessorIdentityV1(bytes);
    return !result && result.identity() == nullptr && result.error() == error &&
           result.errorOffset() == offset;
}

void testIndependentCanonicalAndDigestVectors(Expectations& expectations) {
    std::array<std::byte, kGoldenBytes.size()> encoded{};
    const auto validation = color::validateDisplayProcessorIdentityV1(goldenInput());
    const auto written = color::writeDisplayProcessorIdentityV1(goldenInput(), encoded);
    expectations.expect(validation && validation.error() == Error::None &&
                            validation.requiredByteCount() == kGoldenBytes.size(),
                        "the independently specified fixture has the exact required size");
    expectations.expect(written && written.error() == Error::None &&
                            written.requiredByteCount() == kGoldenBytes.size() &&
                            written.writtenByteCount() == kGoldenBytes.size(),
                        "the canonical fixture writes completely");
    expectations.expect(encoded == kGoldenBytes,
                        "the writer matches the independent hard-coded canonical bytes");

    const auto digest = bloom::core::Sha256Hasher::hash(encoded);
    expectations.expect(digest.has_value() &&
                            *digest == bloom::core::Sha256Digest::fromBytes(kGoldenDigestBytes),
                        "SHA-256 of the canonical bytes matches the independent hard-coded digest");

    const auto parsed = color::parseDisplayProcessorIdentityV1(kGoldenBytes);
    expectations.expect(parsed && parsed.error() == Error::None && parsed.identity() != nullptr &&
                            parsed.identity()->canonicalBytes().data() == kGoldenBytes.data() &&
                            parsed.identity()->canonicalBytes().size() == kGoldenBytes.size() &&
                            parsed.identity()->expectedOcioRevision() ==
                                bloom::core::Sha256Digest::fromBytes(kRevisionBytes),
                        "the typed parser borrows exact canonical bytes and copies the revision");
}

void testTransactionalWrites(Expectations& expectations) {
    constexpr auto sentinel = std::byte{0xA5};
    std::array<std::byte, kGoldenBytes.size() + 12> destination{};
    destination.fill(sentinel);

    auto invalidInput = goldenInput();
    invalidInput.lookMode = enumWithBits<LookMode>(0xFF);
    const auto invalid = color::writeDisplayProcessorIdentityV1(invalidInput, destination);
    expectations.expect(
        !invalid && invalid.error() == Error::InvalidLookMode &&
            std::ranges::all_of(destination,
                                [sentinel](const auto byte) { return byte == sentinel; }),
        "invalid input leaves every destination byte untouched");

    const auto tooSmall = color::writeDisplayProcessorIdentityV1(
        goldenInput(), std::span(destination).first(kGoldenBytes.size() - 1));
    expectations.expect(
        !tooSmall && tooSmall.error() == Error::DestinationTooSmall &&
            tooSmall.requiredByteCount() == kGoldenBytes.size() &&
            std::ranges::all_of(destination,
                                [sentinel](const auto byte) { return byte == sentinel; }),
        "a short destination reports capacity without partial output");

    const auto success = color::writeDisplayProcessorIdentityV1(goldenInput(), destination);
    expectations.expect(
        success && std::equal(kGoldenBytes.begin(), kGoldenBytes.end(), destination.begin()) &&
            std::ranges::all_of(destination.begin() + kGoldenBytes.size(), destination.end(),
                                [sentinel](const auto byte) { return byte == sentinel; }),
        "a successful write changes only its exact canonical prefix");

    std::array<std::byte, kGoldenBytes.size()> aliasedDestination{};
    aliasedDestination.fill(sentinel);
    aliasedDestination[100] = std::byte{'D'};
    const auto beforeAliasAttempt = aliasedDestination;
    auto aliasedInput = goldenInput();
    aliasedInput.displayName =
        std::string_view(reinterpret_cast<const char*>(aliasedDestination.data() + 100), 1);
    const auto aliased = color::writeDisplayProcessorIdentityV1(aliasedInput, aliasedDestination);
    expectations.expect(!aliased && aliased.error() == Error::InputAliasesDestination &&
                            aliasedDestination == beforeAliasAttempt,
                        "input aliasing is rejected before any destination mutation");
}

void testClosedInputValidation(Expectations& expectations) {
    auto input = goldenInput();
    input.lookMode = enumWithBits<LookMode>(2);
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::InvalidLookMode,
                        "an unknown look-mode representation fails closed");

    input = goldenInput();
    input.sourceColorSpaceId = "lin_rec709_display";
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::InvalidSourceColorSpaceId,
                        "the source Color Interop ID is exact");
    input = goldenInput();
    input.outputColorSpaceId = "srgb_rec709_scene";
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::InvalidOutputColorSpaceId,
                        "the output Color Interop ID is exact");
    input = goldenInput();
    input.qualityId = "fast";
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::InvalidQualityId,
                        "the display quality ID is exact");
    input = goldenInput();
    input.semanticsProfileId = "bloom.color.ocio-cpu-display.v2";
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::InvalidSemanticsProfileId,
                        "the qualified semantics profile is exact");
    input = goldenInput();
    input.packingId = "premultiplied-rgba8";
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::InvalidPackingId,
                        "the display packing ID is exact");

    input = goldenInput();
    input.displayName = {};
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::EmptyDisplayName,
                        "display names are nonempty");
    input = goldenInput();
    input.viewName = {};
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::EmptyViewName,
                        "view names are nonempty");

    input = goldenInput();
    input.lookMode = LookMode::Bypass;
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::LookCountMismatch,
                        "bypass mode cannot retain look names");
    input = goldenInput();
    input.lookNames = {};
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::LookCountMismatch,
                        "ordered mode requires at least one look");
    constexpr std::array<std::string_view, 1> emptyLook{{""}};
    input = goldenInput();
    input.lookNames = emptyLook;
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::EmptyLookName,
                        "ordered look names are nonempty");
}

void testContextAndTextValidation(Expectations& expectations) {
    constexpr std::array invalidName{
        color::DisplayProcessorContextVariableV1View{"1SHOT", "A"},
    };
    auto input = goldenInput();
    input.contextVariables = invalidName;
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::InvalidContextName,
                        "context names use the closed ASCII identifier grammar");

    constexpr std::array duplicateNames{
        color::DisplayProcessorContextVariableV1View{"A", "1"},
        color::DisplayProcessorContextVariableV1View{"A", "2"},
    };
    input.contextVariables = duplicateNames;
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::DuplicateContextName,
                        "duplicate context names are distinguished");
    constexpr std::array unorderedNames{
        color::DisplayProcessorContextVariableV1View{"B", "1"},
        color::DisplayProcessorContextVariableV1View{"A", "2"},
    };
    input.contextVariables = unorderedNames;
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::ContextVariablesNotStrictlyOrdered,
                        "context names are strictly byte ordered");

    const std::string oversizedName(color::kDisplayProcessorIdentityMaximumContextNameBytes + 1,
                                    'A');
    const std::array oversizedNameVariable{
        color::DisplayProcessorContextVariableV1View{oversizedName, "value"},
    };
    input.contextVariables = oversizedNameVariable;
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::ContextNameByteCountLimitExceeded,
                        "context-name byte limits are enforced before encoding");

    constexpr std::string_view invalidUtf8{"\xC3", 1};
    constexpr std::string_view embeddedNul{"x\0y", 3};
    constexpr std::string_view validNonAscii{"\xC3\xA9", 2};
    input = goldenInput();
    input.displayName = invalidUtf8;
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::InvalidUtf8,
                        "malformed UTF-8 is rejected");
    input.displayName = embeddedNul;
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::EmbeddedNul,
                        "embedded NUL is rejected");
    input.displayName = validNonAscii;
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::NormalizationUnavailable,
                        "valid non-ASCII text does not claim unimplemented Unicode NFC");

    const std::string oversizedText(color::kDisplayProcessorIdentityMaximumTextBytes + 1, 'x');
    input = goldenInput();
    input.viewName = oversizedText;
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::TextByteCountLimitExceeded,
                        "one text value cannot exceed the closed v1 byte limit");
}

void testCountAndAggregateBounds(Expectations& expectations) {
    std::array<color::DisplayProcessorContextVariableV1View,
               color::kDisplayProcessorIdentityMaximumContextVariables + 1>
        tooManyContexts{};
    auto input = goldenInput();
    input.contextVariables = tooManyContexts;
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::ContextVariableCountLimitExceeded,
                        "the context-variable count ceiling is closed");

    std::array<std::string_view, color::kDisplayProcessorIdentityMaximumLooks + 1> tooManyLooks{};
    input = goldenInput();
    input.lookNames = tooManyLooks;
    expectations.expect(color::validateDisplayProcessorIdentityV1(input).error() ==
                            Error::LookCountLimitExceeded,
                        "the ordered-look count ceiling is closed");

    std::array<std::array<char, 5>, color::kDisplayProcessorIdentityMaximumContextVariables>
        nameStorage{};
    std::array<color::DisplayProcessorContextVariableV1View,
               color::kDisplayProcessorIdentityMaximumContextVariables>
        maximumContexts{};
    const std::string maximumValue(color::kDisplayProcessorIdentityMaximumTextBytes, 'x');
    for (std::size_t index = 0; index < maximumContexts.size(); ++index) {
        nameStorage[index] = {'V', static_cast<char>('0' + ((index / 100) % 10)),
                              static_cast<char>('0' + ((index / 10) % 10)),
                              static_cast<char>('0' + (index % 10)), '\0'};
        maximumContexts[index] = {std::string_view(nameStorage[index].data(), 4), maximumValue};
    }
    std::array<std::string_view, color::kDisplayProcessorIdentityMaximumLooks> maximumLooks{};
    maximumLooks.fill(maximumValue);
    input = goldenInput();
    input.contextVariables = maximumContexts;
    input.lookNames = maximumLooks;

    const auto validation = color::validateDisplayProcessorIdentityV1(input);
    std::vector<std::byte> encoded(validation.requiredByteCount());
    const auto written = color::writeDisplayProcessorIdentityV1(input, encoded);
    const auto parsed = color::parseDisplayProcessorIdentityV1(encoded);
    expectations.expect(validation &&
                            validation.requiredByteCount() <
                                color::kDisplayProcessorIdentityMaximumBytes &&
                            written && parsed,
                        "all component maxima fit, encode, and parse below the total 2 MiB cap");

    const std::vector<std::byte> oversizedIdentity(color::kDisplayProcessorIdentityMaximumBytes +
                                                   1);
    expectations.expect(
        parseHasError(oversizedIdentity, Error::IdentityByteCountLimitExceeded, 0),
        "the parser rejects a record beyond the total cap before inspecting payload bytes");
}

void testMalformedParserInputs(Expectations& expectations) {
    bool allTruncationsRejected = true;
    for (std::size_t size = 0; size < kGoldenBytes.size(); ++size) {
        const auto result =
            color::parseDisplayProcessorIdentityV1(std::span(kGoldenBytes).first(size));
        allTruncationsRejected = allTruncationsRejected && !result &&
                                 result.error() == Error::Truncated && result.errorOffset() == size;
    }
    expectations.expect(allTruncationsRejected,
                        "every canonical-prefix truncation fails at exact end-of-input");

    auto malformed = kGoldenBytes;
    malformed[0] = std::byte{'X'};
    expectations.expect(parseHasError(malformed, Error::InvalidDomain, 0),
                        "a neighboring domain is rejected at its first byte");
    malformed = kGoldenBytes;
    malformed[31] = std::byte{2};
    expectations.expect(parseHasError(malformed, Error::UnsupportedVersion, 30),
                        "an unknown version fails closed");
    malformed = kGoldenBytes;
    malformed[64] = std::byte{1};
    malformed[65] = std::byte{1};
    expectations.expect(parseHasError(malformed, Error::ContextVariableCountLimitExceeded, 64),
                        "a parsed context count beyond the closed ceiling is rejected");
    malformed = kGoldenBytes;
    malformed[69] = std::byte{129};
    expectations.expect(parseHasError(malformed, Error::ContextNameByteCountLimitExceeded, 66),
                        "a parsed context-name declaration cannot exceed its byte ceiling");
    malformed = kGoldenBytes;
    malformed[70] = std::byte{'1'};
    expectations.expect(parseHasError(malformed, Error::InvalidContextName, 70),
                        "a parsed context name uses the exact identifier grammar");
    malformed = kGoldenBytes;
    malformed[106] = std::byte{0xFF};
    expectations.expect(parseHasError(malformed, Error::InvalidLookMode, 106),
                        "an unknown parsed look mode fails closed");
    malformed = kGoldenBytes;
    malformed[108] = std::byte{0};
    expectations.expect(parseHasError(malformed, Error::LookCountMismatch, 107),
                        "ordered mode with zero looks is noncanonical");
    malformed = kGoldenBytes;
    malformed[108] = std::byte{129};
    expectations.expect(parseHasError(malformed, Error::LookCountLimitExceeded, 107),
                        "a parsed look count beyond the closed ceiling is rejected");
    malformed = kGoldenBytes;
    malformed[80] = std::byte{'X'};
    expectations.expect(parseHasError(malformed, Error::InvalidSourceColorSpaceId, 80),
                        "a changed fixed record value is rejected");
    constexpr std::array fixedRecordMutations{
        std::pair{std::size_t{123}, Error::InvalidOutputColorSpaceId},
        std::pair{std::size_t{146}, Error::InvalidQualityId},
        std::pair{std::size_t{159}, Error::InvalidSemanticsProfileId},
        std::pair{std::size_t{194}, Error::InvalidPackingId},
    };
    bool allFixedRecordValuesRejected = true;
    for (const auto [offset, expectedError] : fixedRecordMutations) {
        malformed = kGoldenBytes;
        malformed[offset] = std::byte{'X'};
        allFixedRecordValuesRejected =
            allFixedRecordValuesRejected && parseHasError(malformed, expectedError, offset);
    }
    expectations.expect(allFixedRecordValuesRejected,
                        "every fixed version-1 record value fails closed when changed");

    malformed = kGoldenBytes;
    malformed[100] = std::byte{0};
    expectations.expect(parseHasError(malformed, Error::EmbeddedNul, 100),
                        "parsed text rejects embedded NUL");
    malformed = kGoldenBytes;
    malformed[100] = std::byte{0xC3};
    expectations.expect(parseHasError(malformed, Error::InvalidUtf8, 100),
                        "parsed text rejects malformed UTF-8");
    malformed = kGoldenBytes;
    malformed[96] = std::byte{0};
    malformed[97] = std::byte{0};
    malformed[98] = std::byte{0};
    malformed[99] = std::byte{0};
    expectations.expect(parseHasError(malformed, Error::EmptyDisplayName, 100),
                        "a parsed display name is nonempty");
    malformed = kGoldenBytes;
    malformed[109] = std::byte{0};
    malformed[110] = std::byte{0};
    malformed[111] = std::byte{0};
    malformed[112] = std::byte{0};
    expectations.expect(parseHasError(malformed, Error::EmptyLookName, 113),
                        "a parsed ordered look name is nonempty");

    std::vector<std::byte> nonAscii(kGoldenBytes.begin(), kGoldenBytes.end());
    nonAscii[99] = std::byte{2};
    nonAscii[100] = std::byte{0xC3};
    nonAscii.insert(nonAscii.begin() + 101, std::byte{0xA9});
    expectations.expect(parseHasError(nonAscii, Error::NormalizationUnavailable, 100),
                        "parsed non-ASCII text fails distinctly until NFC is qualified");

    std::vector<std::byte> trailing(kGoldenBytes.begin(), kGoldenBytes.end());
    trailing.push_back(std::byte{0});
    expectations.expect(parseHasError(trailing, Error::TrailingBytes, kGoldenBytes.size()),
                        "a canonical record consumes its complete byte span");
}

void testBorrowedViewLifetimeSurface(Expectations& expectations) {
    std::optional<color::DisplayProcessorIdentityV1View> retainedView;
    {
        const auto parsed = color::parseDisplayProcessorIdentityV1(kGoldenBytes);
        if (parsed.identity() != nullptr) {
            retainedView.emplace(*parsed.identity());
        }
    }
    expectations.expect(retainedView.has_value() &&
                            retainedView->canonicalBytes().data() == kGoldenBytes.data() &&
                            retainedView->canonicalBytes().size() == kGoldenBytes.size() &&
                            retainedView->expectedOcioRevision() ==
                                bloom::core::Sha256Digest::fromBytes(kRevisionBytes),
                        "a copied borrowed view survives its parse result while storage remains");
}

void testOwningAdoptionAndMove(Expectations& expectations) {
    std::vector<std::byte> storage(kGoldenBytes.begin(), kGoldenBytes.end());
    const auto* const originalData = storage.data();
    const auto originalCapacity = storage.capacity();
    auto adopted = color::adoptDisplayProcessorIdentityV1(std::move(storage));
    const auto* const identity = adopted.identity();
    expectations.expect(adopted && adopted.hasIdentity() && !adopted.wasRejected() &&
                            !adopted.identityWasTransferred() && adopted.error() == Error::None &&
                            identity != nullptr &&
                            identity->canonicalBytes().data() == originalData &&
                            identity->canonicalBytes().size() == kGoldenBytes.size() &&
                            identity->canonicalBytes().size() <= originalCapacity,
                        "adoption validates then transfers the caller's exact vector storage");

    const auto borrowed = identity == nullptr ? std::nullopt : identity->borrowedView();
    expectations.expect(borrowed.has_value() && borrowed->canonicalBytes().data() == originalData &&
                            borrowed->canonicalBytes().size() == kGoldenBytes.size() &&
                            borrowed->expectedOcioRevision() ==
                                bloom::core::Sha256Digest::fromBytes(kRevisionBytes),
                        "an owned identity publishes a validated synchronous borrowed view");

    color::DisplayProcessorIdentityV1AdoptionResult movedResult(std::move(adopted));
    // The result type explicitly defines its moved-from state as transferred.
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    const bool movedResultState =
        !adopted.hasIdentity() && !adopted.wasRejected() && adopted.identityWasTransferred();
    expectations.expect(movedResultState && movedResult.hasIdentity() &&
                            !movedResult.wasRejected() && !movedResult.identityWasTransferred(),
                        "moving an adoption result distinguishes source transfer from rejection");

    auto extracted = std::move(movedResult).takeIdentity();
    // takeIdentity has a specified consumed state distinct from validation rejection.
    const bool takenResultState = !movedResult.hasIdentity() && !movedResult.wasRejected() &&
                                  movedResult.identityWasTransferred();
    expectations.expect(extracted.has_value() && extracted->canonicalBytes().data() == originalData,
                        "the typed result transfers ownership without copying bytes");
    expectations.expect(takenResultState,
                        "a taken result has an explicit transferred state, not a false failure");
    if (!extracted.has_value()) {
        return;
    }

    color::DisplayProcessorIdentityV1 retained(std::move(*extracted));
    const auto retainedView = retained.borrowedView();
    // The owner defines moved-from as empty and cannot publish a trusted borrowed view.
    const bool movedOwnerState = !static_cast<bool>(*extracted) &&
                                 extracted->canonicalBytes().empty() &&
                                 !extracted->borrowedView().has_value();
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    expectations.expect(
        retained && retained.canonicalBytes().data() == originalData && retainedView.has_value() &&
            retainedView->canonicalBytes().data() == originalData && movedOwnerState,
        "moving retained ownership preserves storage and its validated view");
}

void testInvalidAdoptionPreservesStorage(Expectations& expectations) {
    std::vector<std::byte> invalid(kGoldenBytes.begin(), kGoldenBytes.end());
    invalid[0] = std::byte{'X'};
    const auto before = invalid;
    const auto* const originalData = invalid.data();
    auto rejected = color::adoptDisplayProcessorIdentityV1(std::move(invalid));
    // Failed adoption explicitly promises not to consume its rvalue-reference argument.
    const bool storagePreserved =
        invalid.data() == originalData && invalid == before; // NOLINT(bugprone-use-after-move)
    expectations.expect(!rejected && !rejected.hasIdentity() && rejected.wasRejected() &&
                            !rejected.identityWasTransferred() && rejected.identity() == nullptr &&
                            rejected.error() == Error::InvalidDomain &&
                            rejected.errorOffset() == 0 && storagePreserved,
                        "invalid adoption returns a typed parse failure without consuming storage");
    const auto extracted = std::move(rejected).takeIdentity();
    expectations.expect(!extracted.has_value(),
                        "a failed adoption cannot manufacture an owning identity");
}

} // namespace

int main() {
    Expectations expectations;
    testIndependentCanonicalAndDigestVectors(expectations);
    testTransactionalWrites(expectations);
    testClosedInputValidation(expectations);
    testContextAndTextValidation(expectations);
    testCountAndAggregateBounds(expectations);
    testMalformedParserInputs(expectations);
    testBorrowedViewLifetimeSurface(expectations);
    testOwningAdoptionAndMove(expectations);
    testInvalidAdoptionPreservesStorage(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
