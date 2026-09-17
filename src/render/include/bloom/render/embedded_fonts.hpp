#ifndef BLOOM_RENDER_EMBEDDED_FONTS_HPP
#define BLOOM_RENDER_EMBEDDED_FONTS_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include <bloom/core/sha256.hpp>

namespace bloom::render {

// The faces the CPU reference text path can rasterize, embedded as build-time constant byte arrays
// by bloom_render_fonts (src/render/CMakeLists.txt's file(READ ... HEX) embed of the vendored
// TTFs).
//
// Qt-free by construction: src/render may not use Qt (AGENTS.md), so the bytes cannot come from the
// interface toolkit's font database, a toolkit resource bundle, or any font-service lookup. They
// are also never read from the filesystem at runtime -- an evaluator that had to find a file on
// disk would not be reproducible across machines, and a render task must not perform media I/O.
// This mirrors the existing configure-time embed of the Bloom Neutral v1 OCIO config
// (src/color/ocio_builtin_payload.inc.in).
//
// Provenance: the embedded bytes are exactly the vendored file recorded in
// src/ui/kit/third_party/{dejavu-sans,inter}/provenance.md, whose licenses permit redistribution
// inside Bloom. The build asserts each file's exact byte count so a silent asset change cannot
// reach the embed. No second copy of a face is checked in: src/ui keeps the assets for Qt's font
// database and src/render embeds the same files.
//
// src/render does not depend on src/ui: this is a build-time read of one checked-in asset file, not
// a module dependency. No src/render translation unit includes a src/ui header, and bloom_render
// links nothing from src/ui.

enum class EmbeddedFace : std::uint8_t {
    DejaVuSans,
    InterRegular,
    InterMedium,
    InterSemiBold,
};

struct ExternalFontFile final {
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    core::Sha256Digest contentDigest;
    std::uint32_t faceIndex = 0;

    friend bool operator==(const ExternalFontFile&, const ExternalFontFile&) = default;
};

using TextFont = std::variant<EmbeddedFace, ExternalFontFile>;

// Identifies an embedded face in diagnostics, the text-raster contract, and the text-source's
// durable font enumeration. The order is the order of the document's closed face mapping.
inline constexpr std::string_view kEmbeddedDejaVuSansFamilyName = "DejaVu Sans";
inline constexpr std::string_view kEmbeddedDejaVuSansStyleName = "Book";
inline constexpr std::string_view kEmbeddedInterFamilyName = "Inter";
inline constexpr std::string_view kEmbeddedInterRegularStyleName = "Regular";
inline constexpr std::string_view kEmbeddedInterMediumStyleName = "Medium";
inline constexpr std::string_view kEmbeddedInterSemiBoldStyleName = "SemiBold";

// Immutable for the process lifetime; the span always refers to the same static storage.
[[nodiscard]] std::span<const std::uint8_t> embeddedFaceBytes(EmbeddedFace face) noexcept;

} // namespace bloom::render

#endif // BLOOM_RENDER_EMBEDDED_FONTS_HPP
