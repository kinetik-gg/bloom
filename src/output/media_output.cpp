#include <algorithm>
#include <array>
#include <bloom/media/provider/encode_session.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/openh264_runtime.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/platform/process_supervisor.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <system_error>

namespace bloom::output {
using namespace media::provider;
H264RuntimeAvailabilityV1 verifyH264RuntimeV1() {
    const auto status = OpenH264Runtime().verify();
    return {.installed = status.installed,
            .version = status.version,
            .digest = status.digest,
            .directory = status.directory,
            .detail = status.detail};
}
H264EncoderSupportV1 h264EncoderSupportV1() noexcept {
#if defined(__APPLE__)
    return {.software = H264EncoderV1::VideoToolbox, .hardwareVaapi = false, .softwareReason = {}};
#elif defined(__linux__)
    if (OpenH264Runtime::platformSupported())
        return {.software = H264EncoderV1::OpenH264, .hardwareVaapi = true, .softwareReason = {}};
    return {.software = H264EncoderV1::None,
            .hardwareVaapi = true,
            .softwareReason = "Cisco publishes no OpenH264 binary for this processor, so H.264 "
                              "is encoded on VA-API hardware."};
#else
    return {.software = H264EncoderV1::None,
            .hardwareVaapi = false,
            .softwareReason = "This platform has no H.264 encoder yet."};
#endif
}
bool vaapiRenderNodeAvailableV1() {
#if defined(__linux__)
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/dri", error))
        if (entry.path().filename().string().starts_with("renderD"))
            return true;
#endif
    return false;
}
bool mediaWorkerAvailableV1() {
    std::error_code error;
    const auto worker = EncodeSessionV1::defaultWorker();
    return !worker.empty() && std::filesystem::is_regular_file(worker, error) && !error;
}
VaapiEncodeProbeV1 probeVaapiEncodeV1(std::string workerExecutable) {
    VaapiEncodeProbeV1 result;
#if defined(__linux__)
    if (!vaapiRenderNodeAvailableV1())
        return result;
    platform::ProcessOptions options;
    options.executable =
        workerExecutable.empty() ? EncodeSessionV1::defaultWorker() : std::move(workerExecutable);
    options.arguments = {"--probe-vaapi"};
    // Hardware drivers map large GPU address ranges; give the probe the worker ceiling.
    options.addressSpaceBytes = kWorkerAddressSpaceCeiling;
    auto launched = platform::ProcessSupervisor::launch(options);
    auto* process = std::get_if<std::unique_ptr<platform::ProcessSupervisor>>(&launched);
    if (process == nullptr || !*process)
        return result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::string text;
    std::array<std::byte, 4096> buffer{};
    while (text.size() < 65536) {
        auto read = (*process)->read(buffer, deadline, {});
        const auto* count = std::get_if<std::size_t>(&read);
        if (count == nullptr || *count == 0)
            break;
        text.append(reinterpret_cast<const char*>(buffer.data()), *count);
    }
    const auto finished = (*process)->finish(deadline);
    const auto* exit = std::get_if<int>(&finished);
    if (exit == nullptr || *exit != 0 || !text.starts_with("bloom-vaapi-probe-v1\n"))
        return result;
    std::size_t at = text.find('\n') + 1;
    while (at < text.size()) {
        const auto end = text.find('\n', at);
        if (end == std::string::npos)
            break;
        const auto line = std::string_view(text).substr(at, end - at);
        at = end + 1;
        if (line.size() < 5)
            continue;
        const bool h264 = line[line.size() - 3] == '1';
        const bool hevc = line[line.size() - 1] == '1';
        const auto node = std::string(line.substr(0, line.size() - 4));
        if (!h264)
            continue;
        if (result.renderNode.empty() || (hevc && !result.hevc))
            result = {node, h264, hevc};
    }
#else
    (void)workerExecutable;
#endif
    return result;
}
H264RuntimeAvailabilityV1 locateH264RuntimeV1(const std::filesystem::path& file,
                                              const std::function<bool()>& cancelled) {
    const auto result = OpenH264Runtime().locate(file, cancelled);
    return {.installed = result.installed,
            .version = OpenH264Runtime::version(),
            .digest = result.installed ? OpenH264Runtime::libraryDigest() : std::string{},
            .directory = result.directory,
            .detail = result.detail};
}
H264RuntimeAvailabilityV1 installH264RuntimeV1(const bool explicitConsent,
                                               const std::function<void(std::uint64_t)>& progress) {
    const auto result = OpenH264Runtime().install(explicitConsent, progress);
    return {.installed = result.installed,
            .version = OpenH264Runtime::version(),
            .digest = result.installed ? OpenH264Runtime::libraryDigest() : std::string{},
            .directory = result.directory,
            .detail = result.detail};
}
Result<MediaOutputAnalysisV1>
analyzeMediaOutputV1(OutputPresetV1 preset, EncodeSettingsV1 settings,
                     std::shared_ptr<const PreparedOutputDisplayV1> display,
                     std::uint64_t lookEffects) {
    const bool prores = preset == OutputPresetV1::ProResMovV1;
    const bool dnx = preset == OutputPresetV1::DnxhrMxfV1;
    const bool pcm = preset == OutputPresetV1::PcmWavV1;
    const bool h264 = preset == OutputPresetV1::H264MovV1 || preset == OutputPresetV1::H264Mp4V1;
    const bool hevc = preset == OutputPresetV1::HevcMovV1 || preset == OutputPresetV1::HevcMp4V1;
    const bool mp4 = preset == OutputPresetV1::H264Mp4V1 || preset == OutputPresetV1::HevcMp4V1;
#if defined(__APPLE__)
    // The preset is ProRes regardless of which provider name the settings carry; the macOS
    // provider spells it "prores", while the shared EncodeSettingsV1 default remains "prores_ks".
    const bool proresCodec = settings.videoCodec == "prores" || settings.videoCodec == "prores_ks";
#else
    const bool proresCodec = settings.videoCodec == "prores_ks";
#endif
    if ((!prores && !dnx && !pcm && !h264 && !hevc) || !valid(settings) ||
        (prores && (!proresCodec || settings.container != "mov")) ||
        (dnx && (settings.videoCodec != "dnxhd" || settings.container != "mxf")) ||
        (h264 && (settings.videoCodec != "h264" || settings.container != (mp4 ? "mp4" : "mov") ||
                  settings.profile != "high")) ||
        (hevc && (settings.videoCodec != "hevc" || settings.container != (mp4 ? "mp4" : "mov") ||
                  (settings.profile != "main" && settings.profile != "main10"))) ||
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
    if (h264)
        result.implementationNote = "Review deliverable — not for archival";
    if (hevc)
        result.implementationNote = "HEVC on VA-API hardware; not for archival";
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
    if (!pcm) {
        if (!display)
            display = PreparedOutputDisplayV1::prepare({});
        if (!display)
            return Unavailable{Error::Unavailable, "Output display processor unavailable"};
        result.display = std::move(display);
        result.facets[0].description = "OCIO display/view; clamp; lossy video";
        result.facets[2].description = result.display->description();
        const auto look = "look: baked (" + std::to_string(lookEffects) + " look-tagged effects)";
        result.facets[9].description += "; " + look;
        result.implementationNote += "; " + result.display->description() + "; " + look;
        const auto identityHex = result.display->digest().toLowercaseHex();
        result.facets[10].description +=
            "; display identity=" + std::string(identityHex.data(), identityHex.size());
    }
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
                                          const platform::ProcessCancellation& cancellation,
                                          const PreparedOutputDisplayV1* display) {
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
    std::shared_ptr<const PreparedOutputDisplayV1> fallback;
    if (!display) {
        fallback = PreparedOutputDisplayV1::prepare({});
        display = fallback.get();
    }
    if (!display)
        return Unavailable{Error::Unavailable, "Output display processor unavailable"};
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
        const float sourceAlpha = pixel.alpha();
        const auto straight = [&](float channel) -> double {
            return sourceAlpha == 0.0F ? 0.0 : static_cast<double>(channel / sourceAlpha);
        };
        const auto converted = display->processor().referenceToDisplay(
            {straight(pixel.red()), straight(pixel.green()), straight(pixel.blue()), alpha});
        if (!converted)
            return Unavailable{Error::InvalidValue, "Output display transform failed"};
        const std::array<double, 4> samples{sourceAlpha == 0.0F ? 0.0 : converted->red,
                                            sourceAlpha == 0.0F ? 0.0 : converted->green,
                                            sourceAlpha == 0.0F ? 0.0 : converted->blue, alpha};
        for (const auto value : samples) {
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
