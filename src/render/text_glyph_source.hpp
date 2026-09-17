#pragma once

#include <bloom/render/embedded_fonts.hpp>

#include <cstdint>
#include <span>

// The Bloom-owned boundary around the single vendored stb_truetype translation unit
// (text_glyph_source.cpp, compiled as the bloom_render_stb_truetype object library). This header is
// deliberately free of every stb type and every stb include: only that one translation unit sees
// stb_truetype.h, so only that one translation unit has vendored warnings suppressed and clang-tidy
// withheld, and Bloom's own text layout code (text_raster.cpp) stays under the full strict-warning
// and clang-tidy profile while calling through these declarations.
//
// The face registry is immutable after each entry's first-use initialization. Every embedded face
// is parsed lazily once, with one thread-safe once flag per face, from the payload selected by
// bloom/render/embedded_fonts.hpp. Every function below is then a pure read of that parsed face:
// no function mutates shared state after initialization, and the glyf outline path stb uses for
// these faces copies its own working buffers, so concurrent evaluation on task threads is safe.
//
// Units: `glyph` is a glyph index in the embedded face, never a Unicode scalar. Horizontal metrics
// and kerning are in unscaled font design units; scale factors and bitmap boxes are in pixels.

namespace bloom::render::detail {

struct FontVerticalMetrics final {
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
};

struct GlyphHorizontalMetrics final {
    int advanceWidth = 0;
    int leftSideBearing = 0;
};

// A glyph's pixel coverage box relative to the glyph's own pen origin on the baseline, with y
// increasing downward: `top` is negative for ink above the baseline. Empty (right <= left or
// bottom <= top) for a blank glyph such as a space.
struct GlyphBitmapBox final {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

// False only if the embedded font bytes do not parse, which is a build-integrity failure (the bytes
// are a digest-recorded compile-time constant), never a document or user error. Every other
// function here returns zeroed/empty results in that state rather than reading an unparsed face.
[[nodiscard]] bool embeddedFontIsParsed(EmbeddedFace face) noexcept;

// The scale that maps font design units to `pixelSize` pixels per em -- the ordinary "font size in
// pixels" convention. Zero when the face did not parse or `pixelSize` is not finite and positive.
[[nodiscard]] float embeddedFontScaleForEmPixelSize(EmbeddedFace face, double pixelSize) noexcept;

[[nodiscard]] FontVerticalMetrics embeddedFontVerticalMetrics(EmbeddedFace face) noexcept;

// Glyph index for a Unicode scalar, or 0 (.notdef) when the face has no coverage for it. 0 is a
// real, renderable glyph in this face, so an unsupported codepoint draws the missing-glyph box
// rather than disappearing or reading out of range.
[[nodiscard]] int embeddedFontGlyphIndex(EmbeddedFace face, char32_t codepoint) noexcept;

[[nodiscard]] GlyphHorizontalMetrics embeddedFontGlyphHorizontalMetrics(EmbeddedFace face,
                                                                        int glyph) noexcept;

// Kerning adjustment in design units for the ordered pair, 0 when the face has no kern pair.
[[nodiscard]] int embeddedFontGlyphKernAdvance(EmbeddedFace face, int leftGlyph,
                                               int rightGlyph) noexcept;

// `shiftX`/`shiftY` are the sub-pixel fractions of the pen position, in [0, 1).
[[nodiscard]] GlyphBitmapBox embeddedFontGlyphBitmapBox(EmbeddedFace face, int glyph, float scaleX,
                                                        float scaleY, float shiftX,
                                                        float shiftY) noexcept;

// Rasterizes `glyph` into `output` as 8-bit coverage, `width` columns by `height` rows with
// `strideBytes` between rows. Writes nothing when the span is too small for that geometry, when the
// geometry is non-positive, or when the face did not parse. Existing bytes in the covered rectangle
// are overwritten, not blended: the caller owns combining overlapping glyphs.
void embeddedFontRasterizeGlyph(EmbeddedFace face, std::span<std::uint8_t> output, int width,
                                int height, int strideBytes, float scaleX, float scaleY,
                                float shiftX, float shiftY, int glyph) noexcept;

using ExternalFontFile = bloom::render::ExternalFontFile;
[[nodiscard]] bool externalFontIsParsed(const ExternalFontFile& font) noexcept;
[[nodiscard]] float externalFontScaleForEmPixelSize(const ExternalFontFile& font,
                                                    double pixelSize) noexcept;
[[nodiscard]] FontVerticalMetrics
externalFontVerticalMetrics(const ExternalFontFile& font) noexcept;
[[nodiscard]] int externalFontGlyphIndex(const ExternalFontFile& font, char32_t codepoint) noexcept;
[[nodiscard]] GlyphHorizontalMetrics
externalFontGlyphHorizontalMetrics(const ExternalFontFile& font, int glyph) noexcept;
[[nodiscard]] int externalFontGlyphKernAdvance(const ExternalFontFile& font, int leftGlyph,
                                               int rightGlyph) noexcept;
[[nodiscard]] GlyphBitmapBox externalFontGlyphBitmapBox(const ExternalFontFile& font, int glyph,
                                                        float scaleX, float scaleY, float shiftX,
                                                        float shiftY) noexcept;
void externalFontRasterizeGlyph(const ExternalFontFile& font, std::span<std::uint8_t> output,
                                int width, int height, int strideBytes, float scaleX, float scaleY,
                                float shiftX, float shiftY, int glyph) noexcept;

} // namespace bloom::render::detail
