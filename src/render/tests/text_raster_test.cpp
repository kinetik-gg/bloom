#include <bloom/core/color.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/embedded_fonts.hpp>
#include <bloom/render/text_raster.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::render::coverageSolidRow;
using bloom::render::EmbeddedFace;
using bloom::render::embeddedFaceBytes;
using bloom::render::ImageErrorCode;
using bloom::render::ImageResult;
using bloom::render::kMaximumTextPixelSize;
using bloom::render::Rgba32f;
using bloom::render::solidPixelFromStraightLinearRec709Scene;
using bloom::render::TextCoverageBitmap;
using bloom::render::TextRasterParameters;

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

template <typename T>
[[nodiscard]] bool hasError(const ImageResult<T>& result, const ImageErrorCode code) {
    return !result && result.error()->code == code;
}

[[nodiscard]] std::size_t generousByteLimit() noexcept { return std::size_t{64} * 1024 * 1024; }

// "Ab" at exactly 16 px per em in the embedded DejaVu Sans Book face, every coverage byte pinned.
// The bitmap is 21 x 14 at text-origin offset (0, 2) -- two rows below the ascender line, because
// this string's tallest ink is the cap height rather than the ascender, and column 0 because the
// capital A's left leg reaches its own pen origin on the bottom row.
//
// Rendered, where '#' is coverage >= 170, '+' >= 85, '-' > 0:
//
// ............--.......
// ....++-.....+#.......
// ...-###.....+#.......
// ...-#+#-....+#.......
// ...+#-#+....+#-###-..
// ..-#+.+#-...+##++##-.
// ..+#-.-#+...+#+..-#+.
// ..##...##...+#-...+#-
// .-#+---+#-..+#....+#-
// .+#######+..+#-...+#-
// -##-----##-.+#-...##.
// -#-.....+#-.+##+-+#+.
// ##-.....-#+.+#+###+-.
// ...............---...
//
// This is a byte-exact golden on purpose. Every input is fixed (the digest-pinned embedded font
// bytes, an integer baseline row, per-glyph horizontal sub-pixel shifts derived from exact Float64
// pen arithmetic) and src/render is compiled with Bloom's strict floating-point flags
// (-fno-fast-math -ffp-contract=off -frounding-math, applied to the vendored rasterizer's
// translation unit too), so the same string at the same size must produce the same bytes. A diff
// here means the font, the layout rule, or the rasterizer changed -- each of which is a
// kCpuImagePrimitiveSemanticsVersion change, never an incidental one.
constexpr std::uint32_t kAbGoldenWidth = 21;
constexpr std::uint32_t kAbGoldenHeight = 14;
constexpr std::size_t kAbGoldenByteCount =
    static_cast<std::size_t>(kAbGoldenWidth) * static_cast<std::size_t>(kAbGoldenHeight);
constexpr std::array<std::uint8_t, kAbGoldenByteCount> kAbGoldenCoverage{
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   24,  34,  0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   93,  169, 84,  0,   0,   0,   0,   0,   153, 215, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   1,   220, 254, 207, 0,   0,   0,   0,   0,   153, 215, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   63,  255, 140, 255, 49,  0,   0,   0,   0,   153,
    215, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   161, 222, 6,   234, 146, 0,   0,   0,
    0,   153, 215, 44,  178, 229, 197, 78,  0,   0,   0,   0,   14,  244, 129, 0,   144, 236, 7,
    0,   0,   0,   153, 235, 230, 163, 100, 215, 253, 64,  0,   0,   0,   100, 254, 36,  0,   50,
    255, 85,  0,   0,   0,   153, 255, 107, 0,   0,   3,   213, 149, 0,   0,   0,   198, 196, 0,
    0,   0,   210, 183, 0,   0,   0,   153, 255, 36,  0,   0,   0,   144, 225, 1,   0,   40,  255,
    159, 78,  78,  78,  168, 252, 28,  0,   0,   153, 233, 0,   0,   0,   0,   86,  255, 37,  0,
    137, 255, 255, 255, 255, 255, 255, 255, 122, 0,   0,   153, 253, 19,  0,   0,   0,   126, 241,
    6,   4,   231, 173, 2,   2,   2,   2,   2,   183, 218, 1,   0,   153, 255, 84,  0,   0,   0,
    193, 170, 0,   77,  255, 82,  0,   0,   0,   0,   0,   92,  255, 61,  0,   153, 246, 219, 95,
    32,  149, 252, 93,  0,   174, 239, 7,   0,   0,   0,   0,   0,   12,  244, 159, 0,   153, 215,
    94,  243, 255, 252, 143, 3,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   4,   42,  13,  0,   0,   0};

