#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/display_buffer.hpp>

#include <array>
#include <cfenv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
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
using bloom::render::fillSolidRow;
using bloom::render::ImageErrorCode;
using bloom::render::ImageResult;
using bloom::render::ImageStatus;
using bloom::render::ImageWindow;
using bloom::render::mapReferenceLinearSrgbToSrgbRow;
using bloom::render::PreparedReferenceDisplayBuffer;
using bloom::render::ReferenceDisplayBufferBuilder;
using bloom::render::ReferenceDisplayBufferDescriptor;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::render::Rgba32fImageView;
using bloom::render::Rgba8;
using bloom::render::solidPixelFromStraightReferenceLinearSrgb;
using bloom::render::sourceOverReferenceLinearSrgbRow;
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
    expectations.expect(kCpuImagePrimitiveSemanticsVersion == 1,
                        "CPU image primitive semantics are explicitly versioned");

    const auto solid = solidPixelFromStraightReferenceLinearSrgb(Color4d{0.5, -2.0, 4.0, 0.25});
    expectations.expect(solid && *solid.value() == pixel(0.125F, -0.5F, 1.0F, 0.25F),
                        "solid conversion premultiplies straight reference-linear color");
    const auto wideStraight = static_cast<double>(std::numeric_limits<float>::max()) * 2.0;
    const auto wideSolid =
        solidPixelFromStraightReferenceLinearSrgb(Color4d{wideStraight, 0.0, 0.0, 0.25});
    expectations.expect(
        wideSolid && wideSolid.value()->red() == static_cast<float>(wideStraight * 0.25),
        "solid conversion premultiplies in Float64 before checked Float32 rounding");
    const auto tinyAlphaSolid = solidPixelFromStraightReferenceLinearSrgb(Color4d{
        std::numeric_limits<double>::max(), 1.0, -1.0, std::numeric_limits<double>::denorm_min()});
    expectations.expect(tinyAlphaSolid && *tinyAlphaSolid.value() == Rgba32f::transparent(),
                        "alpha rounded to Float32 zero canonicalizes every channel");
    const auto zeroAlphaSolid = solidPixelFromStraightReferenceLinearSrgb(
        Color4d{std::numeric_limits<double>::max(), -2.0, 3.0, 0.0});
    expectations.expect(zeroAlphaSolid && *zeroAlphaSolid.value() == Rgba32f::transparent(),
                        "authored alpha zero has an exact transparent representation");

    const auto unrepresentable =
        solidPixelFromStraightReferenceLinearSrgb(Color4d{wideStraight, 0.0, 0.0, 1.0});
    const auto invalidColor = solidPixelFromStraightReferenceLinearSrgb(
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
    const auto status = sourceOverReferenceLinearSrgbRow(source, destination);
    expectations.expect(
        !status.has_value() && destination[0] == pixel(0.5F, 0.0F, 0.25F, 0.75F) &&
            destination[1] == originalTransparentDestination && destination[2] == source[2] &&
            destination[3] == pixel(1.0F, 1.0F, 0.5F, 0.75F),
        "ordered source-over preserves alpha endpoints and negative/HDR scene-linear RGB");

    std::array<Rgba32f, 1> wrongSize{Rgba32f::transparent()};
    expectations.expect(hasError(sourceOverReferenceLinearSrgbRow(source, wrongSize),
                                 ImageErrorCode::InvalidStorageSize),
                        "source-over rejects unequal row sizes");
    auto aliased = source;
    expectations.expect(
        hasError(sourceOverReferenceLinearSrgbRow(std::span<const Rgba32f>(aliased), aliased),
                 ImageErrorCode::InvalidParameter),
        "source-over rejects source storage that aliases its in-place destination");

    const auto maximum = std::numeric_limits<float>::max();
    const std::array overflowingSource{pixel(maximum, 0.0F, 0.0F, 0.5F)};
    std::array overflowingDestination{pixel(maximum, 0.0F, 0.0F, 0.5F)};
    expectations.expect(
        hasError(sourceOverReferenceLinearSrgbRow(overflowingSource, overflowingDestination),
                 ImageErrorCode::NonFiniteResult),
        "source-over reports finite-input RGB overflow without clamping");
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
    const auto status = mapReferenceLinearSrgbToSrgbRow(*view.value(), displayWindow, 4, mapped);
    const std::array expected{
        Rgba8{0, 0, 0, 0},       Rgba8{0, 0, 0, 0}, Rgba8{128, 10, 188, 128},
        Rgba8{0, 255, 188, 128}, Rgba8{0, 0, 0, 0},
    };
    expectations.expect(
        !status.has_value() && mapped == expected,
        "reference display robustly unpremultiplies, clips, encodes, and quantizes straight RGBA8");

    std::array<Rgba8, 5> repeated{};
    expectations.expect(
        !mapReferenceLinearSrgbToSrgbRow(*view.value(), displayWindow, 4, repeated).has_value() &&
            repeated == mapped,
        "reference display byte mapping is exactly repeatable");
    expectations.expect(
        hasError(mapReferenceLinearSrgbToSrgbRow(*view.value(), window(-2, 4, 4, 1), 4, mapped),
                 ImageErrorCode::IncompatibleImageDescriptor) &&
            hasError(mapReferenceLinearSrgbToSrgbRow(*view.value(), displayWindow, 5, mapped),
                     ImageErrorCode::CoordinateOutOfBounds) &&
            hasError(mapReferenceLinearSrgbToSrgbRow(*view.value(), displayWindow, 4,
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
                hasError(solidPixelFromStraightReferenceLinearSrgb(Color4d{1.0, 0.0, 0.0, 1.0}),
                         ImageErrorCode::UnsupportedFloatingPointEnvironment) &&
                hasError(translateOpacityBilinearRow(*view.value(), imageWindow, 0,
                                                     *parameters.value(), processOutput),
                         ImageErrorCode::UnsupportedFloatingPointEnvironment) &&
                hasError(sourceOverReferenceLinearSrgbRow(image.pixels(), processOutput),
                         ImageErrorCode::UnsupportedFloatingPointEnvironment) &&
                hasError(
                    mapReferenceLinearSrgbToSrgbRow(*view.value(), imageWindow, 0, displayOutput),
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
    expectations.expect(hasError(sourceOverReferenceLinearSrgbRow(image.pixels(), processOutput),
                                 ImageErrorCode::UnsupportedFloatingPointEnvironment),
                        "source-over rejects flush-to-zero mode");
    _mm_setcsr(baseline | kDenormalsAreZero);
    expectations.expect(
        hasError(mapReferenceLinearSrgbToSrgbRow(*view.value(), imageWindow, 0, displayOutput),
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
        testSourceOver(expectations);
        testReferenceDisplayMapping(expectations);
        testFloatingPointEnvironment(expectations);
        return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
