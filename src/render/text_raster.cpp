#include <bloom/render/text_raster.hpp>

#include "text_glyph_source.hpp"

#include <bloom/core/utf8.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using bloom::render::EmbeddedFace;
using bloom::render::ImageError;
using bloom::render::ImageErrorCode;
using bloom::render::ImageResult;
using bloom::render::TextCoverageBitmap;
using bloom::render::detail::GlyphBitmapBox;
namespace detail = bloom::render::detail;

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

template <typename Face> [[nodiscard]] bool fontIsParsed(const Face& face) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontIsParsed(face);
    else
        return detail::externalFontIsParsed(face);
}
template <typename Face>
[[nodiscard]] float fontScaleForEmPixelSize(const Face& face, const double size) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontScaleForEmPixelSize(face, size);
    else
        return detail::externalFontScaleForEmPixelSize(face, size);
}
template <typename Face> [[nodiscard]] auto fontVerticalMetrics(const Face& face) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontVerticalMetrics(face);
    else
        return detail::externalFontVerticalMetrics(face);
}
template <typename Face>
[[nodiscard]] int fontGlyphIndex(const Face& face, const char32_t scalar) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontGlyphIndex(face, scalar);
    else
        return detail::externalFontGlyphIndex(face, scalar);
}
template <typename Face>
[[nodiscard]] int fontGlyphKernAdvance(const Face& face, const int left, const int right) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontGlyphKernAdvance(face, left, right);
    else
        return detail::externalFontGlyphKernAdvance(face, left, right);
}
template <typename Face>
[[nodiscard]] auto fontGlyphHorizontalMetrics(const Face& face, const int glyph) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontGlyphHorizontalMetrics(face, glyph);
    else
        return detail::externalFontGlyphHorizontalMetrics(face, glyph);
}
template <typename Face>
[[nodiscard]] auto fontGlyphBitmapBox(const Face& face, const int glyph, const float scaleX,
                                      const float scaleY, const float shiftX,
                                      const float shiftY) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontGlyphBitmapBox(face, glyph, scaleX, scaleY, shiftX, shiftY);
    else
        return detail::externalFontGlyphBitmapBox(face, glyph, scaleX, scaleY, shiftX, shiftY);
}
template <typename Face>
void fontRasterizeGlyph(const Face& face, const std::span<std::uint8_t> output, const int width,
                        const int height, const int stride, const float scaleX, const float scaleY,
                        const float shiftX, const float shiftY, const int glyph) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        detail::embeddedFontRasterizeGlyph(face, output, width, height, stride, scaleX, scaleY,
                                           shiftX, shiftY, glyph);
    else
        detail::externalFontRasterizeGlyph(face, output, width, height, stride, scaleX, scaleY,
                                           shiftX, shiftY, glyph);
}

template <typename Face>
[[nodiscard]] double measureGlyphRun(const Face& face, const std::u32string& run,
                                     const float scaleX, const double letterSpacing) {
    double width = 0.0;
    int previous = -1;
    for (const auto scalar : run) {
        const auto glyph = fontGlyphIndex(face, scalar);
        if (previous >= 0)
            width += static_cast<double>(fontGlyphKernAdvance(face, previous, glyph)) *
                         static_cast<double>(scaleX) +
                     letterSpacing;
        width += static_cast<double>(fontGlyphHorizontalMetrics(face, glyph).advanceWidth) *
                 static_cast<double>(scaleX);
        previous = glyph;
    }
    return width;
}