void testEmbeddedFont(Expectations& expectations) {
    constexpr std::array faces{EmbeddedFace::DejaVuSans, EmbeddedFace::InterRegular,
                               EmbeddedFace::InterMedium, EmbeddedFace::InterSemiBold};
    constexpr std::array expectedByteCounts{757076U, 411640U, 417300U, 419744U};
    for (std::size_t index = 0; index < faces.size(); ++index) {
        const auto bytes = embeddedFaceBytes(faces[index]);
        expectations.expect(bytes.size() == expectedByteCounts[index],
                            "the embedded face has its pinned vendored byte count");
        expectations.expect(bytes.data() == embeddedFaceBytes(faces[index]).data(),
                            "the embed is one immutable static payload, not a per-call copy");
        expectations.expect(
            bytes.size() >= 4 && bytes[0] == 0x00 && bytes[1] == 0x01 && bytes[2] == 0x00 &&
                bytes[3] == 0x00,
            "the embedded bytes are a TrueType sfnt, which is what the rasterizer parses");
    }
    // A TrueType face opens with the 0x00010000 sfnt version tag; a wrong asset (an OpenType/CFF
    // 'OTTO' file, a WOFF, a truncated copy) would not.
    expectations.expect(
        bloom::render::kEmbeddedDejaVuSansFamilyName == "DejaVu Sans" &&
            bloom::render::kEmbeddedDejaVuSansStyleName == "Book" &&
            bloom::render::kEmbeddedInterFamilyName == "Inter" &&
            bloom::render::kEmbeddedInterRegularStyleName == "Regular" &&
            bloom::render::kEmbeddedInterMediumStyleName == "Medium" &&
            bloom::render::kEmbeddedInterSemiBoldStyleName == "SemiBold",
        "the embedded faces name the families and styles their provenance records do");
}

void testParameterValidation(Expectations& expectations) {
    const auto valid = TextRasterParameters::create(72.0, 36.0);
    expectations.expect(valid && valid.value()->horizontalPixelSize() == 72.0 &&
                            valid.value()->verticalPixelSize() == 36.0,
                        "text raster parameters keep their per-axis em sizes exactly");
    expectations.expect(
        hasError(TextRasterParameters::create(0.0, 72.0), ImageErrorCode::InvalidParameter) &&
            hasError(TextRasterParameters::create(72.0, -1.0), ImageErrorCode::InvalidParameter),
        "a non-positive em size is refused rather than clamped");
    expectations.expect(
        hasError(TextRasterParameters::create(std::numeric_limits<double>::quiet_NaN(), 72.0),
                 ImageErrorCode::InvalidParameter),
        "a non-finite em size is refused");
    const auto atLimit = TextRasterParameters::create(kMaximumTextPixelSize, kMaximumTextPixelSize);
    expectations.expect(atLimit && hasError(TextRasterParameters::create(
                                                kMaximumTextPixelSize, kMaximumTextPixelSize + 1.0),
                                            ImageErrorCode::InvalidParameter),
                        "the maximum em size is inclusive and anything past it is refused");
}

