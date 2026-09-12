#include <bloom/render/cpu_image_primitives.hpp>

#include <bloom/core/floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>

namespace {

using bloom::render::ImageError;
using bloom::render::ImageErrorCode;
using bloom::render::ImageResult;
using bloom::render::Rgba32f;

using RawPixel = std::array<float, 4>;

// Each entry is the reference-linear value at the exact half-code transition between adjacent
// 8-bit sRGB codes. Runtime mapping is comparison-only, avoiding platform libm differences.
constexpr std::array<double, 255> kSrgbHalfCodeLinearThresholds{
    0x1.3e45677c176f7p-13, 0x1.dd681b3a23272p-12, 0x1.8dd6c15b1d4b4p-11, 0x1.167cba8c94818p-10,
    0x1.660e146b9a5d5p-10, 0x1.b59f6e4aa0393p-10, 0x1.02986414d30a8p-9,  0x1.2a61110455f87p-9,
    0x1.5229bdf3d8e66p-9,  0x1.79f26ae35bd45p-9,  0x1.a1e5a03a8a4b8p-9,  0x1.cbf734477e0eap-9,
    0x1.f8680590912c3p-9,  0x1.13a0be3e98ad6p-8,  0x1.2c4665c6bc58dp-8,  0x1.4629793a399b2p-8,
    0x1.614e60755415cp-8,  0x1.7db96ca0c99dbp-8,  0x1.9b6ed95fb6dbep-8,  0x1.ba72cde4cb5cbp-8,
    0x1.dac95df18329cp-8,  0x1.fc768ac1bd734p-8,  0x1.0fbf21f2dc48cp-7,  0x1.21f234061c55cp-7,
    0x1.34d662df32ddep-7,  0x1.486d8e075e9d7p-7,  0x1.5cb98d9e25461p-7,  0x1.71bc32a59ad48p-7,
    0x1.87774749cc7c7p-7,  0x1.9dec8f23ba5c1p-7,  0x1.b51dc7783fa2cp-7,  0x1.cd0ca7733ec86p-7,
    0x1.e5bae05f5ea9bp-7,  0x1.ff2a1dda9da48p-7,  0x1.0cae0303fc81ep-6,  0x1.1a291cdf30865p-6,
    0x1.28072a5a9656bp-6,  0x1.3648f6d71d8d6p-6,  0x1.44ef4b4ef6b8fp-6,  0x1.53faee688f618p-6,
    0x1.636ca4889ebeep-6,  0x1.73452fe3532a8p-6,  0x1.8385508caeef9p-6,  0x1.942dc48821f77p-6,
    0x1.a53f47d76ca88p-6,  0x1.b6ba9488d7614p-6,  0x1.c8a062c4c9087p-6,  0x1.daf168dac6650p-6,
    0x1.edae5b4de330ep-6,  0x1.006bf67056983p-5,  0x1.0a3767504c7e5p-5,  0x1.1439d7f87bcfcp-5,
    0x1.1e739f4abdd1dp-5,  0x1.28e5135e29decp-5,  0x1.338e8983f0649p-5,  0x1.3e70564c063c4p-5,
    0x1.498acd89a2d59p-5,  0x1.54de4257938cep-5,  0x1.606b071c66585p-5,  0x1.6c316d8e6dd5fp-5,
    0x1.7831c6b7a0a45p-5,  0x1.846c62f955ceep-5,  0x1.90e1920fdffcfp-5,  0x1.9d91a31608f92p-5,
    0x1.aa7ce4886f088p-5,  0x1.b7a3a448c57c0p-5,  0x1.c5062fa0f9c9dp-5,  0x1.d2a4d3463e6bcp-5,
    0x1.e07fdb5bfcb10p-5,  0x1.ee979376ae979p-5,  0x1.fcec469ea1beap-5,  0x1.05bf1fa952341p-4,
    0x1.0d26e3c54ebeap-4,  0x1.14ad945d08395p-4,  0x1.1c5355e946f20p-4,  0x1.24184ca308d88p-4,
    0x1.2bfc9c84a7aeep-4,  0x1.3400694af6b4bp-4,  0x1.3c23d67658243p-4,  0x1.4467074bcad41p-4,
    0x1.4cca1ed5f04cbp-4,  0x1.554d3fe60b982p-4,  0x1.5df08d14f9172p-4,  0x1.66b428c41f9a3p-4,
    0x1.6f98351e5b03dp-4,  0x1.789cd418e0ac3p-4,  0x1.81c227741dc31p-4,  0x1.8b0850bc8fe6cp-4,
    0x1.946f714b98254p-4,  0x1.9df7aa4848999p-4,  0x1.a7a11ca82cd66p-4,  0x1.b16be9300d4bap-4,
    0x1.bb583074add2fp-4,  0x1.c56612db878e0p-4,  0x1.cf95b09b7e3e9p-4,  0x1.d9e729bd913dep-4,
    0x1.e45a9e1d883c9p-4,  0x1.eef02d6a9be77p-4,  0x1.f9a7f7281a9adp-4,  0x1.02410d57049f7p-3,
    0x1.07bf5b94e038ap-3,  0x1.0d4ef5cf430b2p-3,  0x1.12efeb7311b7ap-3,  0x1.18a24bd8bbe9ep-3,
    0x1.1e66264484130p-3,  0x1.243b89e6c58e7p-3,  0x1.2a2285dc393e7p-3,  0x1.301b292e38aa5p-3,
    0x1.362582d2ffac5p-3,  0x1.3c41a1adecb79p-3,  0x1.426f948fbfc2ap-3,  0x1.48af6a36d7de5p-3,
    0x1.4f01314f6f860p-3,  0x1.5564f873d7af8p-3,  0x1.5bdace2cb1a52p-3,  0x1.6262c0f127b38p-3,
    0x1.68fcdf2724b0dp-3,  0x1.6fa937238a690p-3,  0x1.7667d72a66f40p-3,  0x1.7d38cd6f28febp-3,
    0x1.841c2814d30efp-3,  0x1.8b11f52e2dc70p-3,  0x1.921a42bdf9328p-3,  0x1.99351eb71d1fap-3,
    0x1.a06296fcd88fep-3,  0x1.a7a2b962f040bp-3,  0x1.aef593addc584p-3,  0x1.b65b3392f5354p-3,
    0x1.bdd3a6b89f6dap-3,  0x1.c55efab676fe7p-3,  0x1.ccfd3d1579b04p-3,  0x1.d4ae7b5030badp-3,
    0x1.dc72c2d2d9a61p-3,  0x1.e44a20fb8e727p-3,  0x1.ec34a31a6d0bap-3,  0x1.f4325671be06cp-3,
    0x1.fc4348361ab77p-3,  0x1.0233c2c7494c2p-2,  0x1.064f8dca68079p-2,  0x1.0a750baa9e48fp-2,
    0x1.0ea442e792158p-2,  0x1.12dd39fa6c334p-2,  0x1.171ff755e9548p-2,  0x1.1b6c81666af88p-2,
    0x1.1fc2de920806bp-2,  0x1.242315389d222p-2,  0x1.288d2bb3dcb8fp-2,  0x1.2d0128575ed21p-2,
    0x1.317f1170b096fp-2,  0x1.3606ed4763a0bp-2,  0x1.3a98c21d1d042p-2,  0x1.3f34962da4214p-2,
    0x1.43da6faef137fp-2,  0x1.488a54d13bc08p-2,  0x1.4d444bbf088cap-2,  0x1.52085a9d37af8p-2,
    0x1.56d6878b122dap-2,  0x1.5baed8a2577aep-2,  0x1.609153f74abf7p-2,  0x1.657dff98bfed0p-2,
    0x1.6a74e190289f5p-2,  0x1.6f75ffe1a0cbdp-2,  0x1.7481608bfb427p-2,  0x1.79970988cdfcep-2,
    0x1.7eb700cc7e40ep-2,  0x1.83e14c464c95ap-2,  0x1.8915f1e0608a5p-2,  0x1.8e54f77fd4546p-2,
    0x1.939e6304c03f7p-2,  0x1.98f23a4a45f6dp-2,  0x1.9e5083269ba34p-2,  0x1.a3b9436b16e19p-2,
    0x1.a92c80e43791bp-2,  0x1.aeaa4159b27fbp-2,  0x1.b4328a8e7be4cp-2,  0x1.b9c56240d1c5dp-2,
    0x1.bf62ce2a462aap-2,  0x1.c50ad3ffc933bp-2,  0x1.cabd7971b30b6p-2,  0x1.d07ac42bcdb4ap-2,
    0x1.d642b9d55eb89p-2,  0x1.dc15601130b24p-2,  0x1.e1f2bc7d9cba7p-2,  0x1.e7dad4b493b2ap-2,
    0x1.edcdae4ba7709p-2,  0x1.f3cb4ed413cc2p-2,  0x1.f9d3bbdac78d3p-2,  0x1.ffe6fae86d3d9p-2,
    0x1.030288c0b9edep-1,  0x1.061702930bb95p-1,  0x1.0930eda934ca4p-1,  0x1.0c504cbf2cdcbp-1,
    0x1.0f75228edec23p-1,  0x1.129f71d02c75dp-1,  0x1.15cf3d38f323ep-1,  0x1.1904877d0f24ep-1,
    0x1.1c3f534e5fea6p-1,  0x1.1f7fa35ccbe1dp-1,  0x1.22c57a564448dp-1,  0x1.2610dae6c8f66p-1,
    0x1.2961c7b86c18ap-1,  0x1.2cb8437355e5cp-1,  0x1.301450bdc8433p-1,  0x1.3375f23c225fep-1,
    0x1.36dd2a90e443ep-1,  0x1.3a49fc5cb2567p-1,  0x1.3dbc6a3e58d7bp-1,  0x1.413476d2cf4ffp-1,
    0x1.44b224b53bf63p-1,  0x1.4835767ef70a3p-1,  0x1.4bbe6ec78e26bp-1,  0x1.4f4d1024c7885p-1,
    0x1.52e15d2aa54a8p-1,  0x1.567b586b689d0p-1,  0x1.5a1b047794ed6p-1,  0x1.5dc063ddf3091p-1,
    0x1.616b792b94359p-1,  0x1.651c46ebd53f5p-1,  0x1.68d2cfa861812p-1,  0x1.6c8f15e935e12p-1,
    0x1.70511c34a3c68p-1,  0x1.7418e50f5406ap-1,  0x1.77e672fc49c8ep-1,  0x1.7bb9c87ce563fp-1,
    0x1.7f92e810e7317p-1,  0x1.8371d436725acp-1,  0x1.87568f6a0f9ddp-1,  0x1.8b411c26b009bp-1,
    0x1.8f317ce5afb41p-1,  0x1.9327b41ed8677p-1,  0x1.9723c44864499p-1,  0x1.9b25afd7007b1p-1,
    0x1.9f2d793dcfaf0p-1,  0x1.a33b22ee6cbcfp-1,  0x1.a74eaf58ed2a6p-1,  0x1.ab6820ebe3af4p-1,
    0x1.af877a1462b19p-1,  0x1.b3acbd3dfebc1p-1,  0x1.b7d7ecd2d0ee1p-1,  0x1.bc090b3b79646p-1,
    0x1.c0401adf219b9p-1,  0x1.c47d1e237ecd4p-1,  0x1.c8c0176cd4465p-1,  0x1.cd09091df5b72p-1,
    0x1.d157f598497d5p-1,  0x1.d5acdf3bcae8cp-1,  0x1.da07c8670c7a9p-1,  0x1.de68b3773a1c8p-1,
    0x1.e2cfa2c81b55ap-1,  0x1.e73c98b41576fp-1,  0x1.ebaf97942dc34p-1,  0x1.f028a1c00b92bp-1,
    0x1.f4a7b98dfa6ecp-1,  0x1.f92ce152ec2b6p-1,  0x1.fdb81b627af91p-1,
};

static_assert(std::is_sorted(kSrgbHalfCodeLinearThresholds.begin(),
                             kSrgbHalfCodeLinearThresholds.end()));

[[nodiscard]] ImageError codeError(const ImageErrorCode code) noexcept {
    return ImageError::codeOnly(code);
}

[[nodiscard]] bool supportedEnvironment() noexcept {
    return bloom::core::supportsReferenceFloatingPointEnvironment<float>() &&
           bloom::core::supportsReferenceFloatingPointEnvironment<double>();
}

[[nodiscard]] std::optional<float> checkedFloat(const double value) noexcept {
    constexpr auto maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -maximum || value > maximum) {
        return std::nullopt;
    }
    return static_cast<float>(value);
}

