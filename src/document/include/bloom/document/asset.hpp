#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/validation.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bloom::document {
enum class AssetKind : std::uint8_t { Image, Sequence, Audio, Font };
enum class AssetColorSpace : std::uint8_t { Auto, Srgb, Linear, Raw };
enum class AssetAlphaAssociation : std::uint8_t { Straight, Premultiplied };
struct AssetInterpretation {
    AssetColorSpace colorSpace = AssetColorSpace::Auto;
    AssetAlphaAssociation alphaAssociation = AssetAlphaAssociation::Straight;
    friend bool operator==(const AssetInterpretation&, const AssetInterpretation&) = default;
};
struct AssetLocator {
    std::string kind = "file";
    std::string portability = "project-relative";
    std::string path;
    std::string relinkHint;
    friend bool operator==(const AssetLocator&, const AssetLocator&) = default;
};
struct AssetSequenceMember {
    std::int64_t frame = 0;
    AssetLocator locator;
    core::Sha256Digest contentDigest;
    friend bool operator==(const AssetSequenceMember&, const AssetSequenceMember&) = default;
};
struct AssetSequenceManifest {
    std::string pattern;
    std::uint32_t padding = 0;
    std::int64_t first = 0;
    std::int64_t last = 0;
    std::vector<AssetSequenceMember> members;
    std::vector<std::int64_t> gaps;
    friend bool operator==(const AssetSequenceManifest&, const AssetSequenceManifest&) = default;
};
struct AssetRecord {
    AssetId id;
    AssetKind kind = AssetKind::Image;
    AssetLocator locator;
    core::Sha256Digest contentDigest;
    AssetInterpretation interpretation;
    AssetSequenceManifest manifest;
    // Source descriptor captured by import, used without filesystem I/O by artist-facing views.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    std::uint64_t frames = 0;
    core::RationalTime duration{};
    // Font metadata is captured when the artist picks a face. It keeps the Properties and Assets
    // surfaces useful even when the system font later disappears; the renderer still verifies the
    // locator bytes against contentDigest at compile time.
    std::string fontFamily;
    std::string fontStyle;
    std::uint32_t fontIndex = 0;
    [[nodiscard]] ValidationResult validate() const;
    friend bool operator==(const AssetRecord&, const AssetRecord&) = default;
};
} // namespace bloom::document