void testGlyphCoverageGolden(Expectations& expectations) {
    const auto parameters = TextRasterParameters::create(16.0, 16.0);
    if (!parameters) {
        expectations.expect(false, "the golden's raster parameters are valid");
        return;
    }
    const auto rasterized = TextCoverageBitmap::rasterizeEmbeddedText(
        EmbeddedFace::DejaVuSans, "Ab", *parameters.value(), generousByteLimit());
    if (!rasterized) {
        expectations.expect(false, "a small string rasterizes against the embedded face");
        return;
    }
    const auto& bitmap = *rasterized.value();
    expectations.expect(bitmap.hasCoverage() && bitmap.width() == kAbGoldenWidth &&
                            bitmap.height() == kAbGoldenHeight,
                        "the golden string's coverage extent is exactly 21 x 14");
    expectations.expect(bitmap.originX() == 0 && bitmap.originY() == 2,
                        "the golden bitmap sits at text-origin offset (0, 2)");
    if (bitmap.width() != kAbGoldenWidth || bitmap.height() != kAbGoldenHeight) {
        return;
    }
    expectations.expect(bitmap.coverage().size() == kAbGoldenCoverage.size(),
                        "coverage is exactly one byte per pixel");
    std::size_t mismatches = 0;
    std::size_t firstMismatch = 0;
    for (std::size_t index = 0; index < kAbGoldenCoverage.size(); ++index) {
        if (bitmap.coverage()[index] != kAbGoldenCoverage[index]) {
            if (mismatches == 0) {
                firstMismatch = index;
            }
            ++mismatches;
        }
    }
    expectations.expect(mismatches == 0,
                        "every coverage byte matches the golden (" + std::to_string(mismatches) +
                            " differ, first at index " + std::to_string(firstMismatch) + ")");

    // Coverage is area, so the pinned bytes must also be structurally true: the 'A' crossbar row
    // reaches full coverage, and the row above the cap height is blank except for the 'b' ascender.
    expectations.expect(bitmap.row(9)[2] == 255 && bitmap.row(9)[8] == 255,
                        "the capital A's crossbar row reaches full coverage");
    const auto topRow = bitmap.row(0);
    std::size_t inkInTopRow = 0;
    for (const auto sample : topRow) {
        inkInTopRow += sample == 0 ? 0u : 1u;
    }
    expectations.expect(inkInTopRow == 2,
                        "only the lowercase b's ascender has ink on the bitmap's first row");
    expectations.expect(bitmap.row(kAbGoldenHeight).empty() && bitmap.row(1000).empty(),
                        "an out-of-range row reads nothing instead of reading past the bitmap");
}

void testFaceSelectionChangesCoverage(Expectations& expectations) {
    const auto parameters = TextRasterParameters::create(32.0, 32.0);
    if (!parameters) {
        expectations.expect(false, "face-selection raster parameters are valid");
        return;
    }
    const auto dejavu = TextCoverageBitmap::rasterizeEmbeddedText(
        EmbeddedFace::DejaVuSans, "Bloom", *parameters.value(), generousByteLimit());
    const auto semiBold = TextCoverageBitmap::rasterizeEmbeddedText(
        EmbeddedFace::InterSemiBold, "Bloom", *parameters.value(), generousByteLimit());
    expectations.expect(dejavu && semiBold, "both embedded faces rasterize the same string");
    if (!dejavu || !semiBold)
        return;
    expectations.expect(dejavu.value()->coverage().size() != semiBold.value()->coverage().size() ||
                            !std::equal(dejavu.value()->coverage().begin(),
                                        dejavu.value()->coverage().end(),
                                        semiBold.value()->coverage().begin()),
                        "DejaVu Sans and Inter SemiBold produce different coverage");
}

