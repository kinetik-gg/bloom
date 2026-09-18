#include <bloom/runtime/input_color_context.hpp>

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/media/video/colour.hpp>

#include <iostream>
#include <string_view>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using bloom::media::ImageColorSpace;
using bloom::media::ImageFormat;
using bloom::media::ImageProbe;
using bloom::media::provider::ColourTags;

[[nodiscard]] std::optional<bloom::color::ResolvedBloomNeutralConfig> neutralConfig() {
    return bloom::runtime::detail::resolveInputColorConfig(
        bloom::runtime::EvaluationColorIntent::LinearRec709Scene);
}

[[nodiscard]] std::optional<bloom::color::ResolvedBloomNeutralConfig> acesConfig() {
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!revision)
        return std::nullopt;
    const bloom::runtime::EvaluationColorIntent intent{
        .workingColorSpaceId = bloom::color::kAcesCgV1SceneLinearColorSpaceId,
        .ocioConfigRevision = *revision,
        .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri};
    return bloom::runtime::detail::resolveInputColorConfig(intent);
}

void testImageRules(Expectations& expectations) {
    auto config = neutralConfig();
    expectations.expect(config.has_value(), "Bloom Neutral resolves for image Auto rules");
    if (!config)
        return;
    const auto makeProbe = [](const ImageFormat format, const std::uint8_t bitDepth) {
        return ImageProbe{1,
                          1,
                          bitDepth,
                          {},
                          format,
                          ImageColorSpace::Auto,
                          bloom::media::ImageAlphaAssociation::Straight,
                          {},
                          {}};
    };
    for (const auto format : {ImageFormat::Png, ImageFormat::Jpeg, ImageFormat::Tiff}) {
        for (const auto depth : {std::uint8_t{8}, std::uint8_t{16}}) {
            const auto resolved = bloom::runtime::detail::resolveImageInputColorSpace(
                *config, makeProbe(format, depth), ImageColorSpace::Auto, {});
            expectations.expect(resolved.automatic &&
                                    resolved.id == config->sRgbTextureColorSpaceId(),
                                "8/16-bit PNG/JPEG/TIFF Auto resolves to config sRGB texture");
        }
    }
    const auto unsupported = bloom::runtime::detail::resolveImageInputColorSpace(
        *config, makeProbe(ImageFormat::Png, 32), ImageColorSpace::Auto, {});
    expectations.expect(unsupported.id.empty() && !unsupported.warning.empty(),
                        "unsupported image bit depth refuses automatic interpretation");
    const auto absent = bloom::runtime::detail::resolveImageInputColorSpace(
        *config,
        ImageProbe{1,
                   1,
                   32,
                   {},
                   ImageFormat::Exr,
                   ImageColorSpace::Linear,
                   bloom::media::ImageAlphaAssociation::Premultiplied,
                   "lin_rec709_scene",
                   "EXR scene-linear convention assumed; chromaticities attribute is absent"},
        ImageColorSpace::Auto, {});
    expectations.expect(absent.automatic && absent.id == config->processColorSpaceId() &&
                            !absent.warning.empty(),
                        "EXR without chromaticities assumes the working space with a warning");

    auto aces = acesConfig();
    expectations.expect(aces.has_value(), "ACES config resolves for EXR chromaticity rules");
    if (!aces)
        return;
    const auto ap0 = bloom::runtime::detail::resolveImageInputColorSpace(
        *aces,
        ImageProbe{1,
                   1,
                   32,
                   {},
                   ImageFormat::Exr,
                   ImageColorSpace::Linear,
                   bloom::media::ImageAlphaAssociation::Premultiplied,
                   "ACES2065-1",
                   {}},
        ImageColorSpace::Auto, {});
    const auto ap1 = bloom::runtime::detail::resolveImageInputColorSpace(
        *aces,
        ImageProbe{1,
                   1,
                   32,
                   {},
                   ImageFormat::Exr,
                   ImageColorSpace::Linear,
                   bloom::media::ImageAlphaAssociation::Premultiplied,
                   "ACEScg",
                   {}},
        ImageColorSpace::Auto, {});
    const auto rec709 = bloom::runtime::detail::resolveImageInputColorSpace(
        *aces,
        ImageProbe{1,
                   1,
                   32,
                   {},
                   ImageFormat::Exr,
                   ImageColorSpace::Linear,
                   bloom::media::ImageAlphaAssociation::Premultiplied,
                   "lin_rec709_scene",
                   {}},
        ImageColorSpace::Auto, {});
    const auto other = bloom::runtime::detail::resolveImageInputColorSpace(
        *aces,
        ImageProbe{1,
                   1,
                   32,
                   {},
                   ImageFormat::Exr,
                   ImageColorSpace::Linear,
                   bloom::media::ImageAlphaAssociation::Premultiplied,
                   "exr.chromaticities",
                   {}},
        ImageColorSpace::Auto, {});
    expectations.expect(
        ap0.id == "ACES2065-1" && ap1.id == "ACEScg" &&
            rec709.id == aces->rec709VideoColorSpaceId() && other.id.empty(),
        "EXR AP0/AP1/Rec.709 map to the pinned config ids and other chromaticities refuse");
}

void testVideoRules(Expectations& expectations) {
    auto config = neutralConfig();
    if (!config)
        return;
    const auto rec709 =
        bloom::media::video::resolveVideoInputColorSpace(*config, ColourTags{1, 1, 1, 1}, 0, {});
    const auto srgb =
        bloom::media::video::resolveVideoInputColorSpace(*config, ColourTags{1, 13, 1, 2}, 0, {});
    const auto linear =
        bloom::media::video::resolveVideoInputColorSpace(*config, ColourTags{1, 8, 1, 1}, 0, {});
    const auto hdr =
        bloom::media::video::resolveVideoInputColorSpace(*config, ColourTags{9, 16, 9, 1}, 0, {});
    expectations.expect(rec709.automatic && rec709.id == config->rec709VideoColorSpaceId(),
                        "BT.709 container tags map to the config video space");
    expectations.expect(srgb.id == config->sRgbTextureColorSpaceId() &&
                            linear.id == config->processColorSpaceId(),
                        "sRGB and linear container tags map to config spaces");
    expectations.expect(hdr.id.empty() && !hdr.warning.empty(),
                        "Rec.2020/HLG/PQ container tags remain typed refused");

    const auto explicitLog = bloom::media::video::resolveVideoInputColorSpace(
        *config, ColourTags{1, 1, 1, 1}, 0, "ARRI LogC3");
    expectations.expect(explicitLog.id == "ARRI LogC3" && !explicitLog.automatic,
                        "an explicit LogC3 pick overrides Rec.709 container tags");
    std::string diagnostic;
    const auto missing = bloom::runtime::detail::prepareInputColorProcessor(
        *config, {"missing-input", "missing-input", {}, false, false}, diagnostic);
    expectations.expect(!missing, "a missing explicit id fails closed before decoding");
}

} // namespace

int main() {
    Expectations expectations;
    testImageRules(expectations);
    testVideoRules(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
