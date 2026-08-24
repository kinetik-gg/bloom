#include <bloom/render/cpu_image_primitives.hpp>

#include <bloom/core/floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
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

ImageResult<Rgba32f> solidPixelFromStraightReferenceLinearSrgb(const core::Color4d color) noexcept {
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

ImageStatus sourceOverReferenceLinearSrgbRow(const std::span<const Rgba32f> source,
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

ImageStatus mapReferenceLinearSrgbToSrgbRow(const Rgba32fImageView source,
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
