#include <bloom/core/blend_mode.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/display_buffer.hpp>

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <ranges>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(__SSE2__) || defined(_M_X64)
#include <xmmintrin.h>
#define BLOOM_RENDER_TEST_HAS_MXCSR 1
#else
#define BLOOM_RENDER_TEST_HAS_MXCSR 0
#endif

namespace {

using bloom::core::Color4d;
using bloom::core::PixelAspectRatio;
using bloom::render::blendLinearRec709SceneRow;
using bloom::render::fillSolidRow;
using bloom::render::ImageErrorCode;
using bloom::render::ImageResult;
using bloom::render::ImageStatus;
using bloom::render::ImageWindow;
using bloom::render::LayerTransform;
using bloom::render::layerTransformBilinearRow;
using bloom::render::mapLinearRec709SceneToSrgbRow;
using bloom::render::PreparedReferenceDisplayBuffer;
using bloom::render::ReferenceDisplayBufferBuilder;
using bloom::render::ReferenceDisplayBufferDescriptor;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::render::Rgba32fImageView;
using bloom::render::Rgba8;
using bloom::render::solidPixelFromStraightLinearRec709Scene;
using bloom::render::sourceOverLinearRec709SceneRow;
using bloom::render::translateOpacityBilinearRow;
using bloom::render::TranslationOpacity;

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

class RoundingModeGuard final {
  public:
    RoundingModeGuard() noexcept : original_(std::fegetround()) {}
    ~RoundingModeGuard() { static_cast<void>(std::fesetround(original_)); }

    RoundingModeGuard(const RoundingModeGuard&) = delete;
    RoundingModeGuard& operator=(const RoundingModeGuard&) = delete;

  private:
    int original_ = FE_TONEAREST;
};

#if BLOOM_RENDER_TEST_HAS_MXCSR
class MxcsrGuard final {
  public:
    MxcsrGuard() noexcept : original_(_mm_getcsr()) {}
    ~MxcsrGuard() { _mm_setcsr(original_); }

    MxcsrGuard(const MxcsrGuard&) = delete;
    MxcsrGuard& operator=(const MxcsrGuard&) = delete;

    [[nodiscard]] std::uint32_t original() const noexcept { return original_; }

