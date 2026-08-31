#include <bloom/host/bloom_neutral_profile.hpp>

#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Offline drift guard for the immutable Bloom Neutral v1 built-in OCIO asset (design decision 3
// of the task package for issue #60, corrected per supervisor review to verify the doc-normative
// formula rather than a plain payload hash): reads assets/ocio/neutral-v1/config.ocio from the
// repository source tree, builds the exact "OCIO Content Revision Version 1" built-in envelope
// from docs/architecture/color-management.md --
//   SHA-256("BloomOcioRevision\0" || u16(1) || u8(locatorKind) || u64(payloadByteCount) ||
//           exactPayloadBytes)
// with locatorKind 1 (immutable built-in) and payloadByteCount the asset's exact size -- hashes
// that envelope through the repository's own core::Sha256 surface, and asserts equality with the
// frozen bloom::host::kBloomNeutralV1ConfigDigest constant. This verifies the FORMULA (domain
// string, big-endian integer packing, locatorKind, byte count, payload), not just that the raw
// asset bytes hash to something: a formula regression (wrong locatorKind, wrong endianness, a
// dropped domain byte) would be caught even if the asset bytes themselves never changed.
// Also pins the two exact Color Interop Forum colorspace names as a cheap textual check; full
// OCIO parsing/validation is the runtime epic's job, not this package's (see
// docs/architecture/color-management.md and assets/ocio/neutral-v1/provenance.md).
//
// Locates the repository root the same way tools/quality/repository_checks and
// tools/quality/dependency_artifact_checks locate repository files: an explicit --root argument
// supplied by CMake as ${PROJECT_SOURCE_DIR} (see src/host/CMakeLists.txt), not an ambient
// working directory or environment variable.

namespace {

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

[[nodiscard]] std::optional<std::vector<std::byte>> readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::vector<char> chars((std::istreambuf_iterator<char>(stream)),
                            std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    for (std::size_t index = 0; index < chars.size(); ++index) {
        bytes[index] = static_cast<std::byte>(chars[index]);
    }
    return bytes;
}

[[nodiscard]] std::string_view asStringView(const std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void appendBytes(std::vector<std::byte>& out, const std::string_view text) {
    for (const char character : text) {
        out.push_back(static_cast<std::byte>(character));
    }
}

void appendByte(std::vector<std::byte>& out, const std::uint8_t value) {
    out.push_back(static_cast<std::byte>(value));
}

void appendU16BigEndian(std::vector<std::byte>& out, const std::uint16_t value) {
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::byte>(value & 0xFFU));
}

void appendU64BigEndian(std::vector<std::byte>& out, const std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

// Builds docs/architecture/color-management.md's "OCIO Content Revision Version 1" built-in
// envelope verbatim:
//   "BloomOcioRevision\0" || u16(1) || u8(locatorKind) || u64(payloadByteCount) ||
//   exactPayloadBytes
// with locatorKind 1 (immutable built-in), per that section's exact byte layout.
[[nodiscard]] std::vector<std::byte>
buildBuiltInOcioRevisionEnvelope(const std::span<const std::byte> payload) {
    std::vector<std::byte> envelope;
    envelope.reserve(18 + 2 + 1 + 8 + payload.size());
    appendBytes(envelope, "BloomOcioRevision");
    appendByte(envelope, 0x00); // the domain string's explicit NUL terminator byte
    appendU16BigEndian(envelope, 1);
    constexpr std::uint8_t kBuiltInLocatorKind = 1;
    appendByte(envelope, kBuiltInLocatorKind);
    appendU64BigEndian(envelope, payload.size());
    envelope.insert(envelope.end(), payload.begin(), payload.end());
    return envelope;
}

void testBloomNeutralAssetDigestMatchesFrozenConstant(Expectations& expectations,
                                                      const std::filesystem::path& repositoryRoot) {
    const auto assetPath = repositoryRoot / "assets" / "ocio" / "neutral-v1" / "config.ocio";
    const auto bytes = readFile(assetPath);
    expectations.expect(bytes.has_value(), "the Bloom Neutral v1 asset file is readable");
    if (!bytes.has_value()) {
        return;
    }

    const auto envelope = buildBuiltInOcioRevisionEnvelope(*bytes);
    const auto computed = bloom::core::Sha256Hasher::hash(envelope);
    expectations.expect(computed.has_value(), "the envelope hashes within SHA-256's message limit");
    if (!computed.has_value()) {
        return;
    }

    expectations.expect(
        *computed == bloom::host::kBloomNeutralV1ConfigDigest,
        "the doc-normative BloomOcioRevision envelope built from the checked-in asset bytes "
        "(domain string, u16(1), locatorKind 1, u64 payload length, exact payload) hashes to "
        "exactly kBloomNeutralV1ConfigDigest -- asset, formula, and constant have not drifted "
        "apart");

    const auto text = asStringView(*bytes);
    expectations.expect(
        text.find(bloom::document::kProcessColorSpaceIdV1) != std::string_view::npos,
        "the asset text contains the exact process Color Interop ID colorspace name "
        "(lin_rec709_scene)");
    expectations.expect(text.find("srgb_rec709_display") != std::string_view::npos,
                        "the asset text contains the exact display/output Color Interop ID "
                        "colorspace name (srgb_rec709_display)");
}

// Issue #95 decision 2 ("the digest lives in ONE place"): bloom::host::kBloomNeutralV1ConfigDigest
// now re-points to bloom::color::kBloomNeutralV1ConfigDigest, and this header re-declares the
// locator URI as bloom::color::kBloomNeutralV1ConfigUri rather than including
// bloom/document/color_settings.hpp from bloom_color (which would introduce a color -> document
// module edge). This test, which already links both bloom_document and (transitively, via
// bloom_neutral_profile.hpp) bloom_color, is where the two independently spelled URI constants are
// cross-checked for exact byte equality so they can never silently drift apart.
void testBloomNeutralUriConstantsAgree(Expectations& expectations) {
    expectations.expect(
        bloom::color::kBloomNeutralV1ConfigUri == bloom::document::kBloomNeutralConfigUriV1,
        "bloom::color::kBloomNeutralV1ConfigUri and bloom::document::kBloomNeutralConfigUriV1 "
        "are byte-identical");
}

} // namespace

int main(int argumentCount, char** arguments) {
    std::filesystem::path repositoryRoot = std::filesystem::current_path();
    for (int index = 1; index < argumentCount; ++index) {
        const std::string_view argument{arguments[index]};
        if (argument == "--root" && index + 1 < argumentCount) {
            repositoryRoot = arguments[++index];
        }
    }

    Expectations expectations;
    testBloomNeutralAssetDigestMatchesFrozenConstant(expectations, repositoryRoot);
    testBloomNeutralUriConstantsAgree(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
