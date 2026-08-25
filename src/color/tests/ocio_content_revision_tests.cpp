#include <bloom/color/ocio_content_revision.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <type_traits>

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

using bloom::color::OcioContentLocatorKind;
using bloom::color::OcioContentRevisionError;
using bloom::color::OcioContentRevisionResult;

static_assert(bloom::color::kOcioContentRevisionVersion == 1);
static_assert(static_cast<std::uint8_t>(OcioContentLocatorKind::BuiltIn) == 1);
static_assert(static_cast<std::uint8_t>(OcioContentLocatorKind::ProjectRelativeArchive) == 2);
static_assert(static_cast<std::uint8_t>(OcioContentLocatorKind::ExternalArchive) == 3);
static_assert(std::is_nothrow_copy_constructible_v<OcioContentRevisionResult>);

[[nodiscard]] std::span<const std::byte> asBytes(const std::string_view text) noexcept {
    return std::as_bytes(std::span(text.data(), text.size()));
}

[[nodiscard]] bool hasRevision(const OcioContentRevisionResult& result,
                               const std::string_view expected) {
    if (!result || result.error() != OcioContentRevisionError::None) {
        return false;
    }
    const auto* revision = result.revision();
    if (revision == nullptr) {
        return false;
    }
    const auto hex = revision->toLowercaseHex();
    return std::string_view(hex.data(), hex.size()) == expected;
}

template <std::size_t Size>
[[nodiscard]] constexpr std::array<std::byte, Size> sequence() noexcept {
    std::array<std::byte, Size> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::byte>(index & 0xFFU);
    }
    return result;
}

void testIndependentGoldenVectors(Expectations& expectations) {
    const auto empty = bloom::color::computeOcioContentRevisionV1(OcioContentLocatorKind::BuiltIn,
                                                                  std::span<const std::byte>{});
    expectations.expect(
        hasRevision(empty, "0130bd59e88cd27038e6fe4492982dce6b8e37a6a8efb8fa0fec9312034d2800"),
        "the independently generated empty built-in vector matches");

    const auto small =
        bloom::color::computeOcioContentRevisionV1(OcioContentLocatorKind::BuiltIn, asBytes("abc"));
    expectations.expect(
        hasRevision(small, "6572b997987f8d1f6dab66328a7013afd59bf6935b60691d6887768f347dcf37"),
        "the independently generated small built-in vector matches");

    constexpr std::array binary{
        std::byte{0x00}, std::byte{0xFF}, std::byte{0x80},
        std::byte{0x7F}, std::byte{0x00}, std::byte{0x42},
    };
    const auto binaryResult =
        bloom::color::computeOcioContentRevisionV1(OcioContentLocatorKind::ExternalArchive, binary);
    expectations.expect(
        hasRevision(binaryResult,
                    "0e9d9333472f9e1901b7261d85e6a6c60df72e8ab2c49ad0e8ec450b9f35f925"),
        "the independently generated binary archive vector preserves exact bytes");
}

template <std::size_t Size>
void expectBoundaryVector(Expectations& expectations, const std::string_view expected,
                          const std::string_view message) {
    constexpr auto payload = sequence<Size>();
    const auto result = bloom::color::computeOcioContentRevisionV1(
        OcioContentLocatorKind::ProjectRelativeArchive, payload);
    expectations.expect(hasRevision(result, expected), message);
}

void testHashAndLengthBoundaries(Expectations& expectations) {
    // The serialized header is 29 bytes. These payloads place the complete message immediately
    // around SHA-256's 55/56-byte padding boundary and 63/64/65-byte block boundaries.
    expectBoundaryVector<26>(expectations,
                             "8537e351cbce0c4e8c78d81b3dcd1be33bd97591509ac4e369a0a1346a2427e0",
                             "a 55-byte framed message matches its independent vector");
    expectBoundaryVector<27>(expectations,
                             "7c00a08251a364f936b8df4a12193793e5d48ce7976f7ac74ed2a15e966ea4a8",
                             "a 56-byte framed message matches its independent vector");
    expectBoundaryVector<34>(expectations,
                             "11132eb4f7ac1496794997545ba37d5527167bc3e8e4a6b096eaa35eca6e9489",
                             "a 63-byte framed message matches its independent vector");
    expectBoundaryVector<35>(expectations,
                             "fa6e0704c1f925c426ec4fb2fd9fb5d01aa4173edec63fea2988f3e3187d11ee",
                             "a 64-byte framed message matches its independent vector");
    expectBoundaryVector<36>(expectations,
                             "35488d398d09d56942cd996e9ae55e4b53eae0a38f30b5aeabc1968bbb8948d2",
                             "a 65-byte framed message matches its independent vector");

    // These two payloads force the low u64 length bytes across 0x00ff/0x0100.
    expectBoundaryVector<255>(expectations,
                              "0a66ee94e8f6e7a7b60cc7375cbaf13aff694de30c390e2c7b1876393419f9da",
                              "the 255-byte payload count uses unsigned big-endian framing");
    expectBoundaryVector<256>(expectations,
                              "858447f699ee45ea23efb732b579f5c7f206e6ff56ff7f780c3cbf26d256ae72",
                              "the 256-byte payload count uses unsigned big-endian framing");
}

