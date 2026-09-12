#ifndef BLOOM_RENDER_EMBEDDED_FONTS_HPP
#define BLOOM_RENDER_EMBEDDED_FONTS_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::render {

// The one font the CPU reference text path can rasterize today, embedded as a build-time constant
// byte array by bloom_render_fonts (src/render/CMakeLists.txt's file(READ ... HEX) embed of
// src/ui/kit/third_party/dejavu-sans/DejaVuSans.ttf).
//
// Qt-free by construction: src/render may not use Qt (AGENTS.md), so the bytes cannot come from the
// interface toolkit's font database, a toolkit resource bundle, or any font-service lookup. They
// are also never read from the filesystem at runtime -- an evaluator that had to find a file on
// disk would not be reproducible across machines, and a render task must not perform media I/O.
// This mirrors the existing configure-time embed of the Bloom Neutral v1 OCIO config
// (src/color/ocio_builtin_payload.inc.in).
//
// Provenance: the embedded bytes are exactly the vendored file recorded in
// src/ui/kit/third_party/dejavu-sans/provenance.md ("DejaVuSans.ttf", SHA-256
// 7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954, DejaVu 2.37, registers as
// family "DejaVu Sans" style "Book"), whose license permits redistribution inside Bloom. The build
// asserts that file's exact byte count so a silent asset change cannot reach the embed. No second
// copy of the font is checked in: src/ui keeps the asset for Qt's font database and src/render
// embeds the same file.
//
// src/render does not depend on src/ui: this is a build-time read of one checked-in asset file, not
// a module dependency. No src/render translation unit includes a src/ui header, and bloom_render
// links nothing from src/ui.

// Identifies the embedded face in diagnostics and in the text-raster contract. Not a font-selection
// mechanism: bloom.text-source has no font parameter, so there is nothing to select.
inline constexpr std::string_view kEmbeddedDejaVuSansFamilyName = "DejaVu Sans";
inline constexpr std::string_view kEmbeddedDejaVuSansStyleName = "Book";

// Immutable for the process lifetime; the span always refers to the same static storage.
[[nodiscard]] std::span<const std::uint8_t> embeddedDejaVuSansTrueTypeBytes() noexcept;

} // namespace bloom::render

#endif // BLOOM_RENDER_EMBEDDED_FONTS_HPP