[[nodiscard]] RawPixel rawPixel(const Rgba32f pixel) noexcept {
    return {pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
}

[[nodiscard]] RawPixel transparentRawPixel() noexcept { return {0.0F, 0.0F, 0.0F, 0.0F}; }

[[nodiscard]] float weightedInterpolate(const float start, const float end,
                                        const float factor) noexcept {
    if (factor == 0.0F) {
        return start;
    }
    if (factor == 1.0F) {
        return end;
    }
    const auto startContribution = (1.0F - factor) * start;
    return std::fma(factor, end, startContribution);
}

[[nodiscard]] RawPixel interpolatePixels(const RawPixel& start, const RawPixel& end,
                                         const float factor) noexcept {
    RawPixel result{};
    for (std::size_t component = 0; component < result.size(); ++component) {
        result[component] = weightedInterpolate(start[component], end[component], factor);
    }
    return result;
}

[[nodiscard]] ImageResult<Rgba32f> checkedProcessPixel(const RawPixel& components) noexcept {
    const auto pixel =
        Rgba32f::fromPremultiplied(components[0], components[1], components[2], components[3]);
    if (!pixel) {
        return ImageResult<Rgba32f>::failure(codeError(ImageErrorCode::NonFiniteResult));
    }
    return pixel;
}

[[nodiscard]] RawPixel sampleLocal(const std::span<const Rgba32f> pixels, const std::uint32_t width,
                                   const std::uint32_t height, const std::int64_t x,
                                   const std::int64_t y) noexcept {
    if (x < 0 || y < 0 || x >= static_cast<std::int64_t>(width) ||
        y >= static_cast<std::int64_t>(height)) {
        return transparentRawPixel();
    }
    const auto offset =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
    return rawPixel(pixels[offset]);
}

[[nodiscard]] bool spansOverlap(const std::span<const Rgba32f> source,
                                const std::span<Rgba32f> output) noexcept {
    if (source.empty() || output.empty()) {
        return false;
    }
    const auto* const sourceBegin = source.data();
    const auto* const sourceEnd = source.data() + source.size();
    const auto* const outputBegin = output.data();
    const auto* const outputEnd = output.data() + output.size();
    const std::less<const Rgba32f*> before;
    return before(sourceBegin, outputEnd) && before(outputBegin, sourceEnd);
}

[[nodiscard]] std::uint8_t referenceSrgbByte(const double linear) noexcept {
    const auto* const transition = std::upper_bound(kSrgbHalfCodeLinearThresholds.begin(),
                                                    kSrgbHalfCodeLinearThresholds.end(), linear);
    return static_cast<std::uint8_t>(
        std::distance(kSrgbHalfCodeLinearThresholds.begin(), transition));
}

[[nodiscard]] std::uint8_t alphaByte(const float alpha) noexcept {
    const auto quantized =
        static_cast<unsigned int>(std::lround(static_cast<double>(alpha) * 255.0));
    return static_cast<std::uint8_t>(quantized);
}

// Exact cosine/sine of a rotation in DEGREES. A rotation that is an exact quarter turn is resolved
// to exact 0 and +/-1 rather than std::cos/std::sin of a rounded radian value, so a 90, 180, or 270
// degree layer maps pixel centres onto pixel centres and the bilinear resample interpolates nothing
// at all. std::fmod is exact, and the wrap keeps an authored -90 or 450 as exact as a 270 or 90.
struct RotationCosSin final {
    double cosine = 1.0;
    double sine = 0.0;
};

[[nodiscard]] RotationCosSin rotationCosSin(const double degrees) noexcept {
    const auto wrapped = std::fmod(degrees, 360.0);
    const auto turns = wrapped < 0.0 ? wrapped + 360.0 : wrapped;
    if (turns == 0.0) {
        return {1.0, 0.0};
    }
    if (turns == 90.0) {
        return {0.0, 1.0};
    }
    if (turns == 180.0) {
        return {-1.0, 0.0};
    }
    if (turns == 270.0) {
        return {0.0, -1.0};
    }
    const auto radians = turns * (std::numbers::pi / 180.0);
    return {std::cos(radians), std::sin(radians)};
}

// Inclusive integer span of a real interval, clamped into [low, high]. Doubles do the clamping
// because the real interval can be astronomically wide (a large scale factor) while `low` and
// `high` are always small enough to be exact as doubles, so no int64 conversion can overflow.
struct ClampedSpan final {
    std::int64_t low = 0;
    std::int64_t high = -1;

    [[nodiscard]] constexpr bool empty() const noexcept { return high < low; }
    [[nodiscard]] constexpr std::uint64_t extent() const noexcept {
        return empty() ? 0 : static_cast<std::uint64_t>(high - low) + 1;
    }
};

[[nodiscard]] ClampedSpan clampedSpan(const double minimum, const double maximum,
                                      const std::int64_t low, const std::int64_t high) noexcept {
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum) {
        return {};
    }
    const auto lowBound = static_cast<double>(low);
    const auto highBound = static_cast<double>(high);
    const auto first = std::max(std::floor(minimum), lowBound);
    const auto last = std::min(std::ceil(maximum), highBound);
    if (first > last) {
        return {};
    }
    return {static_cast<std::int64_t>(first), static_cast<std::int64_t>(last)};
}

