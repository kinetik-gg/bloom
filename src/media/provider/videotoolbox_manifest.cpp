#include <bloom/media/provider/videotoolbox_manifest.hpp>

namespace bloom::media::provider {

Handshake videoToolboxHandshake(const bool hardwareEncoder) {
    Handshake handshake;
    auto& execution = handshake.execution;
    execution.provider = "bloom.videotoolbox";
    execution.build = BLOOM_VIDEOTOOLBOX_BUILD_ID;
    if (const auto lock = Digest::fromLowercaseHex(BLOOM_MEDIA_DEPENDENCY_LOCK))
        execution.dependencyLock = *lock;
    execution.os = BLOOM_MEDIA_OS;
    execution.architecture = BLOOM_MEDIA_ARCH;
    execution.sdk = "VideoToolbox";
    execution.driver = "videotoolbox";
    execution.device = "apple-soc";
    execution.implementation = Implementation::Hardware;
    execution.synchronization = "pipe-order";
    execution.resourceProfile = "media-v1";
    execution.entitlement = hardwareEncoder ? "apple_authorized=false;delivery_qualified=false"
                                            : "apple_authorized=false;delivery_qualified=false";
    execution.trustDomain = "apple-videotoolbox";

    QualificationEvidenceV1 evidence;
    if (const auto fixtures = Digest::fromLowercaseHex(BLOOM_VIDEOTOOLBOX_FIXTURE_ID))
        evidence.fixtures = *fixtures;
    evidence.reviewDate = "2026-09-19";
    evidence.qualification = Qualification::PreviewQualified;
    evidence.result = QcResult::Pass;

    auto add = [&](Role role, const char* codec) -> ProviderDeclaration& {
        MediaCapabilityKeyV1 capability;
        capability.role = role;
        capability.container = "apple-demuxers-v1";
        capability.mapping = role == Role::Probe ? "bounded-discovery-v1" : "stream-selection-v1";
        capability.codec = codec;
        capability.profile = "stream-declared";
        capability.level = capability.tier = capability.sampleEntry = "stream-declared";
        capability.bitDepth = 16;
        capability.range = capability.chroma = capability.chromaLocation = "stream-declared";
        capability.alpha = "preserve";
        capability.fieldMode = "progressive";
        capability.maxWidth = capability.maxHeight = Limits::dimension;
        capability.maxRate = {1000, 1};
        capability.audioFormat = "planar-float";
        capability.channelLayout = "stream-declared";
        capability.colorFeatures = "h273-original";
        capability.hdrFeatures = "metadata-only";
        capability.timecodeFeatures = "original-tag";
        capability.metadataFeatures = "duration-rate-timebase";
        capability.timing = "presentation-order-exact";
        capability.surfaceSemantics = "cpu-planes-v2";
        handshake.declarations.push_back({std::move(capability), execution, evidence});
        return handshake.declarations.back();
    };

    add(Role::Probe, "intake-discovery");
    add(Role::DemuxIndex, "intake-discovery");
    for (const auto* codec : {"h264", "hevc", "prores"})
        add(Role::VideoDecode, codec);
    for (const auto* codec : {"aac", "pcm_s16le", "pcm_s24le", "pcm_f32le"})
        add(Role::AudioDecode, codec);

    // Encode roles are declared for exactly what the AVAssetWriter provider implements: ProRes and
    // H.264 video and PCM/AAC audio in MOV/MP4 containers. WAV, MXF and TIFF are deliberately
    // absent.
    auto encode = [&](Role role, const char* codec, const char* container, const char* profile) {
        auto& declaration = add(role, codec);
        declaration.capability.purpose = Purpose::Export;
        declaration.capability.container = container;
        declaration.capability.profile = profile;
        declaration.capability.mapping = "bounded-encode-v1";
        declaration.capability.surfaceSemantics = "rgba16-srgb-pcm-v1";
        declaration.evidence.qualification = Qualification::Development;
        if (const auto fixtures = Digest::fromLowercaseHex(BLOOM_VIDEOTOOLBOX_ENCODE_FIXTURE_ID))
            declaration.evidence.fixtures = *fixtures;
        return &declaration;
    };
    for (const auto* profile : {"proxy", "lt", "422", "hq", "4444", "4444xq"}) {
        auto* declaration = encode(Role::VideoEncode, "prores", "mov", profile);
        declaration->capability.bitDepth = 10;
        declaration->capability.chroma = "422";
        declaration->capability.range = "full";
        declaration->capability.colorFeatures = "rec709-display-referred";
    }
    {
        auto* declaration = encode(Role::VideoEncode, "h264", "mov", "high");
        declaration->capability.bitDepth = 8;
        declaration->capability.chroma = "yuv420p";
        declaration->capability.range = "limited";
        declaration->capability.colorFeatures = "rec709-display-referred";
    }
    for (const auto* codec : {"pcm_s16le", "pcm_s24le", "aac"})
        encode(Role::AudioEncode, codec, "apple-muxers-v1", "source-rate");
    for (const auto* container : {"mov", "mp4"}) {
        encode(Role::Mux, "apple-encoders-v1", container, "closed-stream-layout-v1");
        encode(Role::ReopenDecode, "apple-encoders-v1", container, "first-last-pcm-v1");
    }

    handshake.transports = {Transport::PipeCopiesV0};
    return handshake;
}

PipelineQualificationV1 videoToolboxPipeline(const ProviderDeclaration& declaration) {
    PipelineQualificationV1 pipeline;
    pipeline.steps.push_back({std::get<Digest>(digest(declaration.capability)),
                              std::get<Digest>(digest(declaration.execution)),
                              std::get<Digest>(digest(declaration.evidence))});
    pipeline.profile = "media3-preview-read-v1";
    pipeline.fixtures = declaration.evidence.fixtures;
    pipeline.conversionVersions = {"cpu-planes-v2"};
    pipeline.determinism = MediaDeterminismV1::NoDeterminismClaim;
    pipeline.reopenPolicy = "read-only";
    pipeline.qcProfile = "media3-generated-read-v1";
    pipeline.result = QcResult::Pass;
    if (declaration.capability.purpose == Purpose::Export) {
        pipeline.purpose = Purpose::Export;
        pipeline.profile = "media4-export-component-v1";
        EncodeSettingsV1 settings;
        settings.videoCodec =
            declaration.capability.role == Role::AudioEncode ? "" : declaration.capability.codec;
        settings.audioCodec =
            declaration.capability.role == Role::AudioEncode ? declaration.capability.codec : "";
        // A tolerance-class pipeline requires its immutable tolerance digest; hardware codecs are
        // always tolerance-class, never exact.
        pipeline.determinism = encodeDeterminism(settings);
        pipeline.toleranceProfile = encodeTolerance(settings);
        pipeline.reopenPolicy = "same-provider-not-independent";
        pipeline.qcProfile = "first-last-layout-time-pcm-v1";
    }
    return pipeline;
}

} // namespace bloom::media::provider
