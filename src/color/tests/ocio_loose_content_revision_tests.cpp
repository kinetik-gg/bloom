#include <bloom/color/ocio_loose_content_revision.hpp>

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

using bloom::color::OcioLooseContentRevisionError;
using bloom::color::OcioLooseContentRevisionResult;
using bloom::color::OcioLooseResourceView;

static_assert(bloom::color::kOcioLooseContentRevisionVersion == 1);
static_assert(bloom::color::kOcioLooseMaximumEntryCount == 2048);
static_assert(bloom::color::kOcioLooseMaximumKeyBytes == 4096);
static_assert(bloom::color::kOcioLooseMaximumConfigBytes == 8'388'608);
static_assert(bloom::color::kOcioLooseMaximumResourceBytes == 67'108'864);
static_assert(bloom::color::kOcioLooseMaximumAggregateBytes == 268'435'456);
static_assert(std::is_trivially_copyable_v<OcioLooseResourceView>);
static_assert(std::is_nothrow_copy_constructible_v<OcioLooseContentRevisionResult>);

[[nodiscard]] std::span<const std::byte> asBytes(const std::string_view text) noexcept {
    return std::as_bytes(std::span(text.data(), text.size()));
}

[[nodiscard]] constexpr OcioLooseResourceView
entry(const std::string_view key, const std::span<const std::byte> payload = {}) noexcept {
    return {.key = key, .payload = payload};
}