void testEmptyAndBlankContent(Expectations& expectations) {
    const auto parameters = TextRasterParameters::create(24.0, 24.0);
    if (!parameters) {
        expectations.expect(false, "blank-content raster parameters are valid");
        return;
    }
    const auto empty = TextCoverageBitmap::rasterizeEmbeddedText(
        EmbeddedFace::DejaVuSans, "", *parameters.value(), generousByteLimit());
    expectations.expect(empty && !empty.value()->hasCoverage() && empty.value()->width() == 0 &&
                            empty.value()->height() == 0,
                        "empty content succeeds with no coverage rather than failing");
    const auto spaces = TextCoverageBitmap::rasterizeEmbeddedText(
        EmbeddedFace::DejaVuSans, "   ", *parameters.value(), generousByteLimit());
    expectations.expect(spaces && !spaces.value()->hasCoverage(),
                        "content whose glyphs are all blank also succeeds with no coverage");
    const auto explicitlyEmpty = TextCoverageBitmap::empty();
    expectations.expect(explicitlyEmpty.coverage().empty() && !explicitlyEmpty.hasCoverage() &&
                            explicitlyEmpty.originX() == 0 && explicitlyEmpty.originY() == 0,
                        "the explicit empty bitmap carries nothing");
}

void testRefusals(Expectations& expectations) {
    const auto parameters = TextRasterParameters::create(16.0, 16.0);
    if (!parameters) {
        expectations.expect(false, "refusal-test raster parameters are valid");
        return;
    }
    const std::string illFormed("\xff\xfe", 2);
    expectations.expect(
        hasError(TextCoverageBitmap::rasterizeEmbeddedText(
                     EmbeddedFace::DejaVuSans, illFormed, *parameters.value(), generousByteLimit()),
                 ImageErrorCode::InvalidParameter),
        "content that is not well-formed UTF-8 is refused");
    const std::string truncated("\xe2\x82", 2);
    expectations.expect(
        hasError(TextCoverageBitmap::rasterizeEmbeddedText(
                     EmbeddedFace::DejaVuSans, truncated, *parameters.value(), generousByteLimit()),
                 ImageErrorCode::InvalidParameter),
        "a truncated multi-byte sequence is refused, not rendered as .notdef");

    const auto budgeted = TextCoverageBitmap::rasterizeEmbeddedText(EmbeddedFace::DejaVuSans, "Ab",
                                                                    *parameters.value(), 4);
    expectations.expect(hasError(budgeted, ImageErrorCode::PixelStorageBudgetExceeded),
                        "a coverage bitmap larger than the byte limit is refused");
    expectations.expect(budgeted.error()->requestedPixelStorageBytes == kAbGoldenCoverage.size() &&
                            budgeted.error()->pixelStorageByteLimit == 4,
                        "the budget refusal reports the exact required and permitted byte counts, "
                        "which is only possible because the extent is computed before allocation");

    // A codepoint the face does not cover resolves to glyph 0, which DejaVu Sans draws as a real
    // missing-glyph box: unsupported text is visibly missing, never silently dropped.
    const std::string emoji("\xf0\x9f\x98\x80", 4);
    const auto notdef = TextCoverageBitmap::rasterizeEmbeddedText(
        EmbeddedFace::DejaVuSans, emoji, *parameters.value(), generousByteLimit());
    expectations.expect(notdef && notdef.value()->hasCoverage(),
                        "an uncovered codepoint rasterizes the face's missing-glyph box");
}

void testPerAxisScaling(Expectations& expectations) {
    const auto full = TextRasterParameters::create(64.0, 64.0);
    const auto halfHeight = TextRasterParameters::create(64.0, 32.0);
    if (!full || !halfHeight) {
        expectations.expect(false, "per-axis raster parameters are valid");
        return;
    }
    const auto tall = TextCoverageBitmap::rasterizeEmbeddedText(EmbeddedFace::DejaVuSans, "Bloom",
                                                                *full.value(), generousByteLimit());
    const auto squashed = TextCoverageBitmap::rasterizeEmbeddedText(
        EmbeddedFace::DejaVuSans, "Bloom", *halfHeight.value(), generousByteLimit());
    if (!tall || !squashed) {
        expectations.expect(false, "both per-axis rasterizations succeed");
        return;
    }
    expectations.expect(squashed.value()->width() == tall.value()->width(),
                        "halving only the vertical em size leaves the advance width unchanged");
    expectations.expect(squashed.value()->height() < tall.value()->height(),
                        "and shortens the glyphs, which is what a proxy evaluation needs");
}

