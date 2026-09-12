#include <bloom/render/text_raster.hpp>

#include "text_glyph_source.hpp"

#include <bloom/core/utf8.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace {

using bloom::render::ImageError;
using bloom::render::ImageErrorCode;
using bloom::render::ImageResult;
using bloom::render::TextCoverageBitmap;
using bloom::render::detail::GlyphBitmapBox;

[[nodiscard]] ImageError codeError(const ImageErrorCode code) noexcept {
    return ImageError::codeOnly(code);
}

// One glyph's placement in bitmap space, before the union box is known. Coordinates are relative to
// the text origin (pen start on the ascender line), y increasing downward.
struct GlyphPlacement final {
    int glyph = 0;
    std::int64_t left = 0;
    std::int64_t top = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    float shiftX = 0.0F;
};

// Every placement coordinate is kept inside this bound so the union box, the bitmap extent, and the
// per-glyph destination offsets are all exactly representable and cannot overflow. It is far larger
// than any representable frame and smaller than Int32, so widths and heights narrow safely.
constexpr std::int64_t kCoordinateBound = 1 << 24;

[[nodiscard]] bool withinCoordinateBound(const std::int64_t value) noexcept {
    return value >= -kCoordinateBound && value <= kCoordinateBound;
}

} // namespace

namespace bloom::render {

ImageResult<TextRasterParameters>
TextRasterParameters::create(const double horizontalPixelSize,
                             const double verticalPixelSize) noexcept {
    if (!std::isfinite(horizontalPixelSize) || !std::isfinite(verticalPixelSize) ||
        horizontalPixelSize <= 0.0 || verticalPixelSize <= 0.0 ||
        horizontalPixelSize > kMaximumTextPixelSize || verticalPixelSize > kMaximumTextPixelSize) {
        return ImageResult<TextRasterParameters>::failure(
            codeError(ImageErrorCode::InvalidParameter));
    }
    return ImageResult<TextRasterParameters>::success(
        TextRasterParameters(horizontalPixelSize, verticalPixelSize));
}

TextCoverageBitmap::TextCoverageBitmap(const std::int64_t originX, const std::int64_t originY,
                                       const std::uint32_t width, const std::uint32_t height,
                                       std::vector<std::uint8_t> coverage) noexcept
    : originX_(originX), originY_(originY), width_(width), height_(height),
      coverage_(std::move(coverage)) {}

std::span<const std::uint8_t> TextCoverageBitmap::row(const std::uint32_t y) const& noexcept {
    if (y >= height_ || coverage_.empty()) {
        return {};
    }
    const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
    return std::span<const std::uint8_t>(coverage_).subspan(offset, width_);
}

ImageResult<TextCoverageBitmap>
TextCoverageBitmap::rasterizeEmbeddedDejaVuSans(const std::string_view utf8Content,
                                                const TextRasterParameters parameters,
                                                const std::size_t coverageByteLimit) {
    if (!core::isValidUtf8(utf8Content)) {
        return ImageResult<TextCoverageBitmap>::failure(
            codeError(ImageErrorCode::InvalidParameter));
    }
    if (!detail::embeddedFontIsParsed()) {
        return ImageResult<TextCoverageBitmap>::failure(codeError(ImageErrorCode::InvalidState));
    }

    const auto scaleX = detail::embeddedFontScaleForEmPixelSize(parameters.horizontalPixelSize());
    const auto scaleY = detail::embeddedFontScaleForEmPixelSize(parameters.verticalPixelSize());
    if (scaleX <= 0.0F || scaleY <= 0.0F) {
        return ImageResult<TextCoverageBitmap>::failure(
            codeError(ImageErrorCode::InvalidParameter));
    }

    // The baseline is snapped to a whole row once, for the whole line, and the sub-pixel fraction
    // is spent on the horizontal pen position only: a per-glyph vertical sub-pixel shift would make
    // the same string rasterize differently depending on where the line happened to start, which is
    // the opposite of what a reproducible reference path needs.
    const auto vertical = detail::embeddedFontVerticalMetrics();
    const auto baselineRow = static_cast<std::int64_t>(
        std::lround(static_cast<double>(vertical.ascent) * static_cast<double>(scaleY)));
    if (!withinCoordinateBound(baselineRow)) {
        return ImageResult<TextCoverageBitmap>::failure(
            codeError(ImageErrorCode::ArithmeticOverflow));
    }

    std::vector<GlyphPlacement> placements;
    auto minimumLeft = std::numeric_limits<std::int64_t>::max();
    auto minimumTop = std::numeric_limits<std::int64_t>::max();
    auto maximumRight = std::numeric_limits<std::int64_t>::min();
    auto maximumBottom = std::numeric_limits<std::int64_t>::min();

    double pen = 0.0;
    int previousGlyph = -1;
    std::size_t offset = 0;
    while (offset < utf8Content.size()) {
        const auto scalar = core::decodeUtf8Scalar(utf8Content, offset);
        if (!scalar.isValid()) {
            // isValidUtf8() already accepted these bytes with the same rules, so this is
            // unreachable; it is a hard failure rather than a silent skip so the two can never
            // disagree unnoticed.
            return ImageResult<TextCoverageBitmap>::failure(
                codeError(ImageErrorCode::InvalidState));
        }
        offset += scalar.length;

        const auto glyph = detail::embeddedFontGlyphIndex(scalar.value);
        if (previousGlyph >= 0) {
            pen += static_cast<double>(detail::embeddedFontGlyphKernAdvance(previousGlyph, glyph)) *
                   static_cast<double>(scaleX);
        }
        if (!std::isfinite(pen) ||
            !withinCoordinateBound(static_cast<std::int64_t>(std::floor(pen)))) {
            return ImageResult<TextCoverageBitmap>::failure(
                codeError(ImageErrorCode::ArithmeticOverflow));
        }

        const auto penColumn = static_cast<std::int64_t>(std::floor(pen));
        const auto shiftX = static_cast<float>(pen - static_cast<double>(penColumn));
        const GlyphBitmapBox box =
            detail::embeddedFontGlyphBitmapBox(glyph, scaleX, scaleY, shiftX, 0.0F);
        if (box.right > box.left && box.bottom > box.top) {
            const auto left = penColumn + static_cast<std::int64_t>(box.left);
            const auto top = baselineRow + static_cast<std::int64_t>(box.top);
            const auto right = penColumn + static_cast<std::int64_t>(box.right);
            const auto bottom = baselineRow + static_cast<std::int64_t>(box.bottom);
            if (!withinCoordinateBound(left) || !withinCoordinateBound(top) ||
                !withinCoordinateBound(right) || !withinCoordinateBound(bottom)) {
                return ImageResult<TextCoverageBitmap>::failure(
                    codeError(ImageErrorCode::ArithmeticOverflow));
            }
            placements.push_back({glyph, left, top, static_cast<std::int32_t>(right - left),
                                  static_cast<std::int32_t>(bottom - top), shiftX});
            minimumLeft = std::min(minimumLeft, left);
            minimumTop = std::min(minimumTop, top);
            maximumRight = std::max(maximumRight, right);
            maximumBottom = std::max(maximumBottom, bottom);
        }

        pen += static_cast<double>(detail::embeddedFontGlyphHorizontalMetrics(glyph).advanceWidth) *
               static_cast<double>(scaleX);
        previousGlyph = glyph;
    }

    if (placements.empty()) {
        return ImageResult<TextCoverageBitmap>::success(TextCoverageBitmap::empty());
    }

    const auto width = static_cast<std::uint64_t>(maximumRight - minimumLeft);
    const auto height = static_cast<std::uint64_t>(maximumBottom - minimumTop);
    const auto extent = ImageExtent::create(width, height);
    if (!extent) {
        return ImageResult<TextCoverageBitmap>::failure(*extent.error());
    }
    // One byte per pixel, checked against the budget before a single byte is allocated: a
    // pathological string must be refused, not rasterized and then thrown away.
    const auto coverageBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (coverageBytes > coverageByteLimit) {
        return ImageResult<TextCoverageBitmap>::failure(
            ImageError::pixelStorageBudgetExceeded(coverageBytes, coverageByteLimit));
    }

    std::vector<std::uint8_t> coverage;
    try {
        coverage.assign(coverageBytes, 0);
    } catch (const std::bad_alloc&) {
        return ImageResult<TextCoverageBitmap>::failure(
            ImageError::allocationFailure(coverageBytes));
    }

    const auto stride = static_cast<std::size_t>(width);
    std::vector<std::uint8_t> glyphCoverage;
    for (const auto& placement : placements) {
        const auto glyphWidth = static_cast<std::size_t>(placement.width);
        const auto glyphHeight = static_cast<std::size_t>(placement.height);
        const auto glyphBytes = glyphWidth * glyphHeight;
        try {
            glyphCoverage.assign(glyphBytes, 0);
        } catch (const std::bad_alloc&) {
            return ImageResult<TextCoverageBitmap>::failure(
                ImageError::allocationFailure(glyphBytes));
        }
        detail::embeddedFontRasterizeGlyph(glyphCoverage, placement.width, placement.height,
                                           placement.width, scaleX, scaleY, placement.shiftX, 0.0F,
                                           placement.glyph);

        const auto destinationLeft = static_cast<std::size_t>(placement.left - minimumLeft);
        const auto destinationTop = static_cast<std::size_t>(placement.top - minimumTop);
        for (std::size_t row = 0; row < glyphHeight; ++row) {
            const auto destinationOffset = (destinationTop + row) * stride + destinationLeft;
            for (std::size_t column = 0; column < glyphWidth; ++column) {
                // Maximum, not sum or overwrite: overlapping ink (a kerned pair, an accent over its
                // base letter) must not exceed full coverage, and a later glyph must not erase an
                // earlier one's antialiased edge.
                auto& destination = coverage[destinationOffset + column];
                destination = std::max(destination, glyphCoverage[row * glyphWidth + column]);
            }
        }
    }

    return ImageResult<TextCoverageBitmap>::success(
        TextCoverageBitmap(minimumLeft, minimumTop, extent.value()->width(),
                           extent.value()->height(), std::move(coverage)));
}

} // namespace bloom::render
