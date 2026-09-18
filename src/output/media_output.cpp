#include <algorithm>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/output/media_output.hpp>
#include <cmath>

namespace bloom::output {
using namespace media::provider;
Result<MediaOutputAnalysisV1> analyzeMediaOutputV1(OutputPresetV1 preset,
                                                   EncodeSettingsV1 settings) {
    const bool prores = preset == OutputPresetV1::ProResMovV1;
    const bool dnx = preset == OutputPresetV1::DnxhrMxfV1;
    const bool pcm = preset == OutputPresetV1::PcmWavV1;
    if ((!prores && !dnx && !pcm) || !valid(settings) ||
        (prores && (settings.videoCodec != "prores_ks" || settings.container != "mov")) ||
        (dnx && (settings.videoCodec != "dnxhd" || settings.container != "mxf")) ||
        (pcm && (!settings.videoCodec.empty() || settings.container != "wav" ||
                 (settings.audioCodec != "pcm_s16le" && settings.audioCodec != "pcm_s24le"))))
        return Unavailable{Error::InvalidValue, "Preset and encode settings differ"};
    const auto presetDigest = media::provider::digest(settings);
    if (const auto* e = std::get_if<Unavailable>(&presetDigest))
        return *e;
    MediaOutputAnalysisV1 result;
    result.preset = preset;
    result.settings = std::move(settings);
    result.determinism = encodeDeterminism(result.settings);
    result.toleranceProfile = encodeTolerance(result.settings);
    if (prores)
        result.implementationNote = kProResExportNote;
    using F = OutputFacetIdV1;
    using S = OutputPreservationStateV1;
    const bool alpha =
        prores && (result.settings.profile == "4444" || result.settings.profile == "4444xq");
    result.facets = {
        {{F::Pixels, pcm ? S::Omitted : S::Approximated,
          pcm ? "Audio only" : "Scene-linear Rec.709 to sRGB; clamp; lossy video"},
         {F::Precision, S::Approximated,
          pcm ? "Float mix to signed PCM at source sample rate"
              : "Float32 to RGBA16 and codec sample precision"},
         {F::Color, pcm ? S::Omitted : S::Approximated,
          pcm ? "No picture" : "sRGB transfer; Rec.709 primaries and matrix; limited YUV"},
         {F::AlphaAssociation, alpha ? S::Approximated : S::Omitted,
          alpha ? "Straight alpha retained by ProRes 4444" : "Alpha omitted"},
         {F::Channels, result.settings.audioCodec.empty() ? S::Omitted : S::Equivalent,
          pcm                                  ? "PCM channel layout retained"
          : result.settings.audioCodec.empty() ? "RGB to codec YUV; audio omitted"
                                               : "RGB to codec YUV; PCM channel layout retained"},
         {F::DataWindow, S::Exact, "Origin at zero required"},
         {F::DisplayWindow, S::Exact, "Equal data and display windows required"},
         {F::PixelAspect, S::Exact, "Square pixels required"},
         {F::Compression, pcm ? S::Exact : S::Approximated,
          pcm ? "Uncompressed PCM" : "Intra-frame lossy codec; immutable tolerance profile"},
         {F::Metadata, S::Equivalent,
          "Exact rational cadence, duration, stream layout; optional BWF description"},
         {F::ExternalDependencies, S::ExternalReference,
          prores ? kProResExportNote
                 : "FFmpeg worker; same-provider reopen; no independent delivery verification"}}};
    core::Sha256Hasher hasher;
    const std::string_view domain = "BloomMediaOutputAnalysisV1";
    (void)hasher.update(std::as_bytes(std::span(domain.data(), domain.size())));
    const auto byte = static_cast<std::byte>(preset);
    (void)hasher.update(std::span(&byte, 1));
    (void)hasher.update(std::as_bytes(std::span(std::get<Digest>(presetDigest).bytes())));
    for (const auto& facet : result.facets) {
        const std::array<std::byte, 2> tags{static_cast<std::byte>(facet.facet),
                                            static_cast<std::byte>(facet.preservation)};
        (void)hasher.update(tags);
        (void)hasher.update(
            std::as_bytes(std::span(facet.description.data(), facet.description.size())));
        const std::byte terminator{};
        (void)hasher.update(std::span(&terminator, 1));
    }
    result.digest = hasher.finalize();
    return result;
}
Result<FrameProduct> prepareMediaRgba16V1(const render::Rgba32fImage& image, Rational pts,
                                          const platform::ProcessCancellation& cancellation) {
    const auto* descriptor = image.descriptor();
    if (!descriptor || !valid(pts) || descriptor->dataWindow() != descriptor->displayWindow() ||
        descriptor->pixelAspect() != core::PixelAspectRatio::square())
        return Unavailable{Error::InvalidValue,
                           "Media output requires equal windows and square pixels"};
    const auto window = descriptor->dataWindow();
    if (window.originX() != 0 || window.originY() != 0 ||
        window.extent().width() > Limits::dimension ||
        window.extent().height() > Limits::dimension || image.pixels().size() > Limits::pixels)
        return Unavailable{Error::Oversized, "Media output dimensions exceed provider limits"};
    FrameProduct result;
    result.format = PixelFormat::Rgba16;
    result.pts = pts;
    result.colour = {1, 13, 0, 2};
    CpuPlane plane;
    plane.width = static_cast<std::uint32_t>(window.extent().width());
    plane.height = static_cast<std::uint32_t>(window.extent().height());
    plane.stride = plane.width * 8U;
    plane.bytes.resize(image.pixels().size() * 8U);
    std::size_t offset = 0;
    for (const auto pixel : image.pixels()) {
        if (offset % (std::size_t{64} * 1024U) == 0 && cancellation && cancellation())
            return Unavailable{Error::Cancelled, "Output preparation cancelled"};
        const double alpha = std::clamp(static_cast<double>(pixel.alpha()), 0.0, 1.0);
        const std::array<double, 4> samples{
            static_cast<double>(pixel.red()), static_cast<double>(pixel.green()),
            static_cast<double>(pixel.blue()), static_cast<double>(pixel.alpha())};
        for (std::size_t c = 0; c < samples.size(); ++c) {
            double value = alpha;
            if (c != 3) {
                const double linear = alpha > 0 ? std::clamp(samples[c] / alpha, 0.0, 1.0) : 0;
                value = linear <= 0.0031308 ? linear * 12.92
                                            : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
            }
            const auto quantized =
                static_cast<std::uint32_t>(std::floor(std::clamp(value, 0.0, 1.0) * 65535.0 + 0.5));
            plane.bytes[offset++] = static_cast<std::byte>(quantized & 255U);
            plane.bytes[offset++] = static_cast<std::byte>(quantized >> 8U);
        }
    }
    plane.digest = digestBytes(plane.bytes);
    result.planes.push_back(std::move(plane));
    return result;
}
} // namespace bloom::output