// One separable blend function B(Cb, Cs), on UN-premultiplied channel values, in the W3C
// Compositing and Blending Level 1 sense. `backdrop` is Cb and `source` is Cs.
//
// The function is TOTAL over the mode vocabulary, Normal and Add included, because those two really
// are separable blend functions (Cs, and Cb + Cs) -- blendLinearRec709SceneRow() below reaches them
// through exact shortcuts instead, but the shortcuts are specializations of this same algebra, not
// a different rule. See docs/architecture/color-management.md, "Blend modes", for each formula and
// for what the unit references in Screen and Overlay mean in a scene-referred space.
//
// Nothing is clamped. Screen's and Overlay's `1` is the reference white of lin_rec709_scene, not a
// ceiling, so an HDR or negative channel extrapolates the formula rather than being clipped; the
// process contract forbids clamping before the display boundary.
[[nodiscard]] double separableBlend(const bloom::core::BlendMode mode, const double backdrop,
                                    const double source) noexcept {
    switch (mode) {
    case bloom::core::BlendMode::Normal:
        return source;
    case bloom::core::BlendMode::Add:
        return backdrop + source;
    case bloom::core::BlendMode::Multiply:
        return backdrop * source;
    case bloom::core::BlendMode::Screen:
        return backdrop + source - backdrop * source;
    case bloom::core::BlendMode::Overlay:
        // Hard Light with the operands exchanged, spelled out rather than composed, so the pivot
        // test reads on the BACKDROP -- which is what makes Overlay "the backdrop decides" and Hard
        // Light "the source decides".
        return backdrop <= 0.5 ? 2.0 * backdrop * source
                               : 1.0 - 2.0 * (1.0 - backdrop) * (1.0 - source);
    case bloom::core::BlendMode::Darken:
        return std::min(backdrop, source);
    case bloom::core::BlendMode::Lighten:
        return std::max(backdrop, source);
    case bloom::core::BlendMode::Difference:
        return std::abs(backdrop - source);
    }
    return source;
}

