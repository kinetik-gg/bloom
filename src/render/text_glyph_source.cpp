#include "text_glyph_source.hpp"

#include <bloom/render/embedded_fonts.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// The ONE translation unit in this repository that instantiates stb_truetype. Everything in this
// file exists to keep that instantiation here:
//
//   * STBTT_STATIC gives every stb symbol internal linkage, so nothing from the vendored header
//     reaches bloom_render's symbol table, any Bloom public header, or any other translation unit.
//     It is also why the implementation and its only caller must share this file.
//   * STBTT_assert is defined away. Upstream's asserts fire on malformed font data; Bloom's font
//     bytes are a digest-recorded compile-time constant (dependencies/licenses/stb_truetype/
//     security.md), a release build would compile the asserts out anyway, and aborting inside a
//     render task is not an acceptable failure mode for work that must return a diagnostic.
//   * Allocation stays on stb's defaults (malloc/free via <cstdlib>), which is why <cstdlib> and
//     <cstring> are included before the header rather than relying on its own includes.
//
// This file is compiled as the bloom_render_stb_truetype object library: no
// bloom_enable_warnings(), so no -Wall/-Werror and no clang-tidy, plus an explicit -w. The object
// library exists only to scope that suppression -- its objects link straight into bloom_render, and
// the strict floating-point flags the rest of src/render uses are applied to it too so the
// rasterizer's output stays reproducible.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_assert(x) ((void)0)
#include "third_party/stb_truetype/stb_truetype.h"