[[nodiscard]] bool hasRevision(const OcioLooseContentRevisionResult& result,
                               const std::string_view expected) {
    if (!result || result.error() != OcioLooseContentRevisionError::None) {
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
[[nodiscard]] constexpr std::array<std::byte, Size> byteSequence() noexcept {
    std::array<std::byte, Size> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::byte>(index & 0xFFU);
    }
    return result;
}

void testIndependentGoldenVectors(Expectations& expectations) {
    constexpr std::array emptyConfig{entry("config.ocio")};
    expectations.expect(
        hasRevision(bloom::color::computeOcioLooseContentRevisionV1(emptyConfig),
                    "32e4991b2ca86b053c9bed41a8f539d9732eeb5f95777e6e76a9a39a79fccfcb"),
        "the independently generated empty-config vector matches");

    constexpr std::string_view configText = "ocio_profile_version: 2\n";
    const std::array textConfig{entry("config.ocio", asBytes(configText))};
    expectations.expect(
        hasRevision(bloom::color::computeOcioLooseContentRevisionV1(textConfig),
                    "fbb6683bd6d85aac7dbfc02ecfe156e4b854139949a6ac77ba61da70460467fc"),
        "the independently generated config-text vector matches");

    constexpr std::array binaryPayload{
        std::byte{0x00}, std::byte{0xFF}, std::byte{0x80}, std::byte{0x00}, std::byte{0x42},
    };
    const std::array configAndLut{entry("config.ocio", asBytes("abc")),
                                  entry("luts/a.spi1d", binaryPayload)};
    expectations.expect(
        hasRevision(bloom::color::computeOcioLooseContentRevisionV1(configAndLut),
                    "751fb5c0c72d9438e82279fe23fff80891c54856c5c9ca2f6c88afce4b251727"),
        "the independently generated binary-resource vector matches");

    constexpr std::string_view unicodeKey = "\xC3\xA9/lut.spi1d";
    const std::array unicodeEntries{entry("config.ocio", asBytes("cfg")),
                                    entry(unicodeKey, asBytes("lut"))};
    expectations.expect(
        hasRevision(bloom::color::computeOcioLooseContentRevisionV1(unicodeEntries),
                    "b6511b512032e186c486f74c00643541862d1f7209500635798ccd511bcd0baa"),
        "the independently generated non-ASCII-key vector matches");
}

void testFramingBoundariesAndDomain(Expectations& expectations) {
    std::array<char, 256> keyBytes{};
    std::ranges::fill(keyBytes, 'a');
    const std::array key255{entry(std::string_view(keyBytes.data(), 255), asBytes("x")),
                            entry("config.ocio")};
    const std::array key256{entry(std::string_view(keyBytes.data(), 256), asBytes("x")),
                            entry("config.ocio")};
    expectations.expect(
        hasRevision(bloom::color::computeOcioLooseContentRevisionV1(key255),
                    "0c110fbd58cc1cbc37752713ab5ba0934bb006a017e0a03d7b00fb3f140f9162") &&
            hasRevision(bloom::color::computeOcioLooseContentRevisionV1(key256),
                        "cdbd3e32367fb411d710de0a4fb3ae747da33298faa462689818b9733b546274"),
        "u32 key lengths distinguish 255 and 256 bytes");

    constexpr auto payload = byteSequence<256>();
    const std::array payload255{entry("config.ocio", std::span(payload).first(255))};
    const std::array payload256{entry("config.ocio", std::span(payload))};
    expectations.expect(
        hasRevision(bloom::color::computeOcioLooseContentRevisionV1(payload255),
                    "30412ca5c8e2d0dfbefacd2868e975580de5594234b7879c49fe9ab8a83c5eea") &&
            hasRevision(bloom::color::computeOcioLooseContentRevisionV1(payload256),
                        "75596406aff3af16690116bc8370ba87f42a3e3fdfa0c2969d71694ab0937737"),
        "u64 payload lengths distinguish 255 and 256 bytes");

    constexpr std::array emptyConfig{entry("config.ocio")};
    const auto result = bloom::color::computeOcioLooseContentRevisionV1(emptyConfig);
    if (!result) {
        expectations.expect(false, "the domain-separation fixture computes a revision");
        return;
    }
    const auto hex = result.revision()->toLowercaseHex();
    const std::string_view actual(hex.data(), hex.size());
    expectations.expect(
        actual != "0dd65e4e9fd0a977a17489dad830aebc47857630950a2a4f77339286d057bca0" &&
            actual != "1b9ac18c76149ec40ebfe5179904c078b5ec2d05ba8e101ff67af5c0854918b3" &&
            actual != "513925df1c0678d97f08e35579a28250ee654c319001362b2ff8d270538d4c85" &&
            actual != "fba541c8356c8b2e44b6cbd4df424655c29f6b58b15f1ff547e04f1ed74f32f5",
        "the NUL-terminated domain, version, count, and loose domain are all identity-bearing");
}

void testKeyValidation(Expectations& expectations) {
    const std::array invalidStructuralKeys{
        std::string_view{},
        std::string_view{"/absolute"},
        std::string_view{"trailing/"},
        std::string_view{"empty//component"},
        std::string_view{"."},
        std::string_view{".."},
        std::string_view{"./child"},
        std::string_view{"parent/../child"},
        std::string_view{"back\\slash"},
        std::string_view{"C:relative"},
        std::string_view{"c:/absolute"},
        std::string_view{"nul\0byte", 8},
    };
    for (const auto key : invalidStructuralKeys) {
        const std::array entries{entry(key), entry("config.ocio")};
        const auto result = bloom::color::computeOcioLooseContentRevisionV1(entries);
        expectations.expect(!result && result.error() ==
                                           OcioLooseContentRevisionError::InvalidKeyStructure,
                            "a structurally noncanonical root-relative key is rejected");
    }

    const std::array invalidUtf8Keys{
        std::string_view{"\x80", 1},
        std::string_view{"\xC0\x80", 2},
        std::string_view{"\xE2\x82", 2},
        std::string_view{"\xED\xA0\x80", 3},
        std::string_view{"\xF4\x90\x80\x80", 4},
    };
    for (const auto key : invalidUtf8Keys) {
        const std::array entries{entry(key), entry("config.ocio")};
        const auto result = bloom::color::computeOcioLooseContentRevisionV1(entries);
        expectations.expect(!result &&
                                result.error() == OcioLooseContentRevisionError::InvalidKeyUtf8,
                            "a key containing invalid Unicode-scalar UTF-8 is rejected");
    }
}

void testOrderingUniquenessAndConfig(Expectations& expectations) {
    constexpr std::array duplicate{entry("config.ocio"), entry("config.ocio")};
    const auto duplicateResult = bloom::color::computeOcioLooseContentRevisionV1(duplicate);
    expectations.expect(!duplicateResult &&
                            duplicateResult.error() == OcioLooseContentRevisionError::DuplicateKey,
                        "duplicate keys are rejected distinctly");

    constexpr std::array outOfOrder{entry("config.ocio"), entry("z.lut"), entry("a.lut")};
    const auto orderResult = bloom::color::computeOcioLooseContentRevisionV1(outOfOrder);
    expectations.expect(!orderResult && orderResult.error() ==
                                            OcioLooseContentRevisionError::KeysNotStrictlyOrdered,
                        "out-of-order keys are rejected");

    constexpr std::string_view nonAscii = "\xC3\xA9.lut";
    const std::array signedCharTrap{entry(nonAscii), entry("config.ocio")};
    const auto signedCharResult = bloom::color::computeOcioLooseContentRevisionV1(signedCharTrap);
    expectations.expect(!signedCharResult &&
                            signedCharResult.error() ==
                                OcioLooseContentRevisionError::KeysNotStrictlyOrdered,
                        "ordering compares UTF-8 bytes as unsigned octets");

    constexpr std::array missing{entry("Config.ocio")};
    constexpr std::array<OcioLooseResourceView, 0> empty{};
    const auto missingResult = bloom::color::computeOcioLooseContentRevisionV1(missing);
    const auto emptyResult = bloom::color::computeOcioLooseContentRevisionV1(empty);
    expectations.expect(
        !missingResult && missingResult.error() == OcioLooseContentRevisionError::MissingConfig &&
            !emptyResult && emptyResult.error() == OcioLooseContentRevisionError::MissingConfig,
        "config.ocio presence is exact and mandatory");
}

void testResolverQualificationIsNotClaimed(Expectations& expectations) {
    constexpr std::string_view decomposed = "e\xCC\x81.lut";
    const std::array decomposedEntries{entry("config.ocio"), entry(decomposed)};
    constexpr std::array caseFoldCollision{entry("A.lut"), entry("a.lut"), entry("config.ocio")};
    constexpr std::array filesystemSensitive{entry("config.ocio"), entry("dir/file:stream")};

    expectations.expect(
        bloom::color::computeOcioLooseContentRevisionV1(decomposedEntries) &&
            bloom::color::computeOcioLooseContentRevisionV1(caseFoldCollision) &&
            bloom::color::computeOcioLooseContentRevisionV1(filesystemSensitive),
        "the codec does not pretend to qualify NFC, case-fold, or filesystem safety");
}

void testClosedCeilings(Expectations& expectations) {
    std::array<OcioLooseResourceView, bloom::color::kOcioLooseMaximumEntryCount + 1> tooMany{};
    const auto tooManyResult = bloom::color::computeOcioLooseContentRevisionV1(tooMany);
    expectations.expect(!tooManyResult &&
                            tooManyResult.error() ==
                                OcioLooseContentRevisionError::EntryCountLimitExceeded,
                        "the entry-count ceiling rejects one over before inspecting entries");

    std::array<std::array<char, 4>, bloom::color::kOcioLooseMaximumEntryCount - 1> entryKeys{};
    std::array<OcioLooseResourceView, bloom::color::kOcioLooseMaximumEntryCount> maximumEntries{};
    for (std::size_t index = 0; index < entryKeys.size(); ++index) {
        auto value = index;
        for (std::size_t digit = entryKeys[index].size(); digit > 0; --digit) {
            entryKeys[index][digit - 1] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
        maximumEntries[index] =
            entry(std::string_view(entryKeys[index].data(), entryKeys[index].size()));
    }
    maximumEntries.back() = entry("config.ocio");
    expectations.expect(
        static_cast<bool>(bloom::color::computeOcioLooseContentRevisionV1(maximumEntries)),
        "the exact entry-count ceiling is accepted");

    std::array<char, bloom::color::kOcioLooseMaximumKeyBytes + 1> longKey{};
    std::ranges::fill(longKey, 'z');
    const std::array maximumKey{
        entry("config.ocio"),
        entry(std::string_view(longKey.data(), bloom::color::kOcioLooseMaximumKeyBytes))};
    const std::array oversizedKey{entry("config.ocio"),
                                  entry(std::string_view(longKey.data(), longKey.size()))};
    expectations.expect(
        static_cast<bool>(bloom::color::computeOcioLooseContentRevisionV1(maximumKey)),
        "the exact key-byte ceiling is accepted");
    const auto oversizedKeyResult = bloom::color::computeOcioLooseContentRevisionV1(oversizedKey);
    expectations.expect(!oversizedKeyResult &&
                            oversizedKeyResult.error() ==
                                OcioLooseContentRevisionError::KeyByteCountLimitExceeded,
                        "the key-byte ceiling rejects one over");

    static const std::array<std::byte, bloom::color::kOcioLooseMaximumConfigBytes + 1>
        configBytes{};
    const std::array maximumConfig{entry(
        "config.ocio", std::span(configBytes).first(bloom::color::kOcioLooseMaximumConfigBytes))};
    const std::array oversizedConfig{entry("config.ocio", std::span(configBytes))};
    expectations.expect(
        static_cast<bool>(bloom::color::computeOcioLooseContentRevisionV1(maximumConfig)),
        "the exact loose-config byte ceiling is accepted");
    const auto oversizedConfigResult =
        bloom::color::computeOcioLooseContentRevisionV1(oversizedConfig);
    expectations.expect(!oversizedConfigResult &&
                            oversizedConfigResult.error() ==
                                OcioLooseContentRevisionError::ConfigByteCountLimitExceeded,
                        "the loose-config byte ceiling rejects one over");

    static const std::array<std::byte, bloom::color::kOcioLooseMaximumResourceBytes + 1>
        resourceBytes{};
    const auto maximumPayload =
        std::span(resourceBytes).first(bloom::color::kOcioLooseMaximumResourceBytes);
    const std::array maximumAggregate{
        entry("a.lut", maximumPayload), entry("b.lut", maximumPayload), entry("config.ocio"),
        entry("d.lut", maximumPayload), entry("e.lut", maximumPayload)};
    expectations.expect(
        static_cast<bool>(bloom::color::computeOcioLooseContentRevisionV1(maximumAggregate)),
        "exact per-resource and aggregate byte ceilings are accepted");

    const std::array oversizedResource{entry("config.ocio"),
                                       entry("d.lut", std::span(resourceBytes))};
    const auto oversizedResourceResult =
        bloom::color::computeOcioLooseContentRevisionV1(oversizedResource);
    expectations.expect(!oversizedResourceResult &&
                            oversizedResourceResult.error() ==
                                OcioLooseContentRevisionError::ResourceByteCountLimitExceeded,
                        "the per-resource byte ceiling rejects one over");

    constexpr std::array<std::byte, 1> oneByte{};
    const std::array oversizedAggregate{
        entry("a.lut", maximumPayload), entry("b.lut", maximumPayload), entry("config.ocio"),
        entry("d.lut", maximumPayload), entry("e.lut", maximumPayload), entry("f.lut", oneByte)};
    const auto oversizedAggregateResult =
        bloom::color::computeOcioLooseContentRevisionV1(oversizedAggregate);
    expectations.expect(!oversizedAggregateResult &&
                            oversizedAggregateResult.error() ==
                                OcioLooseContentRevisionError::AggregateByteCountLimitExceeded,
                        "the aggregate byte ceiling rejects one over before hashing");
}

} // namespace

int main() {
    Expectations expectations;
    testIndependentGoldenVectors(expectations);
    testFramingBoundariesAndDomain(expectations);
    testKeyValidation(expectations);
    testOrderingUniquenessAndConfig(expectations);
    testResolverQualificationIsNotClaimed(expectations);
    testClosedCeilings(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
