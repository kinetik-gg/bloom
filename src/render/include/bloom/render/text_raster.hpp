#ifndef BLOOM_RENDER_TEXT_RASTER_HPP
#define BLOOM_RENDER_TEXT_RASTER_HPP

#include <bloom/render/embedded_fonts.hpp>
#include <bloom/render/image_types.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace bloom::render {

// Upper bound on an authored text size, in pixels per em. Chosen so one line of a handful of glyphs
// cannot by itself exceed the evaluator's pixel-storage budget before the budget check runs, and so
// the scaled design-unit arithmetic below stays far inside Float64's exact-integer range. Authoring
// validation refuses a larger size rather than silently clamping it.
inline constexpr double kMaximumTextPixelSize = 4096.0;

// Checked text raster parameters. Horizontal and vertical em sizes are separate so a
// proxy-resolution evaluation can scale glyphs by exactly the same per-axis factors it scales layer
// translation by, instead of rasterizing at full size into a reduced frame.
enum class TextAlignment : std::uint8_t { Left, Center, Right };
struct TextLayoutOptions final {
    TextAlignment alignment = TextAlignment::Left;
    double lineHeight = 1.0;
    double letterSpacing = 0.0;
    bool multiline = true;
    double boxWidth = 0.0;
    double boxHeight = 0.0;
    bool wrap = false;
    enum class VerticalAlignment : std::uint8_t { Top, Middle, Bottom };
    enum class AnchorMode : std::uint8_t { Left, Center, Right };
    enum class Overflow : std::uint8_t { Clip, Grow };
    VerticalAlignment verticalAlignment = VerticalAlignment::Top;
    AnchorMode anchorMode = AnchorMode::Left;
    Overflow overflow = Overflow::Clip;
};

class TextRasterParameters final {
  public:
    [[nodiscard]] static ImageResult<TextRasterParameters>
    create(double horizontalPixelSize, double verticalPixelSize) noexcept;

    [[nodiscard]] constexpr double horizontalPixelSize() const noexcept {
        return horizontalPixelSize_;
    }
    [[nodiscard]] constexpr double verticalPixelSize() const noexcept { return verticalPixelSize_; }

    friend constexpr bool operator==(const TextRasterParameters&,
                                     const TextRasterParameters&) noexcept = default;

  private:
    constexpr TextRasterParameters(const double horizontalPixelSize,
                                   const double verticalPixelSize) noexcept
        : horizontalPixelSize_(horizontalPixelSize), verticalPixelSize_(verticalPixelSize) {}

    double horizontalPixelSize_ = 0.0;
    double verticalPixelSize_ = 0.0;
};

// One rasterized single-line string as 8-bit area coverage.
//
// GAMMA RULE (the one rule this whole path depends on): a coverage byte is a LINEAR area fraction
// -- the share of the pixel that the glyph outline covers, coverage/255 exactly -- and never a
// gamma-encoded intensity. It is therefore used directly as linear alpha, and no transfer function,
// sRGB curve, or gamma exponent is applied anywhere between the rasterizer and the premultiplied
// lin_rec709_scene process pixel (see coverageSolidRow() in cpu_image_primitives.hpp). Compositing
// coverage in a display-encoded space is the classic source of fringed, too-thin text; Bloom's
// process space is scene-linear, so the correct thing and the simple thing coincide here.
//
// GEOMETRY: bitmap pixel (0, 0) sits at the text origin plus (originX(), originY()). The text
// origin is the pen start on the ascender line -- x is the left edge of the first glyph's advance,
// y is the font's ascent above the baseline -- so placing the text origin at a frame's data-window
// origin puts the first line inside the frame with its ascender flush to the top edge.
// originX()/originY() may be negative (a glyph whose ink reaches left of its pen origin, or above
// the ascender), and the bitmap is the exact union of every glyph's ink: nothing is clipped to a
// line box here, so clipping is the compositor's decision rather than this function's.
//
// LAYOUT: one line, no wrapping, no shaping, no bidi, no line breaks. Glyphs advance left to right
// by their own horizontal advance plus the face's kern pair, with the sub-pixel fraction of the pen
// position handed to the rasterizer so spacing is not rounded per glyph. A newline or any other
// control character is a glyph lookup like every other codepoint, not a layout instruction. Text
// shaping is a separate, later concern; this is the proof path the CPU reference evaluator needs.
class TextCoverageBitmap final {
  public:
    // A bitmap with no covered pixels: the correct, successful result for empty content or content
    // whose glyphs are all blank (a run of spaces). Never an error.
    [[nodiscard]] static TextCoverageBitmap empty() noexcept { return {}; }

    TextCoverageBitmap(std::int64_t originX, std::int64_t originY, std::uint32_t width,
                       std::uint32_t height, std::vector<std::uint8_t> coverage) noexcept;

    // Rasterizes `utf8Content` with the selected embedded face (bloom/render/embedded_fonts.hpp).
    // Fails with InvalidParameter for content that is not well-formed UTF-8, InvalidState if the
    // embedded font does not parse (a build-integrity
    // failure), ArithmeticOverflow if the line's extent cannot be represented, and
    // PixelStorageBudgetExceeded if the coverage bitmap would need more than `coverageByteLimit`
    // bytes -- checked from the computed extent BEFORE any storage is allocated or any glyph drawn.
    [[nodiscard]] static ImageResult<TextCoverageBitmap>
    rasterizeEmbeddedText(EmbeddedFace face, std::string_view utf8Content,
                          TextRasterParameters parameters, std::size_t coverageByteLimit,
                          TextLayoutOptions layout = {});

    [[nodiscard]] static ImageResult<TextCoverageBitmap>
    rasterizeText(const TextFont& font, std::string_view utf8Content,
                  TextRasterParameters parameters, std::size_t coverageByteLimit,
                  TextLayoutOptions layout = {});

    [[nodiscard]] bool hasCoverage() const noexcept { return !coverage_.empty(); }
    [[nodiscard]] std::int64_t originX() const noexcept { return originX_; }
    [[nodiscard]] std::int64_t originY() const noexcept { return originY_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::span<const std::uint8_t> coverage() const& noexcept { return coverage_; }
    [[nodiscard]] std::span<const std::uint8_t> coverage() const&& = delete;
    // Empty for an out-of-range row, so a caller that miscomputes a row reads nothing rather than
    // reading past the bitmap.
    [[nodiscard]] std::span<const std::uint8_t> row(std::uint32_t y) const& noexcept;
    [[nodiscard]] std::span<const std::uint8_t> row(std::uint32_t y) const&& = delete;

  private:
    TextCoverageBitmap() noexcept = default;

    std::int64_t originX_ = 0;
    std::int64_t originY_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::uint8_t> coverage_;
};

} // namespace bloom::render

#endif // BLOOM_RENDER_TEXT_RASTER_HPP