[[nodiscard]] std::uint8_t displayChannelByte(const float premultiplied,
                                              const float alpha) noexcept {
    if (premultiplied <= 0.0F) {
        return 0;
    }
    if (premultiplied >= alpha) {
        return 255;
    }
    return referenceSrgbByte(static_cast<double>(premultiplied) / static_cast<double>(alpha));
}

} // namespace

namespace bloom::render {

ImageResult<TranslationOpacity> TranslationOpacity::create(const double translationX,
                                                           const double translationY,
                                                           const double opacity) noexcept {
    if (!std::isfinite(translationX) || !std::isfinite(translationY) || !std::isfinite(opacity)) {
        return ImageResult<TranslationOpacity>::failure(
            codeError(ImageErrorCode::InvalidParameter));
    }
    if (!supportedEnvironment()) {
        return ImageResult<TranslationOpacity>::failure(
            codeError(ImageErrorCode::UnsupportedFloatingPointEnvironment));
    }
    if (opacity < 0.0 || opacity > 1.0) {
        return ImageResult<TranslationOpacity>::failure(
            codeError(ImageErrorCode::InvalidParameter));
    }
    return ImageResult<TranslationOpacity>::success(
        TranslationOpacity(translationX, translationY, static_cast<float>(opacity)));
}

ImageResult<Rgba32f> solidPixelFromStraightLinearRec709Scene(const core::Color4d color) noexcept {
    if (!std::isfinite(color.red) || !std::isfinite(color.green) || !std::isfinite(color.blue) ||
        !std::isfinite(color.alpha)) {
        return ImageResult<Rgba32f>::failure(codeError(ImageErrorCode::InvalidParameter));
    }
    if (!supportedEnvironment()) {
        return ImageResult<Rgba32f>::failure(
            codeError(ImageErrorCode::UnsupportedFloatingPointEnvironment));
    }
    if (color.alpha < 0.0 || color.alpha > 1.0) {
        return ImageResult<Rgba32f>::failure(codeError(ImageErrorCode::InvalidParameter));
    }

    const std::array premultiplied{color.red * color.alpha, color.green * color.alpha,
                                   color.blue * color.alpha};
    const auto alpha = checkedFloat(color.alpha);
    if (!alpha.has_value()) {
        return ImageResult<Rgba32f>::failure(codeError(ImageErrorCode::NonFiniteResult));
    }
    if (*alpha == 0.0F) {
        return ImageResult<Rgba32f>::success(Rgba32f::transparent());
    }
    const auto red = checkedFloat(premultiplied[0]);
    const auto green = checkedFloat(premultiplied[1]);
    const auto blue = checkedFloat(premultiplied[2]);
    if (!red.has_value() || !green.has_value() || !blue.has_value()) {
        return ImageResult<Rgba32f>::failure(codeError(ImageErrorCode::NonFiniteResult));
    }
    const auto pixel = Rgba32f::fromPremultiplied(*red, *green, *blue, *alpha);
    return pixel ? pixel
                 : ImageResult<Rgba32f>::failure(codeError(ImageErrorCode::NonFiniteResult));
}

void fillSolidRow(const std::span<Rgba32f> output, const Rgba32f pixel) noexcept {
    std::ranges::fill(output, pixel);
}

ImageStatus translateOpacityBilinearRow(const Rgba32fImageView source,
                                        const ImageWindow outputWindow, const std::int64_t outputY,
                                        const TranslationOpacity parameters,
                                        const std::span<Rgba32f> output) noexcept {
    const auto sourceDescriptorValue = source.descriptor();
    if (!sourceDescriptorValue.has_value() ||
        source.pixels().size() != sourceDescriptorValue->layout().pixelCount) {
        return codeError(ImageErrorCode::InvalidState);
    }
    if (outputY < outputWindow.originY() || outputY >= outputWindow.maxYExclusive()) {
        return codeError(ImageErrorCode::CoordinateOutOfBounds);
    }
    const auto expectedBytes =
        static_cast<std::size_t>(outputWindow.extent().width()) * sizeof(Rgba32f);
    if (output.size_bytes() != expectedBytes) {
        return ImageError::storageSizeMismatch(output.size_bytes(), expectedBytes);
    }
    if (spansOverlap(source.pixels(), output)) {
        return codeError(ImageErrorCode::InvalidParameter);
    }
    if (!supportedEnvironment()) {
        return codeError(ImageErrorCode::UnsupportedFloatingPointEnvironment);
    }
    if (parameters.opacity() == 0.0F) {
        fillSolidRow(output, Rgba32f::transparent());
        return std::nullopt;
    }

    const auto sourceDescriptor = *sourceDescriptorValue;
    const auto sourceWidth = sourceDescriptor.dataWindow().extent().width();
    const auto sourceHeight = sourceDescriptor.dataWindow().extent().height();
    const auto outputLocalY = static_cast<std::uint64_t>(outputY - outputWindow.originY());
    const auto sourceY = static_cast<double>(outputLocalY) - parameters.translationY();
    if (sourceY <= -1.0 || sourceY >= static_cast<double>(sourceHeight)) {
        fillSolidRow(output, Rgba32f::transparent());
        return std::nullopt;
    }
    const auto baseY = static_cast<std::int64_t>(std::floor(sourceY));
    const auto factorY = static_cast<float>(sourceY - static_cast<double>(baseY));

    for (std::size_t outputX = 0; outputX < output.size(); ++outputX) {
        const auto sourceX = static_cast<double>(outputX) - parameters.translationX();
        if (sourceX <= -1.0 || sourceX >= static_cast<double>(sourceWidth)) {
            output[outputX] = Rgba32f::transparent();
            continue;
        }
        const auto baseX = static_cast<std::int64_t>(std::floor(sourceX));
        const auto factorX = static_cast<float>(sourceX - static_cast<double>(baseX));
        const auto top = interpolatePixels(
            sampleLocal(source.pixels(), sourceWidth, sourceHeight, baseX, baseY),
            sampleLocal(source.pixels(), sourceWidth, sourceHeight, baseX + 1, baseY), factorX);
        const auto bottom = interpolatePixels(
            sampleLocal(source.pixels(), sourceWidth, sourceHeight, baseX, baseY + 1),
            sampleLocal(source.pixels(), sourceWidth, sourceHeight, baseX + 1, baseY + 1), factorX);
        auto sampled = interpolatePixels(top, bottom, factorY);
        if (parameters.opacity() != 1.0F) {
            for (auto& component : sampled) {
                component *= parameters.opacity();
            }
        }
        const auto pixel = checkedProcessPixel(sampled);
        if (!pixel) {
            return *pixel.error();
        }
        output[outputX] = *pixel.value();
    }
    return std::nullopt;
}

ImageResult<LayerTransform> LayerTransform::create(const Authored authored,
                                                   const ImageWindow sourceWindow,
                                                   const double proxyScaleX,
                                                   const double proxyScaleY) noexcept {
    const std::array authoredValues{
        authored.translationX, authored.translationY, authored.anchorX,         authored.anchorY,
        authored.scaleX,       authored.scaleY,       authored.rotationDegrees, authored.opacity};
    if (std::ranges::any_of(authoredValues,
                            [](const double value) { return !std::isfinite(value); }) ||
        !std::isfinite(proxyScaleX) || !std::isfinite(proxyScaleY)) {
        return ImageResult<LayerTransform>::failure(codeError(ImageErrorCode::InvalidParameter));
    }
    if (!supportedEnvironment()) {
        return ImageResult<LayerTransform>::failure(
            codeError(ImageErrorCode::UnsupportedFloatingPointEnvironment));
    }
    if (authored.opacity < 0.0 || authored.opacity > 1.0 || proxyScaleX <= 0.0 ||
        proxyScaleY <= 0.0) {
        return ImageResult<LayerTransform>::failure(codeError(ImageErrorCode::InvalidParameter));
    }
    // A zero scale factor collapses the layer onto a line or a point: there is no inverse to map an
    // output pixel back through, and nothing with area to resample. The Layer Output stage treats
    // that as an empty layer before it ever gets here, so reaching this is a caller error.
    if (authored.scaleX == 0.0 || authored.scaleY == 0.0) {
        return ImageResult<LayerTransform>::failure(codeError(ImageErrorCode::InvalidParameter));
    }

    State state{.sourceWindow = sourceWindow};
    state.opacity = static_cast<float>(authored.opacity);
    // Exactly the pre-S4 expression, so a translate-only layer's device translation is the same
    // double the version-3 Layer Output stage computed.
    state.deviceTranslationX = authored.translationX * proxyScaleX;
    state.deviceTranslationY = authored.translationY * proxyScaleY;

    const auto rotation = rotationCosSin(authored.rotationDegrees);
    state.translationOnly =
        authored.scaleX == 1.0 && authored.scaleY == 1.0 && rotation.cosine == 1.0;
    if (state.translationOnly) {
        // Deliberately leave the linear maps at their identity defaults and the anchor out of the
        // resolved state entirely: inverseMap() must not compute with the anchor on this path,
        // because adding and subtracting it would perturb the last bit of a subpixel translation
        // that version 3 produced exactly.
        return ImageResult<LayerTransform>::success(LayerTransform(state));
    }

    // Forward linear map in full-resolution space, M = R(rotation) * S(scale), and its inverse
    // S^-1 * R(-rotation). Both are written out rather than inverted numerically: the closed form
    // keeps a quarter turn exact, which a general 2x2 inversion would not.
    const auto fullForwardA = rotation.cosine * authored.scaleX;
    const auto fullForwardB = -rotation.sine * authored.scaleY;
    const auto fullForwardC = rotation.sine * authored.scaleX;
    const auto fullForwardD = rotation.cosine * authored.scaleY;
    const auto fullInverseA = rotation.cosine / authored.scaleX;
    const auto fullInverseB = rotation.sine / authored.scaleX;
    const auto fullInverseC = -rotation.sine / authored.scaleY;
    const auto fullInverseD = rotation.cosine / authored.scaleY;

    // Conjugation by the per-axis proxy factor: the off-diagonal terms carry the axis ratio, the
    // diagonal ones are unitless and carry nothing. With equal factors every ratio is exactly 1 and
    // the device maps are the full-resolution maps unchanged.
    const auto ratio = proxyScaleX / proxyScaleY;
    const auto inverseRatio = proxyScaleY / proxyScaleX;
    state.inverseA = fullInverseA;
    state.inverseB = fullInverseB * ratio;
    state.inverseC = fullInverseC * inverseRatio;
    state.inverseD = fullInverseD;
    state.forwardA = fullForwardA;
    state.forwardB = fullForwardB * ratio;
    state.forwardC = fullForwardC * inverseRatio;
    state.forwardD = fullForwardD;
    if (!std::isfinite(state.inverseA) || !std::isfinite(state.inverseB) ||
        !std::isfinite(state.inverseC) || !std::isfinite(state.inverseD) ||
        !std::isfinite(state.forwardA) || !std::isfinite(state.forwardB) ||
        !std::isfinite(state.forwardC) || !std::isfinite(state.forwardD)) {
        return ImageResult<LayerTransform>::failure(codeError(ImageErrorCode::NonFiniteResult));
    }

    // The layer centre is the centre of its own pixel AREA, and the anchor is measured from there.
    // Pixel centres have integer local coordinates, so a w-wide image occupies [-0.5, w - 0.5] and
    // its centre sits at (w - 1) / 2 -- not at w / 2. That half-pixel matters: a quarter turn about
    // (w - 1) / 2 maps pixel centres exactly onto pixel centres, while a turn about w / 2 would
    // shift the layer half a pixel and blur every pixel of an otherwise exact rotation.
    //
    // The position parameter measures from w / 2 instead, because a position of w / 2 is what means
    // "unmoved" in composition coordinates. The two are not in conflict: position supplies a
    // DISPLACEMENT, which is origin-independent, while the anchor names a POINT, which is not.
    const auto centreLocalX = (static_cast<double>(sourceWindow.extent().width()) - 1.0) / 2.0;
    const auto centreLocalY = (static_cast<double>(sourceWindow.extent().height()) - 1.0) / 2.0;
    state.anchorLocalX = centreLocalX + authored.anchorX * proxyScaleX;
    state.anchorLocalY = centreLocalY + authored.anchorY * proxyScaleY;
    state.pivotOutputX =
        static_cast<double>(sourceWindow.originX()) + state.anchorLocalX + state.deviceTranslationX;
    state.pivotOutputY =
        static_cast<double>(sourceWindow.originY()) + state.anchorLocalY + state.deviceTranslationY;
    if (!std::isfinite(state.anchorLocalX) || !std::isfinite(state.anchorLocalY) ||
        !std::isfinite(state.pivotOutputX) || !std::isfinite(state.pivotOutputY)) {
        return ImageResult<LayerTransform>::failure(codeError(ImageErrorCode::NonFiniteResult));
    }
    return ImageResult<LayerTransform>::success(LayerTransform(state));
}

LayerTransform::SamplePoint LayerTransform::inverseMap(const double outputX,
                                                       const double outputY) const noexcept {
    if (state_.translationOnly) {
        // The pre-S4 arithmetic: the output pixel's source-local column minus the translation, in
        // that order and with no other term.
        return {(outputX - static_cast<double>(state_.sourceWindow.originX())) -
                    state_.deviceTranslationX,
                (outputY - static_cast<double>(state_.sourceWindow.originY())) -
                    state_.deviceTranslationY};
    }
    const auto offsetX = outputX - state_.pivotOutputX;
    const auto offsetY = outputY - state_.pivotOutputY;
    return {state_.anchorLocalX + (state_.inverseA * offsetX + state_.inverseB * offsetY),
            state_.anchorLocalY + (state_.inverseC * offsetX + state_.inverseD * offsetY)};
}

std::optional<ImageWindow> LayerTransform::supportBounds(const ImageWindow clip) const noexcept {
    // Bilinear support, in source-local coordinates: a tap is fetched whenever the sample
    // coordinate is strictly inside (-1, extent), so the closed box [-1, extent] bounds every
    // output pixel the resample can write a non-transparent value to.
    const auto lowX = -1.0;
    const auto lowY = -1.0;
    const auto highX = static_cast<double>(state_.sourceWindow.extent().width());
    const auto highY = static_cast<double>(state_.sourceWindow.extent().height());

    auto forward = [this](const double localX, const double localY) noexcept {
        if (state_.translationOnly) {
            return SamplePoint{static_cast<double>(state_.sourceWindow.originX()) + localX +
                                   state_.deviceTranslationX,
                               static_cast<double>(state_.sourceWindow.originY()) + localY +
                                   state_.deviceTranslationY};
        }
        const auto offsetX = localX - state_.anchorLocalX;
        const auto offsetY = localY - state_.anchorLocalY;
        return SamplePoint{
            state_.pivotOutputX + (state_.forwardA * offsetX + state_.forwardB * offsetY),
            state_.pivotOutputY + (state_.forwardC * offsetX + state_.forwardD * offsetY)};
    };

    const std::array corners{forward(lowX, lowY), forward(highX, lowY), forward(lowX, highY),
                             forward(highX, highY)};
    auto minimumX = corners.front().x;
    auto maximumX = corners.front().x;
    auto minimumY = corners.front().y;
    auto maximumY = corners.front().y;
    for (const auto corner : corners) {
        minimumX = std::min(minimumX, corner.x);
        maximumX = std::max(maximumX, corner.x);
        minimumY = std::min(minimumY, corner.y);
        maximumY = std::max(maximumY, corner.y);
    }

    const auto columns = clampedSpan(minimumX, maximumX, clip.originX(), clip.maxXExclusive() - 1);
    const auto rows = clampedSpan(minimumY, maximumY, clip.originY(), clip.maxYExclusive() - 1);
    if (columns.empty() || rows.empty()) {
        return std::nullopt;
    }
    const auto window = ImageWindow::create(columns.low, rows.low, columns.extent(), rows.extent());
    if (!window) {
        return std::nullopt;
    }
    return *window.value();
}

ImageStatus layerTransformBilinearRow(const Rgba32fImageView source, const ImageWindow outputWindow,
                                      const std::int64_t outputY, const LayerTransform& transform,
                                      const std::span<Rgba32f> output) noexcept {
    const auto sourceDescriptorValue = source.descriptor();
    if (!sourceDescriptorValue.has_value() ||
        source.pixels().size() != sourceDescriptorValue->layout().pixelCount) {
        return codeError(ImageErrorCode::InvalidState);
    }
    if (sourceDescriptorValue->dataWindow() != transform.sourceWindow()) {
        return codeError(ImageErrorCode::IncompatibleImageDescriptor);
    }
    if (outputY < outputWindow.originY() || outputY >= outputWindow.maxYExclusive()) {
        return codeError(ImageErrorCode::CoordinateOutOfBounds);
    }
    const auto expectedBytes =
        static_cast<std::size_t>(outputWindow.extent().width()) * sizeof(Rgba32f);
    if (output.size_bytes() != expectedBytes) {
        return ImageError::storageSizeMismatch(output.size_bytes(), expectedBytes);
    }
    if (spansOverlap(source.pixels(), output)) {
        return codeError(ImageErrorCode::InvalidParameter);
    }
    if (!supportedEnvironment()) {
        return codeError(ImageErrorCode::UnsupportedFloatingPointEnvironment);
    }
    if (transform.opacity() == 0.0F) {
        fillSolidRow(output, Rgba32f::transparent());
        return std::nullopt;
    }

    const auto sourceWidth = transform.sourceWindow().extent().width();
    const auto sourceHeight = transform.sourceWindow().extent().height();
    const auto rowY = static_cast<double>(outputY);
    for (std::size_t outputX = 0; outputX < output.size(); ++outputX) {
        const auto sample = transform.inverseMap(
            static_cast<double>(outputWindow.originX() + static_cast<std::int64_t>(outputX)), rowY);
        if (sample.x <= -1.0 || sample.x >= static_cast<double>(sourceWidth) || sample.y <= -1.0 ||
            sample.y >= static_cast<double>(sourceHeight)) {
            output[outputX] = Rgba32f::transparent();
            continue;
        }
        const auto baseX = static_cast<std::int64_t>(std::floor(sample.x));
        const auto factorX = static_cast<float>(sample.x - static_cast<double>(baseX));
        const auto baseY = static_cast<std::int64_t>(std::floor(sample.y));
        const auto factorY = static_cast<float>(sample.y - static_cast<double>(baseY));
        const auto top = interpolatePixels(
            sampleLocal(source.pixels(), sourceWidth, sourceHeight, baseX, baseY),
            sampleLocal(source.pixels(), sourceWidth, sourceHeight, baseX + 1, baseY), factorX);
        const auto bottom = interpolatePixels(
            sampleLocal(source.pixels(), sourceWidth, sourceHeight, baseX, baseY + 1),
            sampleLocal(source.pixels(), sourceWidth, sourceHeight, baseX + 1, baseY + 1), factorX);
        auto sampled = interpolatePixels(top, bottom, factorY);
        if (transform.opacity() != 1.0F) {
            for (auto& component : sampled) {
                component *= transform.opacity();
            }
        }
        const auto pixel = checkedProcessPixel(sampled);
        if (!pixel) {
            return *pixel.error();
        }
        output[outputX] = *pixel.value();
    }
    return std::nullopt;
}

ImageStatus sourceOverLinearRec709SceneRow(const std::span<const Rgba32f> source,
                                           const std::span<Rgba32f> destination) noexcept {
    if (source.size() != destination.size()) {
        return ImageError::storageSizeMismatch(source.size_bytes(), destination.size_bytes());
    }
    if (spansOverlap(source, destination)) {
        return codeError(ImageErrorCode::InvalidParameter);
    }
    if (!supportedEnvironment()) {
        return codeError(ImageErrorCode::UnsupportedFloatingPointEnvironment);
    }

    for (std::size_t index = 0; index < source.size(); ++index) {
        const auto sourcePixel = source[index];
        if (sourcePixel.alpha() == 0.0F) {
            continue;
        }
        const auto destinationPixel = destination[index];
        if (sourcePixel.alpha() == 1.0F || destinationPixel.alpha() == 0.0F) {
            destination[index] = sourcePixel;
            continue;
        }
        const auto inverseSourceAlpha = 1.0F - sourcePixel.alpha();
        const RawPixel composited{
            std::fma(inverseSourceAlpha, destinationPixel.red(), sourcePixel.red()),
            std::fma(inverseSourceAlpha, destinationPixel.green(), sourcePixel.green()),
            std::fma(inverseSourceAlpha, destinationPixel.blue(), sourcePixel.blue()),
            std::fma(inverseSourceAlpha, destinationPixel.alpha(), sourcePixel.alpha()),
        };
        const auto pixel = checkedProcessPixel(composited);
        if (!pixel) {
            return *pixel.error();
        }
        destination[index] = *pixel.value();
    }
    return std::nullopt;
}

ImageStatus blendLinearRec709SceneRow(const core::BlendMode mode,
                                      const std::span<const Rgba32f> source,
                                      const std::span<Rgba32f> destination) noexcept {
    // Normal is the retained kernel itself, not a re-derivation of it: every frame published before
    // blend modes existed came out of that exact code, and delegating is what keeps a Normal layer
    // bit-identical rather than merely equal in algebra.
    if (mode == core::BlendMode::Normal) {
        return sourceOverLinearRec709SceneRow(source, destination);
    }
    if (source.size() != destination.size()) {
        return ImageError::storageSizeMismatch(source.size_bytes(), destination.size_bytes());
    }
    if (spansOverlap(source, destination)) {
        return codeError(ImageErrorCode::InvalidParameter);
    }
    if (!supportedEnvironment()) {
        return codeError(ImageErrorCode::UnsupportedFloatingPointEnvironment);
    }

    for (std::size_t index = 0; index < source.size(); ++index) {
        const auto sourcePixel = source[index];
        if (sourcePixel.alpha() == 0.0F) {
            continue;
        }
        const auto destinationPixel = destination[index];
        // Nothing underneath: the general fold below collapses to exactly the source pixel when the
        // backdrop alpha is zero, under every mode, so writing it through is both right and exact.
        //
        // Source-over's OTHER shortcut -- an opaque source replaces the destination -- is
        // deliberately absent. It holds only when B(Cb, Cs) is Cs; every other mode still reads the
        // backdrop's colour at full source alpha, which is the whole point of blending.
        if (destinationPixel.alpha() == 0.0F) {
            destination[index] = sourcePixel;
            continue;
        }
        // Alpha compositing is source-over for every mode, computed with the EXACT expression the
        // source-over kernel uses, so a mode changes a layer's colour and never its coverage.
        const auto inverseSourceAlpha = 1.0F - sourcePixel.alpha();
        const auto blendedAlpha =
            std::fma(inverseSourceAlpha, destinationPixel.alpha(), sourcePixel.alpha());
        const auto sourceAlpha = static_cast<double>(sourcePixel.alpha());
        const auto backdropAlpha = static_cast<double>(destinationPixel.alpha());
        const auto sourceComponents = rawPixel(sourcePixel);
        const auto backdropComponents = rawPixel(destinationPixel);
        RawPixel composited{0.0F, 0.0F, 0.0F, blendedAlpha};
        bool representable = true;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            if (mode == core::BlendMode::Add) {
                // Add needs no round trip at all: substituting B(Cb, Cs) = Cb + Cs into the general
                // fold cancels both alpha weightings and leaves premultiplied addition, co = cs +
                // cb. Doing the division anyway would only add two roundings to an exact answer.
                const auto sum = static_cast<double>(sourceComponents[channel]) +
                                 static_cast<double>(backdropComponents[channel]);
                const auto value = checkedFloat(sum);
                representable = representable && value.has_value();
                composited[channel] = value.value_or(0.0F);
                continue;
            }
            // The general W3C Compositing and Blending Level 1 fold with source-over as the
            // compositing operator, on un-premultiplied channels, producing a PREMULTIPLIED result:
            //
            //   co = as*(1 - ab)*Cs + as*ab*B(Cb, Cs) + (1 - as)*ab*Cb
            //
            // Both alphas are strictly positive here, so the two divisions are defined. Every
            // product and sum is Float64 and the result is rounded to Float32 exactly once, so the
            // three terms never accumulate Float32 error against each other.
            const auto straightSource =
                static_cast<double>(sourceComponents[channel]) / sourceAlpha;
            const auto straightBackdrop =
                static_cast<double>(backdropComponents[channel]) / backdropAlpha;
            const auto blended = separableBlend(mode, straightBackdrop, straightSource);
            const auto premultiplied = sourceAlpha * (1.0 - backdropAlpha) * straightSource +
                                       sourceAlpha * backdropAlpha * blended +
                                       (1.0 - sourceAlpha) * backdropAlpha * straightBackdrop;
            const auto value = checkedFloat(premultiplied);
            representable = representable && value.has_value();
            composited[channel] = value.value_or(0.0F);
        }
        if (!representable) {
            return codeError(ImageErrorCode::NonFiniteResult);
        }
        const auto pixel = checkedProcessPixel(composited);
        if (!pixel) {
            return *pixel.error();
        }
        destination[index] = *pixel.value();
    }
    return std::nullopt;
}