void testCoverageSolidRow(Expectations& expectations) {
    const auto solid = solidPixelFromStraightLinearRec709Scene(Color4d{0.5, 0.25, 2.0, 0.5});
    if (!solid) {
        expectations.expect(false, "the coverage test's text color converts to a process pixel");
        return;
    }
    const auto pixel = *solid.value();
    const std::array<std::uint8_t, 4> coverage{0, 255, 128, 64};
    std::vector<Rgba32f> output(coverage.size(), Rgba32f::transparent());
    expectations.expect(!coverageSolidRow(coverage, pixel, output).has_value(),
                        "a matched coverage run composites without error");
    expectations.expect(output[0] == Rgba32f::transparent(),
                        "zero coverage is exactly transparent, with no multiply to round");
    expectations.expect(output[1] == pixel,
                        "full coverage is exactly the solid process pixel, unmodified");
    const auto half = static_cast<float>(128.0 / 255.0);
    expectations.expect(output[2].alpha() == static_cast<float>(static_cast<double>(pixel.alpha()) *
                                                                (128.0 / 255.0)) &&
                            output[2].red() == static_cast<float>(static_cast<double>(pixel.red()) *
                                                                  (128.0 / 255.0)),
                        "partial coverage scales every premultiplied component by coverage/255");
    expectations.expect(output[3].alpha() < output[2].alpha() && output[3].alpha() > 0.0F,
                        "coverage is linear alpha: less area means proportionally less alpha");
    expectations.expect(half > 0.0F,
                        "the half-coverage fraction is the linear 128/255, not a gamma "
                        "decode of it");

    std::vector<Rgba32f> shortOutput(coverage.size() - 1, Rgba32f::transparent());
    const auto mismatch = coverageSolidRow(coverage, pixel, shortOutput);
    expectations.expect(mismatch.has_value() &&
                            mismatch->code == ImageErrorCode::InvalidStorageSize,
                        "a coverage run and its output row must have equal length");

    std::vector<Rgba32f> transparentOutput(coverage.size(), pixel);
    expectations.expect(
        !coverageSolidRow(coverage, Rgba32f::transparent(), transparentOutput).has_value() &&
            transparentOutput[1] == Rgba32f::transparent(),
        "a fully transparent text color clears the run instead of writing ink");
}