void testDomainTagAndLengthSeparation(Expectations& expectations) {
    const auto builtIn =
        bloom::color::computeOcioContentRevisionV1(OcioContentLocatorKind::BuiltIn, asBytes("abc"));
    const auto projectArchive = bloom::color::computeOcioContentRevisionV1(
        OcioContentLocatorKind::ProjectRelativeArchive, asBytes("abc"));
    const auto externalArchive = bloom::color::computeOcioContentRevisionV1(
        OcioContentLocatorKind::ExternalArchive, asBytes("abc"));

    expectations.expect(
        hasRevision(builtIn, "6572b997987f8d1f6dab66328a7013afd59bf6935b60691d6887768f347dcf37") &&
            hasRevision(projectArchive,
                        "abb21926cdc94941a97e053cc5f3d83e75c5416befeccd983a4fe2ce2909a98d") &&
            hasRevision(externalArchive,
                        "e16aa1a86dd0cd268c6b5008336bc1488c117b787e393498ba8f06f0f1499e51") &&
            *builtIn.revision() != *projectArchive.revision() &&
            *builtIn.revision() != *externalArchive.revision() &&
            *projectArchive.revision() != *externalArchive.revision(),
        "the closed locator tag separates identical payloads");

    const auto extended = bloom::color::computeOcioContentRevisionV1(
        OcioContentLocatorKind::BuiltIn, asBytes(std::string_view{"abc\0", 4}));
    expectations.expect(
        hasRevision(extended, "41dac3186f8887e2d3aa3f0429462078f3381691a3d0ffa0ac5e19a0e1c48d09") &&
            *builtIn.revision() != *extended.revision(),
        "the u64 payload length and exact trailing NUL separate payload identities");

    constexpr std::string_view rawPayloadDigest =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    constexpr std::string_view alternateDomainDigest =
        "e4cd33ebbd43a095f32d326749a7824c52cdda301d187140bbc12f8df17b65b5";
    constexpr std::string_view omittedLengthDigest =
        "5cd679e35c1f52da6e64eaffd193a82d92cb13bfe996b58fa5d277f45f54838d";
    const auto builtInHex = builtIn.revision()->toLowercaseHex();
    const std::string_view builtInText(builtInHex.data(), builtInHex.size());
    expectations.expect(
        builtInText != rawPayloadDigest && builtInText != alternateDomainDigest &&
            builtInText != omittedLengthDigest,
        "the revision domain and explicit length separate neighboring hash domains");
}

void testClosedKindsAndTypedFailure(Expectations& expectations) {
    for (const std::uint8_t rawKind : std::array<std::uint8_t, 3>{0, 4, 255}) {
        const auto result = bloom::color::computeOcioContentRevisionV1(
            static_cast<OcioContentLocatorKind>(rawKind), asBytes("payload"));
        expectations.expect(!result && result.revision() == nullptr &&
                                result.error() == OcioContentRevisionError::InvalidLocatorKind,
                            "a locator tag outside the closed version 1 set is rejected");
    }
}

void testCanonicalDigestSurface(Expectations& expectations) {
    const auto result =
        bloom::color::computeOcioContentRevisionV1(OcioContentLocatorKind::BuiltIn, asBytes("abc"));
    if (!result) {
        expectations.expect(false, "a digest is available for canonical text checks");
        return;
    }

    const auto lowercase = result.revision()->toLowercaseHex();
    const std::string_view lowercaseText(lowercase.data(), lowercase.size());
    const auto parsed = bloom::core::Sha256Digest::fromLowercaseHex(lowercaseText);
    expectations.expect(parsed.has_value() && *parsed == *result.revision(),
                        "the revision round-trips through the core lowercase digest type");

    auto uppercase = lowercase;
    std::ranges::transform(uppercase, uppercase.begin(), [](const char character) {
        return character >= 'a' && character <= 'f' ? static_cast<char>(character - 'a' + 'A')
                                                    : character;
    });
    expectations.expect(!bloom::core::Sha256Digest::fromLowercaseHex(
                             std::string_view(uppercase.data(), uppercase.size()))
                             .has_value(),
                        "uppercase revision text is rejected by the canonical core digest type");
}

} // namespace

int main() {
    Expectations expectations;
    testIndependentGoldenVectors(expectations);
    testHashAndLengthBoundaries(expectations);
    testDomainTagAndLengthSeparation(expectations);
    testClosedKindsAndTypedFailure(expectations);
    testCanonicalDigestSurface(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
