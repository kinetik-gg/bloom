#include <bloom/output/output_analysis.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace {

using bloom::output::OutputFacetIdV1;

[[nodiscard]] constexpr std::uint16_t facetBit(const OutputFacetIdV1 facet) noexcept {
    return static_cast<std::uint16_t>(std::uint16_t{1} << (static_cast<std::uint8_t>(facet) - 1U));
}

[[nodiscard]] constexpr bool isKnownFacet(const OutputFacetIdV1 facet) noexcept {
    const auto value = static_cast<std::uint8_t>(facet);
    return value >= static_cast<std::uint8_t>(OutputFacetIdV1::Pixels) &&
           value <= static_cast<std::uint8_t>(OutputFacetIdV1::ExternalDependencies);
}

} // namespace

namespace bloom::output {

std::optional<OutputPresetIdentityV1> outputPresetIdentityV1(const OutputPresetV1 preset) noexcept {
    switch (preset) {
    case OutputPresetV1::PngRgba8SrgbV1:
        return OutputPresetIdentityV1{"PngRgba8SrgbV1", kOutputPresetVersionV1,
                                      "bloom.output.png-rgba8-srgb.semantic.v1"};
    case OutputPresetV1::FlatExrRgba32fLinRec709SceneV1:
        return OutputPresetIdentityV1{"FlatExrRgba32fLinRec709SceneV1", kOutputPresetVersionV1,
                                      "bloom.output.exr-rgba32f-lin-rec709-scene.semantic.v1"};
    }
    return std::nullopt;
}

std::optional<OutputFacetDescriptorSchemasV1>
outputFacetDescriptorSchemasV1(const OutputPresetV1 preset, const OutputFacetIdV1 facet) noexcept {
    if (!outputPresetIdentityV1(preset) || !isKnownFacet(facet)) {
        return std::nullopt;
    }

    OutputFacetDescriptorSchemaV1 source = OutputFacetDescriptorSchemaV1::Absent;
    OutputFacetDescriptorSchemaV1 target = OutputFacetDescriptorSchemaV1::Absent;
    switch (facet) {
    case OutputFacetIdV1::Pixels:
        source = target = OutputFacetDescriptorSchemaV1::Pixels;
        break;
    case OutputFacetIdV1::Precision:
        source = target = OutputFacetDescriptorSchemaV1::Precision;
        break;
    case OutputFacetIdV1::Color:
        source = target = OutputFacetDescriptorSchemaV1::Color;
        break;
    case OutputFacetIdV1::AlphaAssociation:
        source = target = OutputFacetDescriptorSchemaV1::AlphaAssociation;
        break;
    case OutputFacetIdV1::Channels:
        source = target = OutputFacetDescriptorSchemaV1::Channels;
        break;
    case OutputFacetIdV1::DataWindow:
    case OutputFacetIdV1::DisplayWindow:
        source = target = OutputFacetDescriptorSchemaV1::Window;
        break;
    case OutputFacetIdV1::PixelAspect:
        source = OutputFacetDescriptorSchemaV1::PixelAspectRational;
        target = preset == OutputPresetV1::PngRgba8SrgbV1
                     ? OutputFacetDescriptorSchemaV1::PixelAspectRational
                     : OutputFacetDescriptorSchemaV1::PixelAspectBinary32;
        break;
    case OutputFacetIdV1::Compression:
        source = OutputFacetDescriptorSchemaV1::Absent;
        target = OutputFacetDescriptorSchemaV1::Compression;
        break;
    case OutputFacetIdV1::Metadata:
        source = target = OutputFacetDescriptorSchemaV1::Metadata;
        break;
    case OutputFacetIdV1::ExternalDependencies:
        source = target = OutputFacetDescriptorSchemaV1::ExternalDependencies;
        break;
    }
    return OutputFacetDescriptorSchemasV1{source, target};
}

std::optional<OutputFacetStableCodeRuleV1>
outputFacetStableCodeRuleV1(const OutputFacetStableCodeV1 code) noexcept {
    const auto one = [](const std::string_view text, const OutputFacetIdV1 facet,
                        const OutputPreservationStateV1 state, const bool png, const bool exr,
                        const bool permits) noexcept {
        return OutputFacetStableCodeRuleV1{text, facetBit(facet), state, png, exr, permits};
    };
    switch (code) {
    case OutputFacetStableCodeV1::None:
        return OutputFacetStableCodeRuleV1{
            "",  kOutputAnalysisAllFacetsPermittedV1, OutputPreservationStateV1::Exact, true, true,
            true};
    case OutputFacetStableCodeV1::PngDisplayTransformClampQuantize:
        return one("png.display-transform-clamp-quantize", OutputFacetIdV1::Pixels,
                   OutputPreservationStateV1::Approximated, true, false, true);
    case OutputFacetStableCodeV1::ProcessFrameMissing:
        return one("process-frame.missing", OutputFacetIdV1::Pixels,
                   OutputPreservationStateV1::Missing, true, true, false);
    case OutputFacetStableCodeV1::PixelsUnsupported:
        return one("pixels.unsupported", OutputFacetIdV1::Pixels,
                   OutputPreservationStateV1::Unsupported, true, true, false);
    case OutputFacetStableCodeV1::PngFloat32ToUint8:
        return one("png.float32-to-uint8", OutputFacetIdV1::Precision,
                   OutputPreservationStateV1::Approximated, true, false, true);
    case OutputFacetStableCodeV1::PrecisionUnsupported:
        return one("precision.unsupported", OutputFacetIdV1::Precision,
                   OutputPreservationStateV1::Unsupported, true, true, false);
    case OutputFacetStableCodeV1::PngLinRec709SceneToSrgb:
        return one("png.lin-rec709-scene-to-srgb", OutputFacetIdV1::Color,
                   OutputPreservationStateV1::Approximated, true, false, true);
    case OutputFacetStableCodeV1::OcioMissing:
        return one("ocio.missing", OutputFacetIdV1::Color, OutputPreservationStateV1::Missing, true,
                   false, false);
    case OutputFacetStableCodeV1::OcioChanged:
        return one("ocio.changed", OutputFacetIdV1::Color, OutputPreservationStateV1::Missing, true,
                   false, false);
    case OutputFacetStableCodeV1::OcioInvalid:
        return one("ocio.invalid", OutputFacetIdV1::Color, OutputPreservationStateV1::Missing, true,
                   false, false);
    case OutputFacetStableCodeV1::OcioResourceMissing:
        return one("ocio.resource-missing", OutputFacetIdV1::Color,
                   OutputPreservationStateV1::Missing, true, false, false);
    case OutputFacetStableCodeV1::OcioVersionUnsupported:
        return one("ocio.version-unsupported", OutputFacetIdV1::Color,
                   OutputPreservationStateV1::Missing, true, false, false);
    case OutputFacetStableCodeV1::ColorUnsupported:
        return one("color.unsupported", OutputFacetIdV1::Color,
                   OutputPreservationStateV1::Unsupported, true, true, false);
    case OutputFacetStableCodeV1::PngPremultipliedToStraight:
        return one("png.premultiplied-to-straight", OutputFacetIdV1::AlphaAssociation,
                   OutputPreservationStateV1::Approximated, true, false, true);
    case OutputFacetStableCodeV1::AlphaUnsupported:
        return one("alpha.unsupported", OutputFacetIdV1::AlphaAssociation,
                   OutputPreservationStateV1::Unsupported, true, true, false);
    case OutputFacetStableCodeV1::ChannelsUnsupported:
        return one("channels.unsupported", OutputFacetIdV1::Channels,
                   OutputPreservationStateV1::Unsupported, true, true, false);
    case OutputFacetStableCodeV1::PngOriginWindowRequired:
        return one("png.origin-window-required", OutputFacetIdV1::DataWindow,
                   OutputPreservationStateV1::Unsupported, true, false, false);
    case OutputFacetStableCodeV1::WindowOutOfRange:
        return OutputFacetStableCodeRuleV1{
            "window.out-of-range",
            static_cast<std::uint16_t>(facetBit(OutputFacetIdV1::DataWindow) |
                                       facetBit(OutputFacetIdV1::DisplayWindow)),
            OutputPreservationStateV1::Unsupported,
            true,
            true,
            false};
    case OutputFacetStableCodeV1::PngEqualWindowRequired:
        return one("png.equal-window-required", OutputFacetIdV1::DisplayWindow,
                   OutputPreservationStateV1::Unsupported, true, false, false);
    case OutputFacetStableCodeV1::PngSquarePixelRequired:
        return one("png.square-pixel-required", OutputFacetIdV1::PixelAspect,
                   OutputPreservationStateV1::Unsupported, true, false, false);
    case OutputFacetStableCodeV1::ExrParRoundedBinary32:
        return one("exr.par-rounded-binary32", OutputFacetIdV1::PixelAspect,
                   OutputPreservationStateV1::Approximated, false, true, true);
    case OutputFacetStableCodeV1::PixelAspectUnsupported:
        return one("pixel-aspect.unsupported", OutputFacetIdV1::PixelAspect,
                   OutputPreservationStateV1::Unsupported, true, true, false);
    case OutputFacetStableCodeV1::CompressionUnavailable:
        return one("compression.unavailable", OutputFacetIdV1::Compression,
                   OutputPreservationStateV1::Missing, true, true, false);
    case OutputFacetStableCodeV1::CompressionUnsupported:
        return one("compression.unsupported", OutputFacetIdV1::Compression,
                   OutputPreservationStateV1::Unsupported, true, true, false);
    case OutputFacetStableCodeV1::MetadataUnsupported:
        return one("metadata.unsupported", OutputFacetIdV1::Metadata,
                   OutputPreservationStateV1::Unsupported, true, true, false);
    case OutputFacetStableCodeV1::PngOcioExternalReference:
        return one("png.ocio-external-reference", OutputFacetIdV1::ExternalDependencies,
                   OutputPreservationStateV1::ExternalReference, true, false, true);
    case OutputFacetStableCodeV1::DependencyMissing:
        return one("dependency.missing", OutputFacetIdV1::ExternalDependencies,
                   OutputPreservationStateV1::Missing, true, true, false);
    case OutputFacetStableCodeV1::AdapterUnavailable:
        return one("adapter.unavailable", OutputFacetIdV1::ExternalDependencies,
                   OutputPreservationStateV1::Missing, true, true, false);
    case OutputFacetStableCodeV1::ResourceLimitExceeded:
        return one("resource.limit-exceeded", OutputFacetIdV1::ExternalDependencies,
                   OutputPreservationStateV1::Missing, true, true, false);
    }
    return std::nullopt;
}

std::optional<std::string_view>
outputFacetStableCodeTextV1(const OutputFacetStableCodeV1 code) noexcept {
    const auto rule = outputFacetStableCodeRuleV1(code);
    return rule ? std::optional<std::string_view>{rule->serializedCode} : std::nullopt;
}

} // namespace bloom::output
