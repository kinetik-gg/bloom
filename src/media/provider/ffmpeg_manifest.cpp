#include <array>
#include <bloom/media/provider/ffmpeg_manifest.hpp>

namespace bloom::media::provider {
Handshake ffmpegHandshake(bool hardware) {
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
    for (const auto* codec : {"h264", "hevc", "prores", "dnxhd", "mjpeg", "tiff"})
        if (!hardware || std::string_view(codec) == "h264" || std::string_view(codec) == "hevc")
            add(Role::VideoDecode, codec);
    if (!hardware)
        for (const auto* codec :
             {"aac", "mp3", "pcm_s16le", "pcm_s16be", "pcm_s24le", "pcm_s24be", "pcm_s32le",
              "pcm_s32be", "pcm_f32le", "pcm_f32be", "pcm_f64le", "pcm_f64be", "pcm_u8", "pcm_s8"})
            add(Role::AudioDecode, codec);
    h.transports = {Transport::PipeCopiesV0};
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
    return p;
}
} // namespace bloom::media::provider