ImageStatus coverageSolidRow(const std::span<const std::uint8_t> coverage, const Rgba32f pixel,
                             const std::span<Rgba32f> output) noexcept {
    if (coverage.size() != output.size()) {
        return ImageError::storageSizeMismatch(coverage.size_bytes(), output.size_bytes());
    }
    if (!supportedEnvironment()) {
        return codeError(ImageErrorCode::UnsupportedFloatingPointEnvironment);
    }
    if (pixel.alpha() == 0.0F) {
        fillSolidRow(output, Rgba32f::transparent());
        return std::nullopt;
    }

    const auto components = rawPixel(pixel);
    for (std::size_t index = 0; index < coverage.size(); ++index) {
        const auto sample = coverage[index];
        // Exact at both ends: no coverage is exactly transparent and full coverage is exactly the
        // solid pixel, with no multiply that could round either one away.
        if (sample == 0) {
            output[index] = Rgba32f::transparent();
            continue;
        }
        if (sample == 255) {
            output[index] = pixel;
            continue;
        }
        const auto fraction = static_cast<double>(sample) / 255.0;
        RawPixel scaled{};
        for (std::size_t component = 0; component < scaled.size(); ++component) {
            const auto value = checkedFloat(static_cast<double>(components[component]) * fraction);
            if (!value.has_value()) {
                return codeError(ImageErrorCode::NonFiniteResult);
            }
            scaled[component] = *value;
        }
        const auto scaledPixel = checkedProcessPixel(scaled);
        if (!scaledPixel) {
            return *scaledPixel.error();
        }
        output[index] = *scaledPixel.value();
    }
    return std::nullopt;
}