void testTypographyGoldens(Expectations& expectations) {
    using bloom::render::TextAlignment;
    using bloom::render::TextLayoutOptions;
    const auto parameters = TextRasterParameters::create(16, 16);
    const std::array<TextLayoutOptions, 5> settings{{{},
                                                     {TextAlignment::Center, 1, 0, true},
                                                     {TextAlignment::Right, 1, 0, true},
                                                     {TextAlignment::Left, 1.5, 0, true},
                                                     {TextAlignment::Left, 1, 3, true}}};
    std::array<std::uint64_t, 5> digests{};
    for (std::size_t index = 0; index < settings.size(); ++index) {
        const auto raster = TextCoverageBitmap::rasterizeEmbeddedText(
            EmbeddedFace::DejaVuSans, "Ab\ni", *parameters.value(), generousByteLimit(),
            settings[index]);
        expectations.expect(static_cast<bool>(raster), "multiline typography rasterizes");
        if (!raster)
            continue;
        auto digest = std::uint64_t{14695981039346656037ULL};
        for (const auto byte : raster.value()->coverage()) {
            digest ^= byte;
            digest *= 1099511628211ULL;
        }
        digests[index] = digest;
        const std::array<std::uint32_t, 5> widths{21, 21, 21, 21, 24};
        const std::array<std::uint32_t, 5> heights{29, 29, 29, 37, 29};
        expectations.expect(raster.value()->originX() == 0 && raster.value()->originY() == 2 &&
                                raster.value()->width() == widths[index] &&
                                raster.value()->height() == heights[index],
                            "typography layout bounds golden");
    }
    expectations.expect(
        digests == std::array<std::uint64_t, 5>{7853883709848069213ULL, 3444746878591692842ULL,
                                                4373560154211617060ULL, 6681727905294334493ULL,
                                                17380785927925869081ULL},
        "left, centre, right, line-height and letter-spacing coverage goldens");
    const auto invalid = TextCoverageBitmap::rasterizeEmbeddedText(
        EmbeddedFace::DejaVuSans, "A", *parameters.value(), generousByteLimit(),
        {TextAlignment::Left, 0, 0, true});
    expectations.expect(hasError(invalid, ImageErrorCode::InvalidParameter),
                        "zero line height is refused");
    const auto huge = TextCoverageBitmap::rasterizeEmbeddedText(
        EmbeddedFace::DejaVuSans, "A\nB", *parameters.value(), 1024,
        {TextAlignment::Left, 1.0e100, 0, true});
    expectations.expect(hasError(huge, ImageErrorCode::ArithmeticOverflow),
                        "unrepresentable layout is refused before allocation");
}

void testBoxTypographyGoldens(Expectations& expectations) {
    using bloom::render::TextAlignment;
    using bloom::render::TextLayoutOptions;
    const auto parameters = TextRasterParameters::create(16, 16);
    TextLayoutOptions layout;
    layout.alignment = TextAlignment::Center;
    layout.boxWidth = 32.0;
    layout.boxHeight = 64.0;
    layout.wrap = true;
    layout.verticalAlignment = TextLayoutOptions::VerticalAlignment::Bottom;
    layout.overflow = TextLayoutOptions::Overflow::Clip;
    const auto raster = TextCoverageBitmap::rasterizeEmbeddedText(
        EmbeddedFace::DejaVuSans, "A A A", *parameters.value(), generousByteLimit(), layout);
    expectations.expect(static_cast<bool>(raster), "wrapped box typography rasterizes");
    if (!raster)
        return;
    auto digest = std::uint64_t{14695981039346656037ULL};
    for (const auto byte : raster.value()->coverage()) {
        digest ^= byte;
        digest *= 1099511628211ULL;
    }
    expectations.expect(raster.value()->originX() == 2 && raster.value()->originY() == 35 &&
                            raster.value()->width() == 28 && raster.value()->height() == 28,
                        "wrapped centred bottom-aligned box bounds golden");
    expectations.expect(digest == 3600944647390057943ULL,
                        "wrapped centred bottom-aligned box coverage golden");
}

