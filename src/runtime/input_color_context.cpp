#include "input_color_context.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/media/video/colour.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <utility>

namespace bloom::runtime::detail {
namespace {
[[nodiscard]] bool zeroDigest(const core::Sha256Digest& digest) noexcept {
    return digest == core::Sha256Digest{};
}

[[nodiscard]] std::string errorName(const color::OcioColorSpaceProcessorError error) {
    switch (error) {
    case color::OcioColorSpaceProcessorError::None:
        return {};
    case color::OcioColorSpaceProcessorError::MissingInputColorSpace:
        return "the input color-space id is missing from the selected OCIO config";
    case color::OcioColorSpaceProcessorError::MissingWorkingColorSpace:
        return "the working color-space id is missing from the selected OCIO config";
    case color::OcioColorSpaceProcessorError::InputColorSpaceIsData:
        return "the selected input color space is a data space";
    case color::OcioColorSpaceProcessorError::WorkingColorSpaceInvalid:
        return "the selected destination color space is a data space";
    case color::OcioColorSpaceProcessorError::TransformBuildFailed:
        return "the OCIO color-space transform could not be built";
    case color::OcioColorSpaceProcessorError::CpuProcessorUnavailable:
        return "the OCIO CPU color-space processor is unavailable";
    case color::OcioColorSpaceProcessorError::UnsupportedFloatingPointEnvironment:
        return "the CPU floating-point environment is not the Bloom reference environment";
    case color::OcioColorSpaceProcessorError::InvalidColorSpaceId:
        return "the color-space id is invalid";
    }
    return "the OCIO color-space transform is unavailable";
}

} // namespace

std::optional<color::ResolvedBloomNeutralConfig>
resolveInputColorConfig(const EvaluationColorIntent& intent) {
    const auto uri =
        intent.ocioConfigUri.empty() ? color::kBloomNeutralV1ConfigUri : intent.ocioConfigUri;
    if (intent.ocioConfigUri.empty() && !zeroDigest(intent.ocioConfigRevision))
        return std::nullopt;
    auto revision = intent.ocioConfigRevision;
    if (zeroDigest(revision)) {
        const auto computed =
            color::ocioBuiltInContentRevision(color::OcioConfigLocatorKind::BloomBuiltIn, uri);
        if (!computed)
            return std::nullopt;
        revision = *computed;
    }
    auto resolved = color::resolveOcioBuiltIn(color::OcioConfigLocatorKind::BloomBuiltIn, uri,
                                              revision, intent.workingColorSpaceId);
    if (!resolved.ready())
        return std::nullopt;
    return std::move(resolved).takeResolved();
}

InputColorSpaceResolution
resolveImageInputColorSpace(const color::ResolvedBloomNeutralConfig& config,
                            const media::ImageProbe& probe, const media::ImageColorSpace legacy,
                            const std::string_view explicitId) {
    if (!explicitId.empty())
        return InputColorSpaceResolution{
            std::string(explicitId), std::string(explicitId), {}, false, false};
    if (legacy == media::ImageColorSpace::Raw)
        return InputColorSpaceResolution{"", "", "", true, false};
    if (legacy == media::ImageColorSpace::Linear)
        return InputColorSpaceResolution{std::string(config.processColorSpaceId()),
                                         std::string(config.processColorSpaceId()),
                                         {},
                                         false,
                                         false};
    if (probe.format != media::ImageFormat::Exr || legacy == media::ImageColorSpace::Srgb) {
        if (legacy == media::ImageColorSpace::Auto && probe.bitDepth != 8 && probe.bitDepth != 16)
            return InputColorSpaceResolution{
                "", "",
                "Automatic input color interpretation supports only 8/16-bit "
                "PNG, JPEG, and TIFF; choose an explicit OCIO color space",
                false, true};
        return InputColorSpaceResolution{std::string(config.sRgbTextureColorSpaceId()),
                                         std::string(config.sRgbTextureColorSpaceId()),
                                         {},
                                         false,
                                         legacy == media::ImageColorSpace::Auto};
    }

    if (probe.interpretationAssumption.find("absent") != std::string::npos)
        return InputColorSpaceResolution{std::string(config.processColorSpaceId()),
                                         std::string(config.processColorSpaceId()),
                                         "EXR chromaticities are absent; the working space is "
                                         "assumed",
                                         false, true};
    if (probe.colorSpaceTag == "ACES2065-1")
        return InputColorSpaceResolution{"ACES2065-1", "ACES2065-1", {}, false, true};
    if (probe.colorSpaceTag == "ACEScg")
        return InputColorSpaceResolution{"ACEScg", "ACEScg", {}, false, true};
    if (probe.colorSpaceTag == "lin_rec709_scene") {
        // Linear Rec.709/D65 data maps to the config's scene-linear Rec.709 space, never to a
        // camera/video space (that carries the BT.709 OETF): Bloom Neutral's lin_rec709_scene,
        // the ACES CG config's "Linear Rec.709 (sRGB)".
        const auto contains = [](std::string_view text, std::string_view part) {
            return std::search(text.begin(), text.end(), part.begin(), part.end(),
                               [](char a, char b) {
                                   return std::tolower(static_cast<unsigned char>(a)) ==
                                          std::tolower(static_cast<unsigned char>(b));
                               }) != text.end();
        };
        std::string linear;
        for (const auto& space : config.colorSpaces())
            if (space.id == "lin_rec709_scene" ||
                (linear.empty() && space.sceneLinear && contains(space.id, "rec.709") &&
                 !contains(space.id, "camera")))
                linear = space.id;
        if (!linear.empty())
            return InputColorSpaceResolution{linear, linear, {}, false, true};
        return InputColorSpaceResolution{
            "", "", "The OCIO config has no scene-linear Rec.709 color space; choose one",
            false, true};
    }
    return InputColorSpaceResolution{
        "", "", "EXR chromaticities are not recognized; choose an explicit OCIO color space",
        false, true};
}

std::shared_ptr<const color::CpuColorSpaceProcessor>
prepareInputColorProcessor(const color::ResolvedBloomNeutralConfig& config,
                           const InputColorSpaceResolution& resolution, std::string& diagnostic) {
    if (resolution.noConversion)
        return {};
    if (resolution.id.empty()) {
        diagnostic = resolution.warning.empty()
                         ? "No input color space was resolved from the media metadata"
                         : resolution.warning;
        return {};
    }
    auto prepared =
        color::CpuColorSpaceProcessor::prepare(config, resolution.id, config.processColorSpaceId());
    if (!prepared) {
        diagnostic = "Input color space '" + resolution.id +
                     "' is unavailable: " + errorName(prepared.error());
        return {};
    }
    return std::move(prepared).takeProcessor();
}

bool assetUsesSdrVideoColourDefault(const document::AssetRecord& asset) {
    if (asset.kind != document::AssetKind::Video ||
        asset.interpretation.colorSpace != document::AssetColorSpace::Auto ||
        !asset.interpretation.inputColorSpaceId.empty())
        return false;
    // The stream Auto interprets: the first captured video stream, as Assets shows it.
    const auto stream = std::ranges::find(asset.videoStreams, 1U, &document::AssetVideoStream::kind);
    if (stream == asset.videoStreams.end())
        return false;
    const media::provider::ColourTags tags{
        static_cast<std::int32_t>(stream->primaries), static_cast<std::int32_t>(stream->transfer),
        static_cast<std::int32_t>(stream->matrix), static_cast<std::int32_t>(stream->range)};
    const auto period = stream->framePeriod;
    return media::video::sdrVideoColourDefaultApplies(
        tags, {stream->width, stream->height,
               period.numerator() > 0 ? static_cast<double>(period.denominator()) /
                                            static_cast<double>(period.numerator())
                                      : 0.0});
}

} // namespace bloom::runtime::detail
