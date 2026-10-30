#include <bloom/runtime/input_color_context.hpp>

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/media/cache/media_disk_cache_decode.hpp>
#include <bloom/media/video/colour.hpp>

#include <array>
#include <cmath>
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
            rec709.id == "Linear Rec.709 (sRGB)" && other.id.empty(),
        "EXR AP0/AP1/Rec.709 map to the pinned config ids and other chromaticities refuse");
    // The ACES CG config has no Rec.709 camera space: Bloom synthesizes one as the explicit camera
    // interpretation, and linear Rec.709 EXR data never uses it. Untagged SDR video resolves in
    // Auto to the config's sRGB texture space (ADR 0032), never refused.
    expectations.expect(aces->rec709VideoColorSpaceId() ==
                                bloom::color::kBloomRec709CameraColorSpaceId &&
                            !bloom::color::synthesizedColorSpaceIdentity(
                                 bloom::color::kBloomRec709CameraColorSpaceId)
                                 .empty(),
                        "the ACES CG config gains the synthesized Rec.709 camera input");
    const auto acesVideo = bloom::media::video::resolveVideoInputColorSpace(
        *aces, bloom::media::provider::ColourTags{2, 2, 2, 0}, 0, {}, {1280, 720, 24.0});
    std::string acesDiagnostic;
    expectations.expect(
        !aces->sRgbTextureColorSpaceId().empty() &&
            acesVideo.id == aces->sRgbTextureColorSpaceId() &&
            acesVideo.warning.find("assumed Rec.709 with the sRGB transfer") != std::string::npos &&
            static_cast<bool>(bloom::runtime::detail::prepareInputColorProcessor(
                *aces, {acesVideo.id, acesVideo.name, {}, false, true}, acesDiagnostic)),
        "untagged HD under ACES CG resolves to the config's sRGB texture space");
    const auto acesCamera = bloom::media::video::resolveVideoInputColorSpace(
        *aces, bloom::media::provider::ColourTags{1, 1, 1, 1}, 0,
        bloom::color::kBloomRec709CameraColorSpaceId);
    expectations.expect(acesCamera.id == bloom::color::kBloomRec709CameraColorSpaceId &&
                            !acesCamera.automatic &&
                            static_cast<bool>(bloom::runtime::detail::prepareInputColorProcessor(
                                *aces, {acesCamera.id, acesCamera.name, {}, false, false},
                                acesDiagnostic)),
                        "the Rec.709 camera interpretation stays an explicit ACES CG choice");
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
    // ADR 0032: ordinary Rec.709 video is shown as players show it, through the sRGB texture
    // space; the Rec.709 camera inverse OETF is an explicit choice, available under Neutral too.
    expectations.expect(rec709.automatic && rec709.id == config->sRgbTextureColorSpaceId() &&
                            rec709.id == "srgb_rec709_display",
                        "BT.709 container tags map to the config sRGB texture space");
    const auto camera = bloom::media::video::resolveVideoInputColorSpace(
        *config, ColourTags{1, 1, 1, 1}, 0, bloom::color::kBloomRec709CameraColorSpaceId);
    std::string cameraDiagnostic;
    const auto cameraProcessor = bloom::runtime::detail::prepareInputColorProcessor(
        *config, {camera.id, camera.name, {}, false, false}, cameraDiagnostic);
    std::array<std::array<float, 4>, 1> cameraSample{{{0.5F, 0.5F, 0.5F, 1.0F}}};
    expectations.expect(
        config->rec709VideoColorSpaceId() == bloom::color::kBloomRec709CameraColorSpaceId &&
            camera.id == bloom::color::kBloomRec709CameraColorSpaceId && !camera.automatic &&
            cameraProcessor && cameraProcessor->apply(cameraSample) &&
            std::abs(cameraSample[0][0] - std::pow((0.5F + 0.099F) / 1.099F, 1.0F / 0.45F)) <
                1e-4F,
        "Neutral offers the synthesized Rec.709 camera inverse OETF as an explicit choice");
    expectations.expect(srgb.id == config->sRgbTextureColorSpaceId() &&
                            linear.id == config->processColorSpaceId(),
                        "sRGB and linear container tags map to config spaces");
    expectations.expect(hdr.id.empty() && !hdr.warning.empty(),
                        "PQ tags refuse when the config has no Rec.2100 PQ space");

    const auto explicitLog = bloom::media::video::resolveVideoInputColorSpace(
        *config, ColourTags{1, 1, 1, 1}, 0, "ARRI LogC3");
    expectations.expect(explicitLog.id == "ARRI LogC3" && !explicitLog.automatic,
                        "an explicit LogC3 pick overrides Rec.709 container tags");
    // Issue #365: missing tags name themselves and the interpretation that resolves them; PQ
    // and HLG resolve to a config's Rec.2100 spaces, and an explicit pick overrides missing tags.
    const auto untagged =
        bloom::media::video::resolveVideoInputColorSpace(*config, ColourTags{2, 2, 2, 0}, 0, {});
    expectations.expect(untagged.id.empty() &&
                            untagged.warning.find("color primaries") != std::string::npos &&
                            untagged.warning.find("matrix coefficients") != std::string::npos &&
                            untagged.warning.find("range") != std::string::npos &&
                            untagged.warning.find("Input Color") != std::string::npos,
                        "an untagged stream names its missing tags and the override");
    // Owner decision (2026-10-24): with the picture geometry, Auto assumes the size convention
    // for the missing tags and carries the visible note instead of refusing.
    const auto assumedHd = bloom::media::video::resolveVideoInputColorSpace(
        *config, ColourTags{2, 2, 2, 0}, 0, {}, {1280, 720, 24.0});
    const auto assumedSd = bloom::media::video::resolveVideoInputColorSpace(
        *config, ColourTags{-1, -1, -1, -1}, 0, {}, {720, 576, 25.0});
    expectations.expect(assumedHd.automatic && assumedHd.id == config->sRgbTextureColorSpaceId() &&
                            assumedHd.warning ==
                                "Color tags missing — assumed Rec.709 with the sRGB transfer. "
                                "Change in Assets → Input Color",
                        "untagged HD resolves to the sRGB texture space with the note");
    expectations.expect(assumedSd.automatic && assumedSd.id == config->sRgbTextureColorSpaceId() &&
                            assumedSd.warning.find("assumed Rec.601 with the sRGB transfer") !=
                                std::string::npos,
                        "untagged SD resolves through the Rec.601 assumption with the note");
    const auto taggedSd = bloom::media::video::resolveVideoInputColorSpace(
        *config, ColourTags{6, 6, 6, 1}, 0, {}, {720, 480, 29.97});
    expectations.expect(taggedSd.automatic && taggedSd.id == config->sRgbTextureColorSpaceId() &&
                            taggedSd.warning.empty(),
                        "a tagged SMPTE 170M stream resolves without a note");
    const auto explicitAssumed = bloom::media::video::resolveVideoInputColorSpace(
        *config, ColourTags{2, 2, 2, 0}, 0, "lin_rec709_scene", {1920, 1080, 25.0});
    expectations.expect(explicitAssumed.id == "lin_rec709_scene" &&
                            explicitAssumed.warning.empty(),
                        "an explicit choice replaces the assumption and its note");
    const auto explicitUntagged = bloom::media::video::resolveVideoInputColorSpace(
        *config, ColourTags{2, 2, 2, 0}, 0, "lin_rec709_scene");
    expectations.expect(explicitUntagged.id == "lin_rec709_scene" &&
                            explicitUntagged.warning.empty(),
                        "an explicit interpretation overrides missing tags");
    const auto constantLuminance =
        bloom::media::video::resolveVideoInputColorSpace(*config, ColourTags{9, 1, 10, 1}, 0, {});
    expectations.expect(constantLuminance.id.empty() &&
                            constantLuminance.warning.find("constant-luminance") !=
                                std::string::npos,
                        "BT.2020 constant luminance is refused by name");
    expectations.expect(hdr.warning.find("Rec.2100 PQ") != std::string::npos,
                        "a config without PQ names the missing Rec.2100 space");
    const auto neutralHlg =
        bloom::media::video::resolveVideoInputColorSpace(*config, ColourTags{9, 18, 9, 1}, 0, {});
    std::string neutralHlgDiagnostic;
    expectations.expect(
        neutralHlg.automatic && neutralHlg.id == bloom::color::kBloomRec2100HlgColorSpaceId &&
            static_cast<bool>(bloom::runtime::detail::prepareInputColorProcessor(
                *config, {neutralHlg.id, neutralHlg.name, {}, false, true}, neutralHlgDiagnostic)),
        "Bloom Neutral also gains the synthesized HLG input space");
    expectations.expect(
        bloom::color::synthesizedColorSpaceIdentity(bloom::color::kBloomRec2100HlgColorSpaceId)
                    .find("ocio=") != std::string::npos &&
            bloom::color::synthesizedColorSpaceIdentity("lin_rec709_scene").empty(),
        "the synthesized space's identity names its definition and the OCIO version");
    if (auto aces = acesConfig()) {
        const auto pq =
            bloom::media::video::resolveVideoInputColorSpace(*aces, ColourTags{9, 16, 9, 1}, 0, {});
        const auto hlg =
            bloom::media::video::resolveVideoInputColorSpace(*aces, ColourTags{9, 18, 9, 1}, 0, {});
        // The ACES CG config declares its Rec.2100 spaces inactive; they still resolve by name.
        expectations.expect(pq.automatic && pq.id == aces->rec2100PqColorSpaceId() &&
                                pq.id.find("2100") != std::string::npos,
                            "a PQ stream resolves to the config's Rec.2100 PQ input space");
        // The pinned CG config defines no HLG space; Bloom synthesizes its scene-light space.
        expectations.expect(hlg.automatic && hlg.id == bloom::color::kBloomRec2100HlgColorSpaceId &&
                                aces->rec2100HlgColorSpaceId() == hlg.id,
                            "an HLG stream resolves to the synthesized Rec.2100 HLG space");
        std::string hlgDiagnostic;
        expectations.expect(static_cast<bool>(bloom::runtime::detail::prepareInputColorProcessor(
                                *aces, {hlg.id, hlg.name, {}, false, true}, hlgDiagnostic)),
                            "the synthesized HLG input processor builds on the ACES config");
        const auto pqUntaggedMatrix =
            bloom::media::video::resolveVideoInputColorSpace(*aces, ColourTags{9, 16, 2, 0}, 0, {});
        expectations.expect(pqUntaggedMatrix.id.empty() &&
                                pqUntaggedMatrix.warning.find("matrix coefficients") !=
                                    std::string::npos,
                            "PQ without a matrix tag needs an explicit interpretation");
        const auto pqAssumedMatrix = bloom::media::video::resolveVideoInputColorSpace(
            *aces, ColourTags{9, 16, 2, 0}, 0, {}, {3840, 2160, 24.0});
        expectations.expect(pqAssumedMatrix.id == aces->rec2100PqColorSpaceId() &&
                                !pqAssumedMatrix.warning.empty(),
                            "with geometry, a PQ stream's missing matrix and range are assumed");
        std::string pqDiagnostic;
        expectations.expect(static_cast<bool>(bloom::runtime::detail::prepareInputColorProcessor(
                                *aces, {pq.id, pq.name, {}, false, true}, pqDiagnostic)),
                            "the Rec.2100 PQ input processor builds");
    } else
        expectations.expect(false, "ACES config resolves for Rec.2100 rules");
    std::string diagnostic;
    const auto missing = bloom::runtime::detail::prepareInputColorProcessor(
        *config, {"missing-input", "missing-input", {}, false, false}, diagnostic);
    expectations.expect(!missing, "a missing explicit id fails closed before decoding");
}