void testLayoutQuery(Expectations& expectations) {
    using namespace bloom::render;
    const auto created = TextRasterParameters::create(16, 16);
    const auto parameters = *created.value();
    const auto point = layoutText(EmbeddedFace::DejaVuSans, "AVé", parameters);
    expectations.expect(point && point.value()->glyphs.size() == 3,
                        "layout reports glyphs with UTF-8 source offsets");
    if (!point)
        return;
    const auto& glyphs = point.value()->glyphs;
    expectations.expect(glyphs[2].byteOffset == 2 &&
                            glyphs[2].advanceRect.x ==
                                glyphs[0].advanceRect.width + glyphs[1].advanceRect.width &&
                            point.value()->lines[0].box.width ==
                                glyphs[2].advanceRect.x + glyphs[2].advanceRect.width,
                        "caret after N bytes equals advances including kerning");
    TextLayoutOptions options;
    options.boxWidth = 32;
    options.boxHeight = 64;
    options.wrap = true;
    options.verticalAlignment = TextLayoutOptions::VerticalAlignment::Bottom;
    const auto wrapped = layoutText(EmbeddedFace::DejaVuSans, "A A A", parameters, options);
    expectations.expect(wrapped && wrapped.value()->lines.size() == 2 &&
                            wrapped.value()->lines[0].box.y == 32 &&
                            wrapped.value()->lines[1].box.y == 48 &&
                            wrapped.value()->lines[1].byteRange.begin == 4 &&
                            wrapped.value()->glyphs.back().byteOffset == 4,
                        "wrapped lines share raster positions and retain original byte offsets");
    const auto blank = layoutText(EmbeddedFace::DejaVuSans, "", parameters);
    expectations.expect(
        blank && blank.value()->lines.size() == 1 && blank.value()->glyphs.empty() &&
            blank.value()->caretHeight == 16 && blank.value()->lines[0].box.x == 0 &&
            blank.value()->lines[0].box.y == 0 && blank.value()->lines[0].byteRange.end == 0,
        "empty content retains a caret at the anchor");
    const auto breaks = layoutText(EmbeddedFace::DejaVuSans, "é\n\nA", parameters);
    expectations.expect(breaks && breaks.value()->lines.size() == 3 &&
                            breaks.value()->lines[0].byteRange.end == 2 &&
                            breaks.value()->lines[1].byteRange.begin == 3 &&
                            breaks.value()->lines[1].byteRange.end == 3 &&
                            breaks.value()->glyphs.back().byteOffset == 4,
                        "explicit empty lines retain byte ranges");
    expectations.expect(
        !layoutText(EmbeddedFace::DejaVuSans, "A", parameters, {}, [] { return true; }),
        "layout observes cancellation");
}

void testOutlineScale(Expectations& expectations) {
    using namespace bloom::render;
    const auto parameters = TextRasterParameters::create(16, 16);
    const auto paths = textOutlines(EmbeddedFace::DejaVuSans, "H", *parameters.value());
    expectations.expect(static_cast<bool>(paths), "glyph outlines are available");
    if (!paths)
        return;
    const auto raster = PathRaster::transformed(*paths.value(), {}, {4, 0, 0, 4, 0, 0}, 1, 1);
    expectations.expect(static_cast<bool>(raster), "400 percent text rasterizes outlines");
    if (!raster)
        return;
    std::array<std::uint8_t, 64> row{};
    expectations.expect(raster.value()->coverageRow(0, 20, row, PathFillRule::NonZero, false),
                        "scaled H row");
    // DejaVu H's left vertical stem begins at design x=201/2048 em: 6.28125 output pixels.
    // It covers 3/4 samples in column 6 and is fully opaque in column 7. A 4x bilinear
    // enlargement spreads that edge over several columns and cannot satisfy this contrast pin.
    expectations.expect(row[5] == 0 && row[6] == 191 && row[7] == 255,
                        "400 percent glyph stem has a one-pixel coverage transition");
    const auto cancelled =
        textOutlines(EmbeddedFace::DejaVuSans, "H", *parameters.value(), {}, [] { return true; });
    expectations.expect(!cancelled, "outline work observes cancellation");
}

} // namespace

int main() {
    try {
        Expectations expectations;
        testLayoutQuery(expectations);
        testOutlineScale(expectations);
        testTypographyGoldens(expectations);
        testEmbeddedFont(expectations);
        testParameterValidation(expectations);
        testGlyphCoverageGolden(expectations);
        testFaceSelectionChangesCoverage(expectations);
        testEmptyAndBlankContent(expectations);
        testRefusals(expectations);
        testPerAxisScaling(expectations);
        testCoverageSolidRow(expectations);
        testBoxTypographyGoldens(expectations);
        return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