ImageStatus mapLinearRec709SceneToSrgbRow(const Rgba32fImageView source,
                                          const ImageWindow displayWindow,
                                          const std::int64_t outputY,
                                          const std::span<Rgba8> output) noexcept {
    const auto sourceDescriptorValue = source.descriptor();
    if (!sourceDescriptorValue.has_value() ||
        source.pixels().size() != sourceDescriptorValue->layout().pixelCount) {
        return codeError(ImageErrorCode::InvalidState);
    }
    const auto sourceDescriptor = *sourceDescriptorValue;
    if (sourceDescriptor.displayWindow() != displayWindow) {
        return codeError(ImageErrorCode::IncompatibleImageDescriptor);
    }
    if (outputY < displayWindow.originY() || outputY >= displayWindow.maxYExclusive()) {
        return codeError(ImageErrorCode::CoordinateOutOfBounds);
    }
    const auto expectedBytes =
        static_cast<std::size_t>(displayWindow.extent().width()) * sizeof(Rgba8);
    if (output.size_bytes() != expectedBytes) {
        return ImageError::storageSizeMismatch(output.size_bytes(), expectedBytes);
    }
    if (!supportedEnvironment()) {
        return codeError(ImageErrorCode::UnsupportedFloatingPointEnvironment);
    }

    const auto dataWindow = sourceDescriptor.dataWindow();
    for (std::size_t outputX = 0; outputX < output.size(); ++outputX) {
        const auto x = displayWindow.originX() + static_cast<std::int64_t>(outputX);
        if (!dataWindow.contains(x, outputY)) {
            output[outputX] = Rgba8{0, 0, 0, 0};
            continue;
        }
        const auto localX = static_cast<std::size_t>(x - dataWindow.originX());
        const auto localY = static_cast<std::size_t>(outputY - dataWindow.originY());
        const auto pixel = source.pixels()[localY * dataWindow.extent().width() + localX];
        if (pixel.alpha() == 0.0F) {
            output[outputX] = Rgba8{0, 0, 0, 0};
            continue;
        }
        output[outputX] =
            Rgba8{displayChannelByte(pixel.red(), pixel.alpha()),
                  displayChannelByte(pixel.green(), pixel.alpha()),
                  displayChannelByte(pixel.blue(), pixel.alpha()), alphaByte(pixel.alpha())};
    }
    return std::nullopt;
}

} // namespace bloom::render