void testSourcePrecedenceAndCache(Expectations& expectations) {
    bloom::document::AssetInterpretation asset;
    asset.colorSpace = bloom::document::AssetColorSpace::Raw;
    asset.inputColorSpaceId = "ACEScg";
    const auto inherited = bloom::document::effectiveAssetInputColor(asset, 0, {});
    const auto sourceLegacy = bloom::document::effectiveAssetInputColor(asset, 1, {});
    const auto sourceConfig = bloom::document::effectiveAssetInputColor(asset, 1, "ACES2065-1");
    expectations.expect(inherited.colorSpace == bloom::document::AssetColorSpace::Auto &&
                            inherited.inputColorSpaceId == "ACEScg" &&
                            sourceLegacy.colorSpace == bloom::document::AssetColorSpace::Srgb &&
                            sourceLegacy.inputColorSpaceId.empty() &&
                            sourceConfig.colorSpace == bloom::document::AssetColorSpace::Auto &&
                            sourceConfig.inputColorSpaceId == "ACES2065-1",
                        "source config > source legacy > asset config; hidden ids never win");
    bloom::media::cache::ImageCacheKeyInputs inputs;
    inputs.workingColorSpaceId = "ACEScg";
    inputs.inputColorSpaceId = inherited.inputColorSpaceId;
    const auto inheritedKey = bloom::media::cache::buildImageCacheKey(inputs, "test-decoder");
    inputs.colorSpace = bloom::media::ImageColorSpace::Srgb;
    inputs.inputColorSpaceId = sourceLegacy.inputColorSpaceId;
    const auto sourceKey = bloom::media::cache::buildImageCacheKey(inputs, "test-decoder");
    inputs.colorSpace = bloom::media::ImageColorSpace::Auto;
    inputs.inputColorSpaceId = sourceConfig.inputColorSpaceId;
    const auto explicitKey = bloom::media::cache::buildImageCacheKey(inputs, "test-decoder");
    expectations.expect(inheritedKey != sourceKey && sourceKey != explicitKey &&
                            inheritedKey != explicitKey,
                        "asset/source interpretation changes invalidate decoded-image cache");
}

} // namespace