namespace bloom::render::detail {
namespace {

struct ParsedFont final {
    std::once_flag once;
    stbtt_fontinfo info{};
    bool parsed = false;
};

struct ParsedExternalFont final {
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    core::Sha256Digest contentDigest;
    std::uint32_t faceIndex = 0;
    stbtt_fontinfo info{};
    bool parsed = false;
};

[[nodiscard]] constexpr std::size_t faceIndex(const EmbeddedFace face) noexcept {
    switch (face) {
    case EmbeddedFace::DejaVuSans:
        return 0;
    case EmbeddedFace::InterRegular:
        return 1;
    case EmbeddedFace::InterMedium:
        return 2;
    case EmbeddedFace::InterSemiBold:
        return 3;
    }
    return 4;
}

// Each registry entry is parsed once, on first use, from its embedded constant. call_once supplies
// the synchronization for the info and parsed flag; after it returns, readers never mutate either.
[[nodiscard]] const ParsedFont* parsedFont(const EmbeddedFace face) noexcept {
    const auto index = faceIndex(face);
    if (index >= 4) {
        return nullptr;
    }
    static std::array<ParsedFont, 4> fonts;
    auto& font = fonts[index];
    std::call_once(font.once, [&font, face] {
        const auto bytes = embeddedFaceBytes(face);
        if (bytes.empty()) {
            return;
        }
        const auto* data = bytes.data();
        const int offset = stbtt_GetFontOffsetForIndex(data, 0);
        if (offset >= 0) {
            font.parsed = stbtt_InitFont(&font.info, data, offset) != 0;
        }
    });
    return &font;
}

[[nodiscard]] const ParsedExternalFont* parsedExternalFont(const ExternalFontFile& font) noexcept {
    if (font.bytes == nullptr || font.bytes->empty())
        return nullptr;
    static std::mutex mutex;
    static std::vector<std::unique_ptr<ParsedExternalFont>> fonts;
    std::lock_guard lock(mutex);
    for (const auto& candidate : fonts) {
        if (candidate->contentDigest == font.contentDigest &&
            candidate->faceIndex == font.faceIndex)
            return candidate.get();
    }
    auto parsed = std::make_unique<ParsedExternalFont>();
    parsed->bytes = font.bytes;
    parsed->contentDigest = font.contentDigest;
    parsed->faceIndex = font.faceIndex;
    const auto* data = parsed->bytes->data();
    const auto offset = stbtt_GetFontOffsetForIndex(data, static_cast<int>(font.faceIndex));
    if (offset >= 0)
        parsed->parsed = stbtt_InitFont(&parsed->info, data, offset) != 0;
    fonts.push_back(std::move(parsed));
    return fonts.back().get();
}

} // namespace

bool embeddedFontIsParsed(const EmbeddedFace face) noexcept {
    const auto* font = parsedFont(face);
    return font != nullptr && font->parsed;
}

float embeddedFontScaleForEmPixelSize(const EmbeddedFace face, const double pixelSize) noexcept {
    const auto* font = parsedFont(face);
    if (font == nullptr || !font->parsed || !std::isfinite(pixelSize) || pixelSize <= 0.0) {
        return 0.0F;
    }
    return stbtt_ScaleForMappingEmToPixels(&font->info, static_cast<float>(pixelSize));
}

FontVerticalMetrics embeddedFontVerticalMetrics(const EmbeddedFace face) noexcept {
    const auto* font = parsedFont(face);
    FontVerticalMetrics metrics;
    if (font == nullptr || !font->parsed) {
        return metrics;
    }
    stbtt_GetFontVMetrics(&font->info, &metrics.ascent, &metrics.descent, &metrics.lineGap);
    return metrics;
}

int embeddedFontGlyphIndex(const EmbeddedFace face, const char32_t codepoint) noexcept {
    const auto* font = parsedFont(face);
    if (font == nullptr || !font->parsed) {
        return 0;
    }
    return stbtt_FindGlyphIndex(&font->info, static_cast<int>(codepoint));
}

GlyphHorizontalMetrics embeddedFontGlyphHorizontalMetrics(const EmbeddedFace face,
                                                          const int glyph) noexcept {
    const auto* font = parsedFont(face);
    GlyphHorizontalMetrics metrics;
    if (font == nullptr || !font->parsed) {
        return metrics;
    }
    stbtt_GetGlyphHMetrics(&font->info, glyph, &metrics.advanceWidth, &metrics.leftSideBearing);
    return metrics;
}

int embeddedFontGlyphKernAdvance(const EmbeddedFace face, const int leftGlyph,
                                 const int rightGlyph) noexcept {
    const auto* font = parsedFont(face);
    if (font == nullptr || !font->parsed) {
        return 0;
    }
    return stbtt_GetGlyphKernAdvance(&font->info, leftGlyph, rightGlyph);
}

GlyphBitmapBox embeddedFontGlyphBitmapBox(const EmbeddedFace face, const int glyph,
                                          const float scaleX, const float scaleY,
                                          const float shiftX, const float shiftY) noexcept {
    const auto* font = parsedFont(face);
    GlyphBitmapBox box;
    if (font == nullptr || !font->parsed) {
        return box;
    }
    stbtt_GetGlyphBitmapBoxSubpixel(&font->info, glyph, scaleX, scaleY, shiftX, shiftY, &box.left,
                                    &box.top, &box.right, &box.bottom);
    return box;
}

void embeddedFontRasterizeGlyph(const EmbeddedFace face, const std::span<std::uint8_t> output,
                                const int width, const int height, const int strideBytes,
                                const float scaleX, const float scaleY, const float shiftX,
                                const float shiftY, const int glyph) noexcept {
    const auto* font = parsedFont(face);
    if (font == nullptr || !font->parsed || width <= 0 || height <= 0 || strideBytes < width) {
        return;
    }
    const auto required = static_cast<std::size_t>(strideBytes) * static_cast<std::size_t>(height);
    if (output.size() < required) {
        return;
    }
    stbtt_MakeGlyphBitmapSubpixel(&font->info, output.data(), width, height, strideBytes, scaleX,
                                  scaleY, shiftX, shiftY, glyph);
}

bool externalFontIsParsed(const ExternalFontFile& font) noexcept {
    const auto* parsed = parsedExternalFont(font);
    return parsed != nullptr && parsed->parsed;
}

float externalFontScaleForEmPixelSize(const ExternalFontFile& font,
                                      const double pixelSize) noexcept {
    const auto* parsed = parsedExternalFont(font);
    if (parsed == nullptr || !parsed->parsed || !std::isfinite(pixelSize) || pixelSize <= 0.0)
        return 0.0F;
    return stbtt_ScaleForMappingEmToPixels(&parsed->info, static_cast<float>(pixelSize));
}

FontVerticalMetrics externalFontVerticalMetrics(const ExternalFontFile& font) noexcept {
    FontVerticalMetrics metrics;
    const auto* parsed = parsedExternalFont(font);
    if (parsed != nullptr && parsed->parsed)
        stbtt_GetFontVMetrics(&parsed->info, &metrics.ascent, &metrics.descent, &metrics.lineGap);
    return metrics;
}

int externalFontGlyphIndex(const ExternalFontFile& font, const char32_t codepoint) noexcept {
    const auto* parsed = parsedExternalFont(font);
    return parsed != nullptr && parsed->parsed
               ? stbtt_FindGlyphIndex(&parsed->info, static_cast<int>(codepoint))
               : 0;
}

GlyphHorizontalMetrics externalFontGlyphHorizontalMetrics(const ExternalFontFile& font,
                                                          const int glyph) noexcept {
    GlyphHorizontalMetrics metrics;
    const auto* parsed = parsedExternalFont(font);
    if (parsed != nullptr && parsed->parsed)
        stbtt_GetGlyphHMetrics(&parsed->info, glyph, &metrics.advanceWidth,
                               &metrics.leftSideBearing);
    return metrics;
}

int externalFontGlyphKernAdvance(const ExternalFontFile& font, const int leftGlyph,
                                 const int rightGlyph) noexcept {
    const auto* parsed = parsedExternalFont(font);
    return parsed != nullptr && parsed->parsed
               ? stbtt_GetGlyphKernAdvance(&parsed->info, leftGlyph, rightGlyph)
               : 0;
}

GlyphBitmapBox externalFontGlyphBitmapBox(const ExternalFontFile& font, const int glyph,
                                          const float scaleX, const float scaleY,
                                          const float shiftX, const float shiftY) noexcept {
    GlyphBitmapBox box;
    const auto* parsed = parsedExternalFont(font);
    if (parsed != nullptr && parsed->parsed)
        stbtt_GetGlyphBitmapBoxSubpixel(&parsed->info, glyph, scaleX, scaleY, shiftX, shiftY,
                                        &box.left, &box.top, &box.right, &box.bottom);
    return box;
}

void externalFontRasterizeGlyph(const ExternalFontFile& font, const std::span<std::uint8_t> output,
                                const int width, const int height, const int strideBytes,
                                const float scaleX, const float scaleY, const float shiftX,
                                const float shiftY, const int glyph) noexcept {
    const auto* parsed = parsedExternalFont(font);
    if (parsed == nullptr || !parsed->parsed || width <= 0 || height <= 0 || strideBytes < width)
        return;
    const auto required = static_cast<std::size_t>(strideBytes) * static_cast<std::size_t>(height);
    if (output.size() < required)
        return;
    stbtt_MakeGlyphBitmapSubpixel(&parsed->info, output.data(), width, height, strideBytes, scaleX,
                                  scaleY, shiftX, shiftY, glyph);
}

} // namespace bloom::render::detail
