#include <array>
#include <bloom/media/provider/ffmpeg_manifest.hpp>

namespace bloom::media::provider {
Handshake ffmpegHandshake(const bool hardware) {
    Handshake h;
    auto& e = h.execution;
    e.provider = "bloom.ffmpeg";
    e.build = BLOOM_FFMPEG_BUILD_ID;
    if (const auto lock = Digest::fromLowercaseHex(BLOOM_MEDIA_DEPENDENCY_LOCK))
        e.dependencyLock = *lock;
    e.os = BLOOM_MEDIA_OS;
    e.architecture = BLOOM_MEDIA_ARCH;
    e.sdk = "FFmpeg-8.1.2";
    e.driver = hardware ? "vaapi-runtime-unqualified" : "none";
    e.device = hardware ? "/dev/dri/renderD128" : "cpu";
    e.implementation = hardware ? Implementation::Hardware : Implementation::Software;
    e.synchronization = "pipe-order";
    e.resourceProfile = "media-v1";
    e.entitlement = "apple_authorized=false;delivery_qualified=false";
    e.trustDomain = "ffmpeg-lgpl";
    QualificationEvidenceV1 q;
    if (const auto fixtures = Digest::fromLowercaseHex(BLOOM_FFMPEG_FIXTURE_ID))
        q.fixtures = *fixtures;
    q.reviewDate = "2026-09-17";
    q.qualification = Qualification::PreviewQualified;
    q.result = hardware ? QcResult::Incomplete : QcResult::Pass;
    if (hardware)
        q.qualification = Qualification::Development;
    auto add = [&](Role role, const char* codec) {
        MediaCapabilityKeyV1 c;
        c.role = role;
        c.container = "intake-demuxers-v1";
        c.mapping = role == Role::Probe ? "bounded-discovery-v1" : "stream-selection-v1";
        c.codec = codec;
        c.profile = "stream-declared";
        c.level = c.tier = c.sampleEntry = "stream-declared";
        c.bitDepth = 16;
        c.range = c.chroma = c.chromaLocation = "stream-declared";
        c.alpha = "preserve";
        c.fieldMode = "progressive";
        c.maxWidth = c.maxHeight = Limits::dimension;
        c.maxRate = {1000, 1};
        c.audioFormat = "planar-float";
        c.channelLayout = "stream-declared";
        c.colorFeatures = "h273-original";
        c.hdrFeatures = "metadata-only";
        c.timecodeFeatures = "original-tag";
        c.metadataFeatures = "duration-rate-timebase";
        c.timing = "presentation-order-exact";
        c.surfaceSemantics = "cpu-planes-v2";
        h.declarations.push_back({std::move(c), e, q});
    };
    if (!hardware) {
        add(Role::Probe, "intake-discovery");
        add(Role::DemuxIndex, "intake-discovery");
    }
    // VP9 is FFmpeg's native decoder; AV1 is the bundled dav1d (ADR 0036).
    for (const auto* codec : {"h264", "hevc", "vp9", "av1", "prores", "dnxhd", "mjpeg", "tiff"})
        if (!hardware || std::string_view(codec) == "h264" || std::string_view(codec) == "hevc")
            add(Role::VideoDecode, codec);
    if (!hardware)
        for (const auto* codec :
             {"aac", "mp3", "opus", "pcm_s16le", "pcm_s16be", "pcm_s24le", "pcm_s24be", "pcm_s32le",
              "pcm_s32be", "pcm_f32le", "pcm_f32be", "pcm_f64le", "pcm_f64be", "pcm_u8", "pcm_s8"})
            add(Role::AudioDecode, codec);
    if (!hardware) {
        auto encode = [&](Role role, const char* codec, const char* container,
                          const char* profile) {
            add(role, codec);
            auto& d = h.declarations.back();
            d.capability.purpose = Purpose::Export;
            d.capability.container = container;
            d.capability.profile = profile;
            d.capability.mapping = "bounded-encode-v1";
            d.capability.surfaceSemantics = "rgba16-srgb-pcm-v1";
            d.evidence.qualification = Qualification::Development;
            if (const auto fixtures = Digest::fromLowercaseHex(BLOOM_FFMPEG_ENCODE_FIXTURE_ID))
                d.evidence.fixtures = *fixtures;
        };
        for (const auto* profile : {"proxy", "lt", "422", "hq", "4444", "4444xq"})
            encode(Role::VideoEncode, "prores_ks", "mov", profile);
        for (const auto* profile :
             {"dnxhd", "dnxhr_lb", "dnxhr_sq", "dnxhr_hq", "dnxhr_hqx", "dnxhr_444"})
            encode(Role::VideoEncode, "dnxhd", "mxf", profile);
        encode(Role::VideoEncode, "tiff", "tiff", "rgba16");
        for (const auto* codec : {"pcm_s16le", "pcm_s24le", "aac"})
            encode(Role::AudioEncode, codec, "intake-muxers-v1", "source-rate");
        // Opus (the bundled libopus) is defined at 48 kHz and carried by WebM.
        encode(Role::AudioEncode, "opus", "webm", "48000");
        for (const auto* container : {"mov", "mp4", "mxf", "matroska", "webm", "wav", "tiff"}) {
            encode(Role::Mux, "intake-encoders-v1", container, "closed-stream-layout-v1");
            encode(Role::ReopenDecode, "intake-encoders-v1", container, "first-last-pcm-v1");
        }
        // Software H.264 and HEVC encode: the bundled OpenH264 and Kvazaar, linked into
        // libavcodec on every platform. Kvazaar is 8-bit: a Main 10 request is served as Main.
        for (const auto [codec, profile] : {std::pair{"h264", "high"}, std::pair{"hevc", "main"}}) {
            encode(Role::VideoEncode, codec, "mov", profile);
            auto& d = h.declarations.back();
            d.capability.bitDepth = 8;
            d.capability.chroma = "yuv420p";
            d.capability.range = "limited";
            d.capability.colorFeatures = "rec709-display-referred";
        }
        // VP9 (the bundled libvpx, Profile 0 or 2) and AV1 (the bundled SVT-AV1, Main), each
        // 8-bit or 10-bit 4:2:0; software only (ADR 0036).
        for (const auto* codec : {"vp9", "av1"})
            for (const auto [profile, depth] : {std::pair{"8bit", 8U}, std::pair{"10bit", 10U}}) {
                encode(Role::VideoEncode, codec, "webm", profile);
                auto& d = h.declarations.back();
                d.capability.bitDepth = depth;
                d.capability.chroma = depth == 8U ? "yuv420p" : "yuv420p10";
                d.capability.range = "limited";
                d.capability.colorFeatures = "rec709-display-referred";
            }
    } else {
        auto hardwareEncode = [&](const char* codec) {
            add(Role::VideoEncode, codec);
            auto& d = h.declarations.back();
            d.capability.purpose = Purpose::Export;
            d.capability.container = "mov";
            d.capability.profile = "high";
            d.capability.mapping = "bounded-encode-v1";
            d.capability.bitDepth = 8;
            d.capability.chroma = "yuv420p";
            d.capability.range = "limited";
            d.capability.colorFeatures = "rec709-display-referred";
        };
        hardwareEncode("h264");
        hardwareEncode("hevc");
        for (const auto* codec : {"pcm_s16le", "pcm_s24le", "aac"}) {
            add(Role::AudioEncode, codec);
            auto& d = h.declarations.back();
            d.capability.purpose = Purpose::Export;
            d.capability.container = "intake-muxers-v1";
            d.capability.profile = "source-rate";
            d.capability.mapping = "bounded-encode-v1";
        }
        for (const auto* container : {"mov", "mp4"})
            for (const auto role : {Role::Mux, Role::ReopenDecode}) {
                add(role, "intake-encoders-v1");
                auto& d = h.declarations.back();
                d.capability.purpose = Purpose::Export;
                d.capability.container = container;
                d.capability.profile =
                    role == Role::Mux ? "closed-stream-layout-v1" : "first-last-pcm-v1";
                d.capability.mapping = "bounded-encode-v1";
            }
    }
    h.transports = {Transport::PipeCopiesV0};
    return h;
}
Handshake ffmpegNvencHandshake() {
    auto h = ffmpegHandshake(true);
    auto& e = h.execution;
    e.driver = "nvenc-runtime-unqualified";
    e.device = "nvidia-nvenc";
    std::vector<ProviderDeclaration> declarations;
    for (auto declaration : h.declarations) {
        declaration.execution = e;
        declarations.push_back(declaration);
        // NVENC encodes HEVC Main 10 from P010 frames.
        if (declaration.capability.role == Role::VideoEncode &&
            declaration.capability.codec == "hevc") {
            declaration.capability.profile = "main10";
            declaration.capability.bitDepth = 10;
            declaration.capability.chroma = "p010";
            declarations.push_back(std::move(declaration));
        }
    }
    h.declarations = std::move(declarations);
    return h;
}
Handshake ffmpegHardwareDecodeHandshake() {
    // Preview-only VA-API decode. The worker discovers the render node at run time, so the
    // execution key names the discovery policy rather than one device path. Every worker
    // generation re-proves its first hardware frame against the software reference before any
    // hardware frame is returned, and the host never routes final rendering here.
    auto h = ffmpegHandshake();
    auto& e = h.execution;
    e.driver = "vaapi-libva";
    e.device = "vaapi-render-node-discovered";
    e.implementation = Implementation::Hardware;
    e.entitlement = "apple_authorized=false;delivery_qualified=false;preview_only=true";
    std::vector<ProviderDeclaration> declarations;
    for (auto declaration : h.declarations) {
        if (declaration.capability.role != Role::VideoDecode ||
            (declaration.capability.codec != "h264" && declaration.capability.codec != "hevc"))
            continue;
        declaration.execution = e;
        declaration.capability.surfaceSemantics = "cpu-planes-v2-vaapi-download";
        declaration.evidence.reviewDate = "2026-10-23";
        declaration.evidence.qualification = Qualification::PreviewQualified;
        declaration.evidence.result = QcResult::Pass;
        declarations.push_back(std::move(declaration));
    }
    h.declarations = std::move(declarations);
    return h;
}
PipelineQualificationV1 ffmpegPipeline(const ProviderDeclaration& d) {
    PipelineQualificationV1 p;
    p.steps.push_back({std::get<Digest>(digest(d.capability)),
                       std::get<Digest>(digest(d.execution)),
                       std::get<Digest>(digest(d.evidence))});
    p.profile = "media3-preview-read-v1";
    p.fixtures = d.evidence.fixtures;
    p.conversionVersions = {"cpu-planes-v2"};
    p.determinism = d.execution.implementation == Implementation::Hardware
                        ? MediaDeterminismV1::NoDeterminismClaim
                        : MediaDeterminismV1::DecodedSemanticExact;
    p.reopenPolicy = "read-only";
    p.qcProfile = "media3-generated-read-v1";
    p.result = d.evidence.result;
    if (d.capability.purpose == Purpose::Export) {
        p.purpose = Purpose::Export;
        p.profile = "media4-export-component-v1";
        EncodeSettingsV1 settings;
        settings.videoCodec = d.capability.role == Role::AudioEncode ? "" : d.capability.codec;
        settings.audioCodec = d.capability.role == Role::AudioEncode ? d.capability.codec : "";
        p.determinism = d.execution.implementation == Implementation::Hardware
                            ? MediaDeterminismV1::NoDeterminismClaim
                            : encodeDeterminism(settings);
        p.toleranceProfile = encodeTolerance(settings);
        p.reopenPolicy = "same-provider-not-independent";
        p.qcProfile = "first-last-layout-time-pcm-v1";
    }
    return p;
}
} // namespace bloom::media::provider