// ADR 0032 compatibility: exactly the Auto video assets whose stored tags make Auto show them as
// ordinary SDR video (and so looked different before document 1.28) trigger the open note.
void testSdrVideoColourDefaultAssets(Expectations& expectations) {
    const auto video = [](const std::int64_t primaries, const std::int64_t transfer,
                          const std::int64_t matrix, const std::int64_t range,
                          const std::uint32_t width = 1920, const std::uint32_t height = 1080) {
        bloom::document::AssetRecord asset;
        asset.kind = bloom::document::AssetKind::Video;
        bloom::document::AssetVideoStream stream;
        stream.kind = 1;
        stream.width = width;
        stream.height = height;
        stream.framePeriod = *bloom::core::RationalTime::create(1, 25);
        stream.primaries = primaries;
        stream.transfer = transfer;
        stream.matrix = matrix;
        stream.range = range;
        asset.videoStreams = {stream};
        return asset;
    };
    using bloom::runtime::detail::assetUsesSdrVideoColourDefault;
    expectations.expect(assetUsesSdrVideoColourDefault(video(-1, -1, -1, -1)) &&
                            assetUsesSdrVideoColourDefault(video(2, 2, 2, 0, 720, 576)) &&
                            assetUsesSdrVideoColourDefault(video(1, 1, 1, 1)) &&
                            assetUsesSdrVideoColourDefault(video(6, 6, 6, 1, 720, 480)),
                        "untagged, Rec.709 and Rec.601 SDR video in Auto uses the SDR default");
    auto explicitChoice = video(1, 1, 1, 1);
    explicitChoice.interpretation.inputColorSpaceId = "lin_rec709_scene";
    auto legacyChoice = video(1, 1, 1, 1);
    legacyChoice.interpretation.colorSpace = bloom::document::AssetColorSpace::Linear;
    auto image = video(1, 1, 1, 1);
    image.kind = bloom::document::AssetKind::Image;
    expectations.expect(
        !assetUsesSdrVideoColourDefault(video(9, 16, 9, 1)) &&
            !assetUsesSdrVideoColourDefault(video(9, 18, 9, 1)) &&
            !assetUsesSdrVideoColourDefault(video(1, 13, 1, 2)) &&
            !assetUsesSdrVideoColourDefault(video(1, 8, 1, 1)) &&
            !assetUsesSdrVideoColourDefault(video(9, 1, 9, 1)) &&
            !assetUsesSdrVideoColourDefault(explicitChoice) &&
            !assetUsesSdrVideoColourDefault(legacyChoice) && !assetUsesSdrVideoColourDefault(image),
        "HDR, sRGB, linear and Rec.2020 tags, explicit or legacy choices and stills are unaffected");
}

int main() {
    Expectations expectations;
    testSdrVideoColourDefaultAssets(expectations);
    testImageRules(expectations);
    testVideoRules(expectations);
    testSourcePrecedenceAndCache(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