  private:
    std::uint32_t original_ = 0;
};
#endif

template <typename T>
[[nodiscard]] bool hasError(const ImageResult<T>& result, const ImageErrorCode code) noexcept {
    return !result && result.error() != nullptr && result.error()->code == code;
}

[[nodiscard]] bool hasError(const ImageStatus& status, const ImageErrorCode code) noexcept {
    return status.has_value() && status->code == code;
}

[[nodiscard]] ImageWindow window(const std::int64_t originX, const std::int64_t originY,
                                 const std::uint64_t width, const std::uint64_t height) {
    const auto result = ImageWindow::create(originX, originY, width, height);
    if (!result) {
        throw std::logic_error("invalid window fixture");
    }
    return *result.value();
}

[[nodiscard]] Rgba32f pixel(const float red, const float green, const float blue,
                            const float alpha) {
    const auto result = Rgba32f::fromPremultiplied(red, green, blue, alpha);
    if (!result) {
        throw std::logic_error("invalid pixel fixture");
    }
    return *result.value();
}

// Rgba32f has no default state by design, so a scratch row has to be filled with the one value that
// is always meaningful: exact transparent black.
template <std::size_t Size> [[nodiscard]] std::array<Rgba32f, Size> transparentRow() {
    return []<std::size_t... Index>(std::index_sequence<Index...>) {
        return std::array<Rgba32f, Size>{((void)Index, Rgba32f::transparent())...};
    }(std::make_index_sequence<Size>{});
}

[[nodiscard]] Rgba32fImageDescriptor descriptor(const ImageWindow dataWindow,
                                                const ImageWindow displayWindow) {
    const auto result =
        Rgba32fImageDescriptor::create(dataWindow, displayWindow, PixelAspectRatio::square());
    if (!result) {
        throw std::logic_error("invalid descriptor fixture");
    }
    return *result.value();
}

template <std::size_t Size>
[[nodiscard]] Rgba32fImage makeImage(const Rgba32fImageDescriptor imageDescriptor,
                                     const std::array<Rgba32f, Size>& pixels) {
    if (pixels.size() != imageDescriptor.layout().pixelCount) {
        throw std::logic_error("invalid image pixel fixture size");
    }
    auto builderResult =
        Rgba32fImageBuilder::create(imageDescriptor, imageDescriptor.layout().pixelStorageBytes);
    if (!builderResult) {
        throw std::logic_error("image fixture allocation failed");
    }
    auto builder = std::move(*builderResult.value());
    for (std::uint32_t y = 0; y < imageDescriptor.dataWindow().extent().height(); ++y) {
        const auto row = builder.row(imageDescriptor.dataWindow().originY() + y);
        if (!row) {
            throw std::logic_error("image fixture row failed");
        }
        for (std::uint32_t x = 0; x < imageDescriptor.dataWindow().extent().width(); ++x) {
            (*row.value())[x] =
                pixels[static_cast<std::size_t>(y) * imageDescriptor.dataWindow().extent().width() +
                       x];
        }
    }
    auto image = std::move(builder).freeze();
    if (!image) {
        throw std::logic_error("image fixture freeze failed");
    }
    return std::move(*image.value());
}

template <typename T>
concept BorrowsMutableRowFromRvalue = requires(T value) { std::move(value).row(0); };

static_assert(!BorrowsMutableRowFromRvalue<ReferenceDisplayBufferBuilder>);
static_assert(!std::is_copy_constructible_v<ReferenceDisplayBufferBuilder>);
static_assert(std::is_nothrow_move_constructible_v<ReferenceDisplayBufferBuilder>);

void testDisplayBuilder(Expectations& expectations) {
    const auto descriptorResult =
        ReferenceDisplayBufferDescriptor::create(window(-2, 3, 2, 2), PixelAspectRatio::square());
    if (!descriptorResult) {
        expectations.expect(false, "display descriptor fixture succeeds");
        return;
    }
    const auto displayDescriptor = *descriptorResult.value();
    const auto bytes = displayDescriptor.layout().pixelStorageBytes;
    const auto belowBudget = ReferenceDisplayBufferBuilder::create(displayDescriptor, bytes - 1);
    expectations.expect(hasError(belowBudget, ImageErrorCode::PixelStorageBudgetExceeded) &&
                            belowBudget.error()->requestedPixelStorageBytes == bytes &&
                            belowBudget.error()->pixelStorageByteLimit == bytes - 1,
                        "display builder enforces its exact payload budget");

    auto builderResult = ReferenceDisplayBufferBuilder::create(displayDescriptor, bytes);
    if (!builderResult) {
        expectations.expect(false, "display builder exact budget succeeds");
        return;
    }
    auto sourceBuilder = std::move(*builderResult.value());
    expectations.expect(sourceBuilder.isValid() && sourceBuilder.pixels().size() == 4 &&
                            sourceBuilder.pixels()[0] == Rgba8{0, 0, 0, 0},
                        "display builder starts as deterministic transparent storage");
    const auto row = sourceBuilder.row(3);
    expectations.expect(row && row.value()->size() == 2,
                        "display builder exposes origin-aware mutable rows");
    if (row) {
        (*row.value())[0] = Rgba8{1, 2, 3, 4};
        (*row.value())[1] = Rgba8{5, 6, 7, 8};
    }
    expectations.expect(hasError(sourceBuilder.row(2), ImageErrorCode::CoordinateOutOfBounds),
                        "display builder rejects rows outside its display window");

    auto movedBuilder = std::move(sourceBuilder);
    expectations.expect(
        !sourceBuilder.isValid() && // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            sourceBuilder.descriptor() == nullptr && movedBuilder.isValid(),
        "display builder moves leave a coherent invalid source");
    const auto* const payload = movedBuilder.pixels().data();
    auto bufferResult = std::move(movedBuilder).freeze();
    expectations.expect(
        !movedBuilder.isValid() && // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            bufferResult && bufferResult.value()->pixels().data() == payload &&
            bufferResult.value()->pixels()[0] == Rgba8{1, 2, 3, 4},
        "display builder freeze transfers its payload without a second copy");
}

void testSolidAndParameters(Expectations& expectations) {
    using bloom::render::kCpuImagePrimitiveSemanticsVersion;
    // ADAPTED (blend modes): the Layer Stack stage folds through the blend kernel now.
    expectations.expect(kCpuImagePrimitiveSemanticsVersion == 5,
                        "CPU image primitive semantics are explicitly versioned");

    const auto solid = solidPixelFromStraightLinearRec709Scene(Color4d{0.5, -2.0, 4.0, 0.25});
    expectations.expect(solid && *solid.value() == pixel(0.125F, -0.5F, 1.0F, 0.25F),
                        "solid conversion premultiplies straight v1 authoring color");
    const auto wideStraight = static_cast<double>(std::numeric_limits<float>::max()) * 2.0;
    const auto wideSolid =
        solidPixelFromStraightLinearRec709Scene(Color4d{wideStraight, 0.0, 0.0, 0.25});
    expectations.expect(
        wideSolid && wideSolid.value()->red() == static_cast<float>(wideStraight * 0.25),
        "solid conversion premultiplies in Float64 before checked Float32 rounding");
    const auto tinyAlphaSolid = solidPixelFromStraightLinearRec709Scene(Color4d{
        std::numeric_limits<double>::max(), 1.0, -1.0, std::numeric_limits<double>::denorm_min()});
    expectations.expect(tinyAlphaSolid && *tinyAlphaSolid.value() == Rgba32f::transparent(),
                        "alpha rounded to Float32 zero canonicalizes every channel");
    const auto zeroAlphaSolid = solidPixelFromStraightLinearRec709Scene(
        Color4d{std::numeric_limits<double>::max(), -2.0, 3.0, 0.0});
    expectations.expect(zeroAlphaSolid && *zeroAlphaSolid.value() == Rgba32f::transparent(),
                        "authored alpha zero has an exact transparent representation");

    const auto unrepresentable =
        solidPixelFromStraightLinearRec709Scene(Color4d{wideStraight, 0.0, 0.0, 1.0});
    const auto invalidColor = solidPixelFromStraightLinearRec709Scene(
        Color4d{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 1.0});
    expectations.expect(hasError(unrepresentable, ImageErrorCode::NonFiniteResult) &&
                            hasError(invalidColor, ImageErrorCode::InvalidParameter),
                        "solid conversion distinguishes bad input from unrepresentable output");

    const auto parameters = TranslationOpacity::create(-2.25, 3.5, 0.1);
    expectations.expect(parameters && parameters.value()->translationX() == -2.25 &&
                            parameters.value()->translationY() == 3.5 &&
                            parameters.value()->opacity() == static_cast<float>(0.1),
                        "layer parameters retain translation and bind opacity once to Float32");
    expectations.expect(
        hasError(TranslationOpacity::create(0.0, 0.0, -0.1), ImageErrorCode::InvalidParameter) &&
            hasError(TranslationOpacity::create(std::numeric_limits<double>::infinity(), 0.0, 1.0),
                     ImageErrorCode::InvalidParameter),
        "layer parameters reject domain and finiteness violations");

    std::array row{Rgba32f::transparent(), Rgba32f::transparent(), Rgba32f::transparent()};
    fillSolidRow(row, pixel(-2.0F, 4.0F, 0.5F, 0.25F));
    expectations.expect(row[0] == row[1] && row[1] == row[2] && row[0].red() == -2.0F,
                        "solid row fill preserves negative and HDR process RGB");
}

void testTranslationAndOpacity(Expectations& expectations) {
    const auto imageWindow = window(-7, 11, 2, 2);
    const auto imageDescriptor = descriptor(imageWindow, imageWindow);
    const std::array sourcePixels{
        pixel(1.0F, 0.0F, 0.0F, 1.0F),
        pixel(0.0F, 1.0F, 0.0F, 1.0F),
        pixel(0.0F, 0.0F, 1.0F, 1.0F),
        pixel(1.0F, 1.0F, 1.0F, 1.0F),
    };
    const auto sourceImage = makeImage(imageDescriptor, sourcePixels);
    const auto sourceView = sourceImage.view();
    if (!sourceView) {
        expectations.expect(false, "translation source view fixture succeeds");
        return;
    }

    const auto halfTranslation = TranslationOpacity::create(0.5, 0.5, 1.0);
    std::array firstRow{Rgba32f::transparent(), Rgba32f::transparent()};
    std::array secondRow{Rgba32f::transparent(), Rgba32f::transparent()};
    const auto firstStatus = translateOpacityBilinearRow(*sourceView.value(), imageWindow, 11,
                                                         *halfTranslation.value(), firstRow);
    const auto secondStatus = translateOpacityBilinearRow(*sourceView.value(), imageWindow, 12,
                                                          *halfTranslation.value(), secondRow);
    expectations.expect(
        !firstStatus.has_value() && !secondStatus.has_value() &&
            firstRow[0] == pixel(0.25F, 0.0F, 0.0F, 0.25F) &&
            firstRow[1] == pixel(0.25F, 0.25F, 0.0F, 0.5F) &&
            secondRow[0] == pixel(0.25F, 0.0F, 0.25F, 0.5F) &&
            secondRow[1] == pixel(0.5F, 0.5F, 0.5F, 1.0F),
        "fractional translation uses pixel-center bilinear gather and per-tap transparent borders");

    const auto identity = TranslationOpacity::create(0.0, 0.0, 1.0);
    std::array identityRow{Rgba32f::transparent(), Rgba32f::transparent()};
    expectations.expect(!translateOpacityBilinearRow(*sourceView.value(), imageWindow, 11,
                                                     *identity.value(), identityRow)
                                .has_value() &&
                            identityRow[0] == sourcePixels[0] && identityRow[1] == sourcePixels[1],
                        "integer identity translation preserves source pixels exactly");

    const auto halfOpacity = TranslationOpacity::create(0.0, 0.0, 0.5);
    std::array opacityRow{Rgba32f::transparent(), Rgba32f::transparent()};
    expectations.expect(!translateOpacityBilinearRow(*sourceView.value(), imageWindow, 11,
                                                     *halfOpacity.value(), opacityRow)
                                .has_value() &&
                            opacityRow[0] == pixel(0.5F, 0.0F, 0.0F, 0.5F) &&
                            opacityRow[1] == pixel(0.0F, 0.5F, 0.0F, 0.5F),
                        "layer opacity scales all four premultiplied components after sampling");

    const auto zeroOpacity = TranslationOpacity::create(0.0, 0.0, 0.0);
    std::array<Rgba32f, 2> zeroRow{sourcePixels[0], sourcePixels[1]};
    expectations.expect(!translateOpacityBilinearRow(*sourceView.value(), imageWindow, 11,
                                                     *zeroOpacity.value(), zeroRow)
                                .has_value() &&
                            zeroRow[0] == Rgba32f::transparent() &&
                            zeroRow[1] == Rgba32f::transparent(),
                        "opacity zero returns exact transparent pixels without sampling");

    const auto hugeTranslation = TranslationOpacity::create(
        std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(), 1.0);
    std::array<Rgba32f, 2> clippedRow{sourcePixels[0], sourcePixels[1]};
    expectations.expect(!translateOpacityBilinearRow(*sourceView.value(), imageWindow, 11,
                                                     *hugeTranslation.value(), clippedRow)
                                .has_value() &&
                            clippedRow[0] == Rgba32f::transparent() &&
                            clippedRow[1] == Rgba32f::transparent(),
                        "huge finite translation clips without unsafe coordinate conversion");

    std::array wrongSize{Rgba32f::transparent()};
    expectations.expect(
        hasError(translateOpacityBilinearRow(*sourceView.value(), imageWindow, 11,
                                             *identity.value(), wrongSize),
                 ImageErrorCode::InvalidStorageSize) &&
            hasError(translateOpacityBilinearRow(Rgba32fImageView{}, imageWindow, 11,
                                                 *identity.value(), identityRow),
                     ImageErrorCode::InvalidState) &&
            hasError(translateOpacityBilinearRow(*sourceView.value(), imageWindow, 13,
                                                 *identity.value(), identityRow),
                     ImageErrorCode::CoordinateOutOfBounds),
        "translation row reports malformed storage, source state, and coordinates distinctly");
}

// The affine layer resample. Every expectation here is a hand-computed consequence of the
// documented model -- inverse map, then pixel-centre bilinear gather with transparent taps outside
// the source -- never a value copied out of the implementation.
[[nodiscard]] LayerTransform transform(const LayerTransform::Authored authored,
                                       const ImageWindow sourceWindow,
                                       const double proxyScaleX = 1.0,
                                       const double proxyScaleY = 1.0) {
    const auto result = LayerTransform::create(authored, sourceWindow, proxyScaleX, proxyScaleY);
    if (!result) {
        throw std::logic_error("invalid layer transform fixture");
    }
    return *result.value();
}

void testLayerTransformTranslationIsBitIdenticalToThePreviousPrimitive(Expectations& expectations) {
    // The pin the task's "identical output to today for scale 1 / rotation 0" requirement names:
    // the retained pre-S4 primitive and the new one must agree BIT FOR BIT on a subpixel
    // translation, and must keep agreeing when an anchor is authored, because an anchor is
    // algebraically irrelevant to a translate-only layer and must therefore be numerically
    // irrelevant too.
    const auto imageWindow = window(0, 0, 4, 3);
    const auto imageDescriptor = descriptor(imageWindow, imageWindow);
    const std::array sourcePixels{
        pixel(0.25F, 0.5F, 0.75F, 1.0F),    pixel(1.0F, 0.0F, 0.0F, 1.0F),
        pixel(0.0F, 0.125F, 0.0F, 0.25F),   pixel(0.5F, 0.5F, 0.5F, 0.5F),
        pixel(0.0F, 0.0F, 1.0F, 1.0F),      pixel(0.125F, 0.25F, 0.375F, 0.5F),
        pixel(1.0F, 1.0F, 1.0F, 1.0F),      Rgba32f::transparent(),
        pixel(0.75F, 0.25F, 0.0F, 0.75F),   pixel(0.0F, 0.5F, 0.5F, 0.5F),
        pixel(0.0625F, 0.0F, 0.0F, 0.125F), pixel(0.9F, 0.8F, 0.7F, 1.0F),
    };
    const auto sourceImage = makeImage(imageDescriptor, sourcePixels);
    const auto sourceView = sourceImage.view();
    if (!sourceView) {
        expectations.expect(false, "layer transform source view fixture succeeds");
        return;
    }

    constexpr double kSubpixelX = 0.3;
    constexpr double kSubpixelY = -0.7;
    constexpr double kOpacity = 0.625;
    const auto legacy = TranslationOpacity::create(kSubpixelX, kSubpixelY, kOpacity);
    if (!legacy) {
        expectations.expect(false, "legacy translation fixture succeeds");
        return;
    }
    // A deliberately off-centre anchor, an exact 360 degree rotation, and unit scale: all three
    // resolve to the translate-only path.
    const auto affine = transform({.translationX = kSubpixelX,
                                   .translationY = kSubpixelY,
                                   .anchorX = -37.25,
                                   .anchorY = 11.5,
                                   .rotationDegrees = -360.0,
                                   .opacity = kOpacity},
                                  imageWindow);
    expectations.expect(
        affine.isTranslationOnly(),
        "unit scale with a whole-turn rotation resolves to the translate-only path");

    bool identical = true;
    for (std::int64_t y = imageWindow.originY(); y < imageWindow.maxYExclusive(); ++y) {
        auto legacyRow = transparentRow<4>();
        auto affineRow = transparentRow<4>();
        const auto legacyStatus = translateOpacityBilinearRow(*sourceView.value(), imageWindow, y,
                                                              *legacy.value(), legacyRow);
        const auto affineStatus =
            layerTransformBilinearRow(*sourceView.value(), imageWindow, y, affine, affineRow);
        identical = identical && !legacyStatus.has_value() && !affineStatus.has_value() &&
                    legacyRow == affineRow;
    }
    expectations.expect(identical,
                        "a translate-only layer resamples bit-identically to the retained pre-S4 "
                        "primitive, anchor included");
}

void testLayerTransformQuarterTurnsAreExact(Expectations& expectations) {
    const auto imageWindow = window(0, 0, 2, 2);
    const auto imageDescriptor = descriptor(imageWindow, imageWindow);
    const std::array sourcePixels{
        pixel(1.0F, 0.0F, 0.0F, 1.0F),
        pixel(0.0F, 1.0F, 0.0F, 1.0F),
        pixel(0.0F, 0.0F, 1.0F, 1.0F),
        pixel(1.0F, 1.0F, 0.0F, 1.0F),
    };
    const auto sourceImage = makeImage(imageDescriptor, sourcePixels);
    const auto sourceView = sourceImage.view();
    if (!sourceView) {
        expectations.expect(false, "quarter turn source view fixture succeeds");
        return;
    }
    const auto rowsOf = [&](const LayerTransform& value) {
        std::array<std::array<Rgba32f, 2>, 2> rows{transparentRow<2>(), transparentRow<2>()};
        bool ok = true;
        for (std::int64_t y = 0; y < 2; ++y) {
            ok = ok && !layerTransformBilinearRow(*sourceView.value(), imageWindow, y, value,
                                                  rows[static_cast<std::size_t>(y)])
                            .has_value();
        }
        return std::pair{ok, rows};
    };

    // A 2x2 layer's pixel-area centre is (0.5, 0.5). A quarter turn clockwise about it maps source
    // (u, v) to output (1 - v, u), so output (x, y) samples source (y, 1 - x) -- every sample
    // coordinate an exact integer, every bilinear factor exactly zero, every output pixel therefore
    // an exact copy of one source pixel.
    const auto [quarterOk, quarter] = rowsOf(transform({.rotationDegrees = 90.0}, imageWindow));
    expectations.expect(quarterOk && quarter[0][0] == sourcePixels[2] &&
                            quarter[0][1] == sourcePixels[0] && quarter[1][0] == sourcePixels[3] &&
                            quarter[1][1] == sourcePixels[1],
                        "a 90 degree rotation is an exact clockwise permutation of pixel centres");

    // Half a turn is the exact reversal of both axes.
    const auto [halfOk, half] = rowsOf(transform({.rotationDegrees = 180.0}, imageWindow));
    expectations.expect(halfOk && half[0][0] == sourcePixels[3] && half[0][1] == sourcePixels[2] &&
                            half[1][0] == sourcePixels[1] && half[1][1] == sourcePixels[0],
                        "a 180 degree rotation is an exact reversal of both axes");

    // 270 and -90 name the same quarter turn, and both must be exact.
    const auto [threeQuarterOk, threeQuarter] =
        rowsOf(transform({.rotationDegrees = 270.0}, imageWindow));
    const auto [negativeOk, negative] = rowsOf(transform({.rotationDegrees = -90.0}, imageWindow));
    expectations.expect(
        threeQuarterOk && negativeOk && threeQuarter == negative &&
            threeQuarter[0][0] == sourcePixels[1] && threeQuarter[0][1] == sourcePixels[3] &&
            threeQuarter[1][0] == sourcePixels[0] && threeQuarter[1][1] == sourcePixels[2],
        "270 and -90 degrees are the same exact quarter turn");

    // 450 degrees winds past a full turn and must land exactly where 90 does: the wrap is exact
    // arithmetic, not an approximation.
    const auto [woundOk, wound] = rowsOf(transform({.rotationDegrees = 450.0}, imageWindow));
    expectations.expect(woundOk && wound == quarter,
                        "a rotation wound past a full turn is exactly its reduced angle");

    // Anchored at the top-left pixel centre instead -- anchor (-0.5, -0.5) offsets the centre by
    // half a pixel on each axis -- half a turn carries every other pixel off the layer, leaving
    // only the anchored pixel itself.
    const auto [cornerOk, corner] = rowsOf(
        transform({.anchorX = -0.5, .anchorY = -0.5, .rotationDegrees = 180.0}, imageWindow));
    expectations.expect(
        cornerOk && corner[0][0] == sourcePixels[0] && corner[0][1] == Rgba32f::transparent() &&
            corner[1][0] == Rgba32f::transparent() && corner[1][1] == Rgba32f::transparent(),
        "an anchor at a corner pivots about that corner, not about the centre");
}

void testLayerTransformScaleAndBounds(Expectations& expectations) {
    const auto imageWindow = window(0, 0, 2, 2);
    const auto imageDescriptor = descriptor(imageWindow, imageWindow);
    const auto white = pixel(1.0F, 1.0F, 1.0F, 1.0F);
    const std::array sourcePixels{white, Rgba32f::transparent(), Rgba32f::transparent(), white};
    const auto sourceImage = makeImage(imageDescriptor, sourcePixels);
    const auto sourceView = sourceImage.view();
    if (!sourceView) {
        expectations.expect(false, "scale source view fixture succeeds");
        return;
    }

    // Half scale about the centre (0.5, 0.5) inverts to u = 0.5 + 2 (x - 0.5), so the four output
    // pixel centres sample at -0.5 and 1.5 on each axis. Each in-range sample sits exactly half a
    // pixel outside the checker's white corner, so it reads half that corner against a transparent
    // tap on one axis and half again on the other: one quarter of white.
    const auto halved = transform({.scaleX = 0.5, .scaleY = 0.5}, imageWindow);
    const auto quarterWhite = pixel(0.25F, 0.25F, 0.25F, 0.25F);
    auto firstRow = transparentRow<2>();
    auto secondRow = transparentRow<2>();
    const auto firstStatus =
        layerTransformBilinearRow(*sourceView.value(), imageWindow, 0, halved, firstRow);
    const auto secondStatus =
        layerTransformBilinearRow(*sourceView.value(), imageWindow, 1, halved, secondRow);
    expectations.expect(!firstStatus.has_value() && !secondStatus.has_value() &&
                            firstRow[0] == quarterWhite && firstRow[1] == Rgba32f::transparent() &&
                            secondRow[0] == Rgba32f::transparent() && secondRow[1] == quarterWhite,
                        "half scale resamples a 2x2 checker to exact quarter-weighted corners");

    // Bounds. The bilinear support of a 2x2 layer is the closed box [-1, 2] on each axis, so an
    // unscaled, unmoved layer covers the whole 2x2 composition.
    const auto identity = transform({}, imageWindow);
    const auto identityBounds = identity.supportBounds(imageWindow);
    expectations.expect(identityBounds.has_value() && *identityBounds == imageWindow,
                        "an unmoved layer's data window is the whole composition");

    // Moved one pixel right, the support starts at output column 0 still (the -1 tap) but the layer
    // can no longer reach anything left of it; clipped to the composition it is columns 0..1.
    const auto moved = transform({.translationX = 1.0}, imageWindow);
    const auto movedBounds = moved.supportBounds(imageWindow);
    expectations.expect(movedBounds.has_value() && *movedBounds == imageWindow,
                        "a one-pixel move still reaches every column of a 2x2 composition");

    // Far enough away and the layer reaches nothing at all, so it needs no image.
    const auto gone = transform({.translationX = 100.0, .translationY = 100.0}, imageWindow);
    expectations.expect(!gone.supportBounds(imageWindow).has_value(),
                        "a layer carried off the composition has no data window at all");

    // A larger composition shows the clipping doing real work: moved three pixels right inside an
    // 8x8 frame, a 2x2 layer's support is output columns 2..5 and rows -1..2 clipped to 0..2.
    const auto frame = window(0, 0, 8, 8);
    const auto insideFrame = transform({.translationX = 3.0}, imageWindow);
    const auto frameBounds = insideFrame.supportBounds(frame);
    expectations.expect(frameBounds.has_value() && *frameBounds == window(2, 0, 4, 3),
                        "a layer's data window is its transformed bounds clipped to the frame");
}

void testLayerTransformProxyAndRejections(Expectations& expectations) {
    const auto imageWindow = window(0, 0, 4, 4);
    const auto imageDescriptor = descriptor(imageWindow, imageWindow);
    auto sourcePixels = transparentRow<16>();
    sourcePixels[5] = pixel(1.0F, 0.0F, 0.0F, 1.0F);
    const auto sourceImage = makeImage(imageDescriptor, sourcePixels);
    const auto sourceView = sourceImage.view();
    if (!sourceView) {
        expectations.expect(false, "proxy source view fixture succeeds");
        return;
    }

    // Proxy factors scale the authored translation and anchor into device pixels. At twice the
    // composition extent an authored one-pixel move is a two-pixel device move, which is what keeps
    // a proxy frame the same picture at a different size.
    const auto proxied = transform({.translationX = 1.0}, imageWindow, 2.0, 2.0);
    auto proxyRow = transparentRow<4>();
    const auto proxyStatus =
        layerTransformBilinearRow(*sourceView.value(), imageWindow, 1, proxied, proxyRow);
    expectations.expect(!proxyStatus.has_value() && proxyRow[3] == sourcePixels[5] &&
                            proxyRow[1] == Rgba32f::transparent(),
                        "a proxy factor scales the authored translation into device pixels");

    const auto identity = transform({}, imageWindow);
    auto row = transparentRow<4>();
    const auto otherWindow = window(1, 0, 4, 4);
    expectations.expect(
        hasError(layerTransformBilinearRow(*sourceView.value(), otherWindow, 0,
                                           transform({}, otherWindow), row),
                 ImageErrorCode::IncompatibleImageDescriptor),
        "the resample refuses a source whose data window is not the one it was built for");
    expectations.expect(
        hasError(layerTransformBilinearRow(Rgba32fImageView{}, imageWindow, 0, identity, row),
                 ImageErrorCode::InvalidState) &&
            hasError(layerTransformBilinearRow(*sourceView.value(), imageWindow, 9, identity, row),
                     ImageErrorCode::CoordinateOutOfBounds),
        "the resample reports source state and coordinate failures distinctly");
    auto wrongSize = transparentRow<1>();
    expectations.expect(hasError(layerTransformBilinearRow(*sourceView.value(), imageWindow, 0,
                                                           identity, wrongSize),
                                 ImageErrorCode::InvalidStorageSize),
                        "the resample reports a row span that does not match its window");

    const auto rejects = [&](const LayerTransform::Authored authored, const double proxyX = 1.0,
                             const double proxyY = 1.0) {
        const auto result = LayerTransform::create(authored, imageWindow, proxyX, proxyY);
        return !result && result.error() != nullptr &&
               (result.error()->code == ImageErrorCode::InvalidParameter ||
                result.error()->code == ImageErrorCode::NonFiniteResult);
    };
    const auto infinity = std::numeric_limits<double>::infinity();
    expectations.expect(rejects({.scaleX = 0.0}) && rejects({.scaleY = 0.0}),
                        "a collapsed layer has no inverse and is refused rather than resampled");
    expectations.expect(rejects({.translationX = infinity}) && rejects({.anchorY = infinity}) &&
                            rejects({.rotationDegrees = infinity}) && rejects({.opacity = -0.5}) &&
                            rejects({.opacity = 1.5}) && rejects({}, 0.0) && rejects({}, 1.0, -2.0),
                        "non-finite values, an out-of-range opacity, and a non-positive proxy "
                        "factor are all refused");

    // A negative scale factor is authorable and mirrors the axis: a 4x4 layer's centre is 1.5, so
    // x mirrors to 3 - x.
    const auto mirrored = transform({.scaleX = -1.0}, imageWindow);
    auto mirroredRow = transparentRow<4>();
    expectations.expect(
        !layerTransformBilinearRow(*sourceView.value(), imageWindow, 1, mirrored, mirroredRow)
                .has_value() &&
            mirroredRow[2] == sourcePixels[5] && mirroredRow[1] == Rgba32f::transparent(),
        "a negative scale factor mirrors the axis exactly");
}

void testSourceOver(Expectations& expectations) {
    const std::array source{
        pixel(0.5F, 0.0F, 0.0F, 0.5F),
        Rgba32f::transparent(),
        pixel(-2.0F, 4.0F, 0.5F, 1.0F),
        pixel(-1.0F, 2.0F, 0.0F, 0.5F),
    };
    std::array destination{
        pixel(0.0F, 0.0F, 0.5F, 0.5F),
        pixel(1.0F, 2.0F, 3.0F, 0.25F),
        pixel(8.0F, 8.0F, 8.0F, 0.5F),
        pixel(4.0F, -2.0F, 1.0F, 0.5F),
    };
    const auto originalTransparentDestination = destination[1];
    const auto status = sourceOverLinearRec709SceneRow(source, destination);
    expectations.expect(
        !status.has_value() && destination[0] == pixel(0.5F, 0.0F, 0.25F, 0.75F) &&
            destination[1] == originalTransparentDestination && destination[2] == source[2] &&
            destination[3] == pixel(1.0F, 1.0F, 0.5F, 0.75F),
        "ordered source-over preserves alpha endpoints and negative/HDR scene-linear RGB");

    std::array<Rgba32f, 1> wrongSize{Rgba32f::transparent()};
    expectations.expect(hasError(sourceOverLinearRec709SceneRow(source, wrongSize),
                                 ImageErrorCode::InvalidStorageSize),
                        "source-over rejects unequal row sizes");
    auto aliased = source;
    expectations.expect(
        hasError(sourceOverLinearRec709SceneRow(std::span<const Rgba32f>(aliased), aliased),
                 ImageErrorCode::InvalidParameter),
        "source-over rejects source storage that aliases its in-place destination");

    const auto maximum = std::numeric_limits<float>::max();
    const std::array overflowingSource{pixel(maximum, 0.0F, 0.0F, 0.5F)};
    std::array overflowingDestination{pixel(maximum, 0.0F, 0.0F, 0.5F)};
    expectations.expect(
        hasError(sourceOverLinearRec709SceneRow(overflowingSource, overflowingDestination),
                 ImageErrorCode::NonFiniteResult),
        "source-over reports finite-input RGB overflow without clamping");
}

// The blend-mode goldens, on ONE 2x2 premultiplied fixture carrying every awkward case the process
// representation allows: partial alpha on both sides, an opaque source over a translucent backdrop,
// an HDR channel above 1 on both sides, and a negative channel.
//
// Every fixture value and every expected value here is dyadic -- exactly representable in binary32
// -- so the goldens are exact algebra rather than a particular rounding, and they were derived from
// the documented formulas (docs/architecture/color-management.md, "Blend modes") independently of
// the implementation rather than captured from its output.
struct BlendGolden final {
    bloom::core::BlendMode mode;
    std::array<Rgba32f, 4> expected;
};

void testBlendModes(Expectations& expectations) {
    using bloom::core::BlendMode;
    const std::array blendSource{
        pixel(0.5F, 0.25F, 0.125F, 0.5F),    // straight (1, 0.5, 0.25) at half alpha
        pixel(2.0F, 0.5F, -0.25F, 1.0F),     // opaque, HDR red, negative blue
        pixel(0.125F, 0.375F, 0.25F, 0.25F), // straight (0.5, 1.5, 1) at quarter alpha
        pixel(0.75F, 0.0F, 0.75F, 0.75F),    // straight (1, 0, 1) at three-quarter alpha
    };
    const std::array blendDestination{
        pixel(0.25F, 0.5F, 0.75F, 1.0F),        // opaque backdrop
        pixel(0.375F, 0.125F, 0.625F, 0.5F),    // straight (0.75, 0.25, 1.25): HDR backdrop blue
        pixel(0.5F, 0.5F, 0.5F, 1.0F),          // opaque mid grey
        pixel(0.0625F, 0.125F, 0.1875F, 0.25F), // straight (0.25, 0.5, 0.75) at quarter alpha
    };
    const std::array<BlendGolden, 8> goldens{{
        {BlendMode::Normal,
         {pixel(0.625F, 0.5F, 0.5F, 1.0F), pixel(2.0F, 0.5F, -0.25F, 1.0F),
          pixel(0.5F, 0.75F, 0.625F, 1.0F), pixel(0.765625F, 0.03125F, 0.796875F, 0.8125F)}},
        {BlendMode::Add,
         {pixel(0.75F, 0.75F, 0.875F, 1.0F), pixel(2.375F, 0.625F, 0.375F, 1.0F),
          pixel(0.625F, 0.875F, 0.75F, 1.0F), pixel(0.8125F, 0.125F, 0.9375F, 0.8125F)}},
        {BlendMode::Multiply,
         {pixel(0.25F, 0.375F, 0.46875F, 1.0F), pixel(1.75F, 0.3125F, -0.28125F, 1.0F),
          pixel(0.4375F, 0.5625F, 0.5F, 1.0F), pixel(0.625F, 0.03125F, 0.75F, 0.8125F)}},
        {BlendMode::Screen,
         {pixel(0.625F, 0.625F, 0.78125F, 1.0F), pixel(1.625F, 0.5625F, 0.53125F, 1.0F),
          pixel(0.5625F, 0.6875F, 0.625F, 1.0F), pixel(0.765625F, 0.125F, 0.796875F, 0.8125F)}},
        {BlendMode::Overlay,
         {pixel(0.375F, 0.5F, 0.6875F, 1.0F), pixel(1.75F, 0.375F, 0.6875F, 1.0F),
          pixel(0.5F, 0.75F, 0.625F, 1.0F), pixel(0.671875F, 0.03125F, 0.796875F, 0.8125F)}},
        {BlendMode::Darken,
         {pixel(0.25F, 0.5F, 0.5F, 1.0F), pixel(1.375F, 0.375F, -0.25F, 1.0F),
          pixel(0.5F, 0.5F, 0.5F, 1.0F), pixel(0.625F, 0.03125F, 0.75F, 0.8125F)}},
        {BlendMode::Lighten,
         {pixel(0.625F, 0.5F, 0.75F, 1.0F), pixel(2.0F, 0.5F, 0.5F, 1.0F),
          pixel(0.5F, 0.75F, 0.625F, 1.0F), pixel(0.765625F, 0.125F, 0.796875F, 0.8125F)}},
        {BlendMode::Difference,
         {pixel(0.5F, 0.25F, 0.625F, 1.0F), pixel(1.625F, 0.375F, 0.625F, 1.0F),
          pixel(0.375F, 0.625F, 0.5F, 1.0F), pixel(0.71875F, 0.125F, 0.65625F, 0.8125F)}},
    }};
    expectations.expect(goldens.size() == bloom::core::kBlendModes.size(),
                        "every implemented blend mode has a golden row");
    for (const auto& golden : goldens) {
        auto destination = blendDestination;
        const auto status = blendLinearRec709SceneRow(golden.mode, blendSource, destination);
        expectations.expect(!status.has_value() && std::ranges::equal(destination, golden.expected),
                            "a blend mode reproduces its documented formula exactly on "
                            "premultiplied alpha < 1 and HDR > 1 pixels");
    }

    // The old behaviour, bit for bit: Normal is not merely algebraically source-over, it IS the
    // retained source-over kernel, so the two must agree on identical storage with no tolerance.
    auto blended = blendDestination;
    auto composited = blendDestination;
    expectations.expect(
        !blendLinearRec709SceneRow(BlendMode::Normal, blendSource, blended).has_value() &&
            !sourceOverLinearRec709SceneRow(blendSource, composited).has_value() &&
            blended == composited,
        "Normal is bit-exactly the pre-blend-mode source-over result");

    // Alpha compositing is source-over under EVERY mode: a mode changes a layer's colour, never how
    // much of the backdrop it covers.
    for (const auto mode : bloom::core::kBlendModes) {
        auto modeDestination = blendDestination;
        auto overDestination = blendDestination;
        const bool ok =
            !blendLinearRec709SceneRow(mode, blendSource, modeDestination).has_value() &&
            !sourceOverLinearRec709SceneRow(blendSource, overDestination).has_value();
        expectations.expect(ok && std::ranges::equal(modeDestination, overDestination,
                                                     [](const Rgba32f left, const Rgba32f right) {
                                                         return left.alpha() == right.alpha();
                                                     }),
                            "every mode composites alpha as source-over");
    }

    // The two alpha endpoints, under a mode that is nowhere near source-over in colour.
    const std::array endpointSource{Rgba32f::transparent(), pixel(0.5F, 0.25F, 0.125F, 0.5F)};
    std::array endpointDestination{pixel(0.25F, 0.5F, 0.75F, 1.0F), Rgba32f::transparent()};
    const auto untouchedBackdrop = endpointDestination[0];
    expectations.expect(
        !blendLinearRec709SceneRow(BlendMode::Difference, endpointSource, endpointDestination)
                .has_value() &&
            endpointDestination[0] == untouchedBackdrop &&
            endpointDestination[1] == endpointSource[1],
        "a transparent source leaves the backdrop alone and a transparent backdrop takes the "
        "source exactly, under every mode");

    std::array<Rgba32f, 1> wrongSize{Rgba32f::transparent()};
    expectations.expect(
        hasError(blendLinearRec709SceneRow(BlendMode::Screen, blendSource, wrongSize),
                 ImageErrorCode::InvalidStorageSize),
        "blending rejects unequal row sizes");
    auto aliased = blendDestination;
    expectations.expect(hasError(blendLinearRec709SceneRow(
                                     BlendMode::Screen, std::span<const Rgba32f>(aliased), aliased),
                                 ImageErrorCode::InvalidParameter),
                        "blending rejects source storage that aliases its in-place destination");

    const auto maximum = std::numeric_limits<float>::max();
    const std::array overflowingSource{pixel(maximum, 0.0F, 0.0F, 1.0F)};
    std::array overflowingDestination{pixel(maximum, 0.0F, 0.0F, 1.0F)};
    expectations.expect(hasError(blendLinearRec709SceneRow(BlendMode::Add, overflowingSource,
                                                           overflowingDestination),
                                 ImageErrorCode::NonFiniteResult),
                        "blending reports finite-input RGB overflow without clamping");
}

void testReferenceDisplayMapping(Expectations& expectations) {
    const auto dataWindow = window(-1, 4, 3, 1);
    const auto displayWindow = window(-2, 4, 5, 1);
    const auto imageDescriptor = descriptor(dataWindow, displayWindow);
    const std::array processPixels{
        Rgba32f::transparent(),
        pixel(0.10702057F, 0.0015654F, 0.25F, 0.5F),
        pixel(-4.0F, 2.0F, 0.25F, 0.5F),
    };
    const auto image = makeImage(imageDescriptor, processPixels);
    const auto view = image.view();
    if (!view) {
        expectations.expect(false, "display source view fixture succeeds");
        return;
    }

    std::array<Rgba8, 5> mapped{};
    const auto status = mapLinearRec709SceneToSrgbRow(*view.value(), displayWindow, 4, mapped);
    const std::array expected{
        Rgba8{0, 0, 0, 0},       Rgba8{0, 0, 0, 0}, Rgba8{128, 10, 188, 128},
        Rgba8{0, 255, 188, 128}, Rgba8{0, 0, 0, 0},
    };
    expectations.expect(
        !status.has_value() && mapped == expected,
        "reference display robustly unpremultiplies, clips, encodes, and quantizes straight RGBA8");

    std::array<Rgba8, 5> repeated{};
    expectations.expect(
        !mapLinearRec709SceneToSrgbRow(*view.value(), displayWindow, 4, repeated).has_value() &&
            repeated == mapped,
        "reference display byte mapping is exactly repeatable");
    expectations.expect(
        hasError(mapLinearRec709SceneToSrgbRow(*view.value(), window(-2, 4, 4, 1), 4, mapped),
                 ImageErrorCode::IncompatibleImageDescriptor) &&
            hasError(mapLinearRec709SceneToSrgbRow(*view.value(), displayWindow, 5, mapped),
                     ImageErrorCode::CoordinateOutOfBounds) &&
            hasError(mapLinearRec709SceneToSrgbRow(*view.value(), displayWindow, 4,
                                                   std::span<Rgba8>(mapped).first(4)),
                     ImageErrorCode::InvalidStorageSize),
        "display mapper reports descriptor, coordinate, and storage contract violations");
}

void testFloatingPointEnvironment(Expectations& expectations) {
    expectations.expect(std::fegetround() == FE_TONEAREST,
                        "render primitive tests begin in round-to-nearest mode");
    const auto imageWindow = window(0, 0, 1, 1);
    const auto image =
        makeImage(descriptor(imageWindow, imageWindow), std::array{pixel(0.25F, 0.5F, 1.0F, 1.0F)});
    const auto view = image.view();
    const auto parameters = TranslationOpacity::create(0.0, 0.0, 1.0);
    std::array processOutput{Rgba32f::transparent()};
    std::array<Rgba8, 1> displayOutput{};

    {
        RoundingModeGuard guard;
        expectations.expect(std::fesetround(FE_DOWNWARD) == 0,
                            "test platform exposes a non-default rounding mode");
        expectations.expect(
            hasError(TranslationOpacity::create(0.0, 0.0, 1.0),
                     ImageErrorCode::UnsupportedFloatingPointEnvironment) &&
                hasError(
                    TranslationOpacity::create(std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0),
                    ImageErrorCode::InvalidParameter) &&
                hasError(solidPixelFromStraightLinearRec709Scene(Color4d{1.0, 0.0, 0.0, 1.0}),
                         ImageErrorCode::UnsupportedFloatingPointEnvironment) &&
                hasError(translateOpacityBilinearRow(*view.value(), imageWindow, 0,
                                                     *parameters.value(), processOutput),
                         ImageErrorCode::UnsupportedFloatingPointEnvironment) &&
                hasError(sourceOverLinearRec709SceneRow(image.pixels(), processOutput),
                         ImageErrorCode::UnsupportedFloatingPointEnvironment) &&
                hasError(
                    mapLinearRec709SceneToSrgbRow(*view.value(), imageWindow, 0, displayOutput),
                    ImageErrorCode::UnsupportedFloatingPointEnvironment),
            "every authored arithmetic boundary rejects non-default rounding after input "
            "validation");
    }
    expectations.expect(std::fegetround() == FE_TONEAREST,
                        "rounding-mode guard restores the reference environment");

#if BLOOM_RENDER_TEST_HAS_MXCSR
    constexpr std::uint32_t kDenormalsAreZero = 1U << 6U;
    constexpr std::uint32_t kFlushToZero = 1U << 15U;
    MxcsrGuard guard;
    const auto baseline = guard.original() & ~(kDenormalsAreZero | kFlushToZero);
    _mm_setcsr(baseline | kFlushToZero);
    expectations.expect(hasError(sourceOverLinearRec709SceneRow(image.pixels(), processOutput),
                                 ImageErrorCode::UnsupportedFloatingPointEnvironment),
                        "source-over rejects flush-to-zero mode");
    _mm_setcsr(baseline | kDenormalsAreZero);
    expectations.expect(
        hasError(mapLinearRec709SceneToSrgbRow(*view.value(), imageWindow, 0, displayOutput),
                 ImageErrorCode::UnsupportedFloatingPointEnvironment),
        "display mapping rejects denormals-are-zero mode");
    _mm_setcsr(baseline);
#endif
}

} // namespace

int main() {
    try {
        Expectations expectations;
        testDisplayBuilder(expectations);
        testSolidAndParameters(expectations);
        testTranslationAndOpacity(expectations);
        testLayerTransformTranslationIsBitIdenticalToThePreviousPrimitive(expectations);
        testLayerTransformQuarterTurnsAreExact(expectations);
        testLayerTransformScaleAndBounds(expectations);
        testLayerTransformProxyAndRejections(expectations);
        testSourceOver(expectations);
        testBlendModes(expectations);
        testReferenceDisplayMapping(expectations);
        testFloatingPointEnvironment(expectations);
        return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