template <typename Face>
[[nodiscard]] std::string wrappedText(const Face& face, const std::string_view content,
                                      const float scaleX, const double letterSpacing,
                                      const double boxWidth) {
    std::vector<std::u32string> lines;
    std::u32string current;
    for (std::size_t cursor = 0; cursor < content.size();) {
        const auto scalar = bloom::core::decodeUtf8Scalar(content, cursor);
        cursor += scalar.length;
        if (scalar.value == U'\r')
            continue;
        if (scalar.value == U'\n') {
            lines.push_back(std::move(current));
            current = {};
            continue;
        }
        current.push_back(scalar.value);
    }
    lines.push_back(std::move(current));

    std::vector<std::u32string> wrapped;
    for (const auto& source : lines) {
        std::u32string line;
        std::size_t cursor = 0;
        while (cursor < source.size()) {
            while (cursor < source.size() && (source[cursor] == U' ' || source[cursor] == U'\t'))
                ++cursor;
            if (cursor == source.size())
                break;
            auto end = cursor;
            while (end < source.size() && source[end] != U' ' && source[end] != U'\t')
                ++end;
            const std::u32string word = source.substr(cursor, end - cursor);
            std::u32string candidate = line;
            if (!candidate.empty())
                candidate.push_back(U' ');
            candidate += word;
            if (!line.empty() &&
                measureGlyphRun(face, candidate, scaleX, letterSpacing) > boxWidth) {
                wrapped.push_back(std::move(line));
                line = word;
            } else if (line.empty() &&
                       measureGlyphRun(face, word, scaleX, letterSpacing) > boxWidth) {
                for (const auto scalar : word) {
                    std::u32string one{scalar};
                    if (!line.empty() &&
                        measureGlyphRun(face, line + one, scaleX, letterSpacing) > boxWidth) {
                        wrapped.push_back(std::move(line));
                        line = {};
                    }
                    line += scalar;
                }
            } else {
                line = std::move(candidate);
            }
            cursor = end;
        }
        wrapped.push_back(std::move(line));
    }

    auto appendScalar = [](std::string& output, const char32_t scalar) {
        if (scalar <= 0x7FU) {
            output.push_back(static_cast<char>(scalar));
        } else if (scalar <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (scalar >> 6U)));
            output.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
        } else if (scalar <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (scalar >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (scalar >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
        }
    };
    std::string result;
    for (std::size_t index = 0; index < wrapped.size(); ++index) {
        for (const auto scalar : wrapped[index])
            appendScalar(result, scalar);
        if (index + 1 < wrapped.size())
            result.push_back('\n');
    }
    return result;
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

template <typename Face>
ImageResult<TextCoverageBitmap>
rasterizeFontText(const Face& face, const std::string_view utf8Content,
                  const TextRasterParameters parameters, const std::size_t coverageByteLimit,
                  const TextLayoutOptions layout, std::vector<Path>* outlines = nullptr,
                  const PathCancellation& cancelled = {}) {
    if (!core::isValidUtf8(utf8Content) || !std::isfinite(layout.lineHeight) ||
        layout.lineHeight <= 0.0 || !std::isfinite(layout.letterSpacing) ||
        layout.alignment > TextAlignment::Right || !std::isfinite(layout.boxWidth) ||
        !std::isfinite(layout.boxHeight) || layout.boxWidth < 0.0 || layout.boxHeight < 0.0 ||
        ((layout.boxWidth == 0.0) != (layout.boxHeight == 0.0)) ||
        layout.verticalAlignment > TextLayoutOptions::VerticalAlignment::Bottom ||
        layout.anchorMode > TextLayoutOptions::AnchorMode::Right ||
        layout.overflow > TextLayoutOptions::Overflow::Grow) {
        return ImageResult<TextCoverageBitmap>::failure(
            codeError(ImageErrorCode::InvalidParameter));
    }
    if (!fontIsParsed(face)) {
        return ImageResult<TextCoverageBitmap>::failure(codeError(ImageErrorCode::InvalidState));
    }

    const auto scaleX = fontScaleForEmPixelSize(face, parameters.horizontalPixelSize());
    const auto scaleY = fontScaleForEmPixelSize(face, parameters.verticalPixelSize());
    if (scaleX <= 0.0F || scaleY <= 0.0F) {
        return ImageResult<TextCoverageBitmap>::failure(
            codeError(ImageErrorCode::InvalidParameter));
    }

    const auto effectiveContent =
        layout.wrap && layout.boxWidth > 0.0
            ? wrappedText(face, utf8Content, scaleX, layout.letterSpacing, layout.boxWidth)
            : std::string(utf8Content);

    // The baseline is snapped to a whole row once, for the whole line, and the sub-pixel fraction
    // is spent on the horizontal pen position only: a per-glyph vertical sub-pixel shift would make
    // the same string rasterize differently depending on where the line happened to start, which is
    // the opposite of what a reproducible reference path needs.
    const auto vertical = fontVerticalMetrics(face);
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

    std::vector<double> lineWidths(1, 0.0);
    int measurePrevious = -1;
    for (std::size_t cursor = 0; cursor < effectiveContent.size();) {
        const auto scalar = core::decodeUtf8Scalar(effectiveContent, cursor);
        cursor += scalar.length;
        if (layout.multiline && scalar.value == U'\n') {
            lineWidths.push_back(0.0);
            measurePrevious = -1;
            continue;
        }
        if (layout.multiline && scalar.value == U'\r')
            continue;
        const auto glyph = fontGlyphIndex(face, scalar.value);
        auto& width = lineWidths.back();
        if (measurePrevious >= 0)
            width += static_cast<double>(fontGlyphKernAdvance(face, measurePrevious, glyph)) *
                         static_cast<double>(scaleX) +
                     layout.letterSpacing;
        width += static_cast<double>(fontGlyphHorizontalMetrics(face, glyph).advanceWidth) *
                 static_cast<double>(scaleX);
        if (!std::isfinite(width) || std::abs(width) > static_cast<double>(kCoordinateBound))
            return ImageResult<TextCoverageBitmap>::failure(
                codeError(ImageErrorCode::ArithmeticOverflow));
        measurePrevious = glyph;
    }
    const auto widestLine = *std::max_element(lineWidths.begin(), lineWidths.end());
    const auto maximumAdvance = layout.boxWidth > 0.0 ? layout.boxWidth : widestLine;
    const auto lineOffset = [&](const std::size_t line) {
        const auto difference = maximumAdvance - lineWidths[line];
        const auto aligned = layout.alignment == TextAlignment::Left     ? 0.0
                             : layout.alignment == TextAlignment::Center ? difference / 2.0
                                                                         : difference;
        if (layout.boxWidth == 0.0 && layout.anchorMode != TextLayoutOptions::AnchorMode::Left)
            return aligned - (layout.anchorMode == TextLayoutOptions::AnchorMode::Center
                                  ? widestLine / 2.0
                                  : widestLine);
        return aligned;
    };
    std::size_t line = 0;
    const auto lineStep = parameters.verticalPixelSize() * layout.lineHeight;
    const auto naturalHeight = lineStep * static_cast<double>(lineWidths.size());
    const auto verticalDifference = layout.boxHeight > 0.0 ? layout.boxHeight - naturalHeight : 0.0;
    const auto verticalOffset =
        layout.verticalAlignment == TextLayoutOptions::VerticalAlignment::Middle
            ? verticalDifference / 2.0
        : layout.verticalAlignment == TextLayoutOptions::VerticalAlignment::Bottom
            ? verticalDifference
            : 0.0;
    if (!std::isfinite(verticalOffset) ||
        std::abs(verticalOffset) > static_cast<double>(kCoordinateBound))
        return ImageResult<TextCoverageBitmap>::failure(
            codeError(ImageErrorCode::ArithmeticOverflow));
    std::int64_t lineRow = static_cast<std::int64_t>(std::lround(verticalOffset));
    double pen = lineOffset(line);
    int previousGlyph = -1;
    std::size_t offset = 0;
    while (offset < effectiveContent.size()) {
        const auto scalar = core::decodeUtf8Scalar(effectiveContent, offset);
        if (!scalar.isValid()) {
            // isValidUtf8() already accepted these bytes with the same rules, so this is
            // unreachable; it is a hard failure rather than a silent skip so the two can never
            // disagree unnoticed.
            return ImageResult<TextCoverageBitmap>::failure(
                codeError(ImageErrorCode::InvalidState));
        }
        offset += scalar.length;

        if (layout.multiline && scalar.value == U'\n') {
            ++line;
            const auto row = static_cast<double>(line) * lineStep + verticalOffset;
            if (!std::isfinite(row) || row > static_cast<double>(kCoordinateBound))
                return ImageResult<TextCoverageBitmap>::failure(
                    codeError(ImageErrorCode::ArithmeticOverflow));
            lineRow = static_cast<std::int64_t>(std::lround(row));
            pen = lineOffset(line);
            previousGlyph = -1;
            continue;
        }
        if (layout.multiline && scalar.value == U'\r')
            continue;
        if (cancelled && cancelled())
            return ImageResult<TextCoverageBitmap>::failure(
                codeError(ImageErrorCode::InvalidState));
        const auto glyph = fontGlyphIndex(face, scalar.value);
        if (previousGlyph >= 0) {
            pen += layout.letterSpacing;
            pen += static_cast<double>(fontGlyphKernAdvance(face, previousGlyph, glyph)) *
                   static_cast<double>(scaleX);
        }
        if (!std::isfinite(pen) ||
            !withinCoordinateBound(static_cast<std::int64_t>(std::floor(pen)))) {
            return ImageResult<TextCoverageBitmap>::failure(
                codeError(ImageErrorCode::ArithmeticOverflow));
        }

        if (outlines) {
            auto paths = [&] {
                if constexpr (std::is_same_v<Face, EmbeddedFace>)
                    return detail::embeddedFontGlyphOutlines(face, glyph);
                else
                    return detail::externalFontGlyphOutlines(face, glyph);
            }();
            const auto map = [&](PathPoint p) -> PathPoint {
                return {pen + p.x * static_cast<double>(scaleX),
                        static_cast<double>(lineRow + baselineRow) -
                            p.y * static_cast<double>(scaleY)};
            };
            for (auto& path : paths) {
                for (auto& anchor : path.anchors) {
                    anchor.point = map(anchor.point);
                    if (anchor.inHandle)
                        anchor.inHandle = map(*anchor.inHandle);
                    if (anchor.outHandle)
                        anchor.outHandle = map(*anchor.outHandle);
                }
                outlines->push_back(std::move(path));
            }
        }
        const auto penColumn = static_cast<std::int64_t>(std::floor(pen));
        const auto shiftX = static_cast<float>(pen - static_cast<double>(penColumn));
        const GlyphBitmapBox box = fontGlyphBitmapBox(face, glyph, scaleX, scaleY, shiftX, 0.0F);
        if (box.right > box.left && box.bottom > box.top) {
            const auto left = penColumn + static_cast<std::int64_t>(box.left);
            const auto top = lineRow + baselineRow + static_cast<std::int64_t>(box.top);
            const auto right = penColumn + static_cast<std::int64_t>(box.right);
            const auto bottom = lineRow + baselineRow + static_cast<std::int64_t>(box.bottom);
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

        pen += static_cast<double>(fontGlyphHorizontalMetrics(face, glyph).advanceWidth) *
               static_cast<double>(scaleX);
        previousGlyph = glyph;
    }

    if (outlines || placements.empty()) {
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
        fontRasterizeGlyph(face, glyphCoverage, placement.width, placement.height, placement.width,
                           scaleX, scaleY, placement.shiftX, 0.0F, placement.glyph);

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

ImageResult<std::vector<Path>> textOutlines(const TextFont& font, std::string_view content,
                                            TextRasterParameters parameters,
                                            TextLayoutOptions layout, PathCancellation cancelled) {
    std::vector<Path> paths;
    const auto result = std::visit(
        [&](const auto& face) {
            return rasterizeFontText(face, content, parameters, 0, layout, &paths, cancelled);
        },
        font);
    if (!result)
        return ImageResult<std::vector<Path>>::failure(*result.error());
    return ImageResult<std::vector<Path>>::success(std::move(paths));
}

ImageResult<TextCoverageBitmap> TextCoverageBitmap::rasterizeEmbeddedText(
    const EmbeddedFace face, const std::string_view utf8Content,
    const TextRasterParameters parameters, const std::size_t coverageByteLimit,
    const TextLayoutOptions layout) {
    return rasterizeFontText(face, utf8Content, parameters, coverageByteLimit, layout);
}

ImageResult<TextCoverageBitmap> TextCoverageBitmap::rasterizeText(
    const TextFont& font, const std::string_view utf8Content, const TextRasterParameters parameters,
    const std::size_t coverageByteLimit, const TextLayoutOptions layout) {
    return std::visit(
        [&](const auto& face) {
            return rasterizeFontText(face, utf8Content, parameters, coverageByteLimit, layout);
        },
        font);
}

} // namespace bloom::render
