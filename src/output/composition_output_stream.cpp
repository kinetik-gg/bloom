#include <algorithm>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/media/image.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/provider_chain.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/output/composition_output_stream.hpp>
#include <bloom/render/processing_host_storage.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

namespace bloom::output {
namespace {
using namespace media::provider;
using AudioEngine = media::audio::playback::AudioEngine;
using NullBackend = media::audio::playback::NullBackend;
void require(bool condition, const char* detail, Error code = Error::InvalidValue) {
    if (!condition)
        throw Unavailable{code, detail};
}
void checked(const std::optional<Unavailable>& error) {
    if (error)
        throw Unavailable{error->reason, error->detail};
}
template <typename T> T checked(Result<T> result) {
    if (const auto* error = std::get_if<Unavailable>(&result))
        throw Unavailable{error->reason, error->detail};
    return std::get<T>(std::move(result));
}
template <typename T> T required(std::optional<T> value, const char* message) {
    if (!value)
        throw Unavailable{Error::InvalidValue, message};
    return std::move(*value);
}
Rational fraction(std::uint64_t numerator, std::uint64_t denominator) {
    const auto gcd = std::gcd(numerator, denominator);
    return {static_cast<std::int64_t>(numerator / gcd),
            static_cast<std::int64_t>(denominator / gcd)};
}

struct AudioBackingBytes final {
    std::uint64_t allocated = 0;
    std::uint64_t initialized = 0;
};

[[nodiscard]] std::optional<AudioBackingBytes>
measuredAudioBacking(const media::audio::AudioBuffer& buffer) noexcept {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    AudioBackingBytes bytes{sizeof(buffer), sizeof(buffer)};
    const auto add = [](std::size_t count, std::size_t width, std::uint64_t& total) {
        if (count > (max - total) / width)
            return false;
        total += static_cast<std::uint64_t>(count) * static_cast<std::uint64_t>(width);
        return true;
    };
    if (!add(buffer.planes.capacity(), sizeof(std::vector<float>), bytes.allocated) ||
        !add(buffer.planes.size(), sizeof(std::vector<float>), bytes.initialized))
        return {};
    for (const auto& plane : buffer.planes) {
        if (!add(plane.capacity(), sizeof(float), bytes.allocated) ||
            !add(plane.size(), sizeof(float), bytes.initialized))
            return {};
    }
    return bytes;
}

struct OwnedAudioBuffer final {
    // The ledger reservation must retire after every buffer owner, including mixer clips.
    std::shared_ptr<ExportResourceReservationV1> reservation;
    std::shared_ptr<const media::audio::AudioBuffer> buffer;
};
} // namespace
struct CompositionOutputStreamV1::State {
    CompositionOutputSourceV1 source;
    EncodeSettingsV1 encoding;
    std::shared_ptr<ExportResourceReservationV1> queueReservation;
    platform::ProcessCancellation cancellation;
    std::unique_ptr<EncodeSessionV1> encoder;
    std::unique_ptr<AudioEngine> mixer;
    NullBackend* audioSink = nullptr;
    std::map<runtime::AudioStreamKey, OwnedAudioBuffer> audioBuffers;
    std::uint64_t samples = 0;
    bool started = false;
    State(CompositionOutputSourceV1 input, EncodeSettingsV1 settings, EncodeSessionOptionsV1 worker,
          std::shared_ptr<ExportResourceReservationV1> reservation,
          platform::ProcessCancellation cancel)
        : source(std::move(input)), encoding(std::move(settings)),
          queueReservation(std::move(reservation)), cancellation(std::move(cancel)),
          encoder(std::make_unique<EncodeSessionV1>(std::move(worker), cancellation)) {}
    ~State() {
        // AudioEngine keeps shared clip buffers. Drop those before their ledger owners.
        mixer.reset();
        audioBuffers.clear();
    }
    const EncodeSettingsV1& settings() const { return encoding; }
    void checkpoint() const {
        require(!cancellation || !cancellation(), "Composition output cancelled", Error::Cancelled);
    }
    void audio(runtime::TaskContext& context, std::uint64_t through) {
        if (settings().audioCodec.empty())
            return;
        const auto cancel = [&] {
            return (cancellation && cancellation()) || context.isCancellationRequested();
        };
        if (!mixer) {
            auto backend = std::make_unique<NullBackend>();
            audioSink = backend.get();
            mixer = std::make_unique<AudioEngine>(
                std::move(backend),
                AudioEngine::Config{settings().sampleRate, settings().channels, 16384});
            const auto origin = source.origin;
            require(!mixer->play(origin), "Offline mixer failed");
        }
        while (samples < through) {
            checkpoint();
            const auto origin = source.origin;
            const auto time =
                required(core::RationalTime::create(origin.numerator() * settings().sampleRate +
                                                        static_cast<std::int64_t>(samples) *
                                                            origin.denominator(),
                                                    origin.denominator() * settings().sampleRate),
                         "Audio sample timestamp overflow");
            const auto mix = required(runtime::CpuCompositionEvaluator{}.evaluateAudioMix(
                                          source.plan, time, context.cancellation()),
                                      "Composition audio graph is unavailable");
            std::vector<media::audio::playback::AudioClip> clips;
            clips.reserve(mix.clips.size());
            for (const auto& description : mix.clips) {
                const runtime::AudioStreamKey key{description.assetId, description.audioStream};
                auto found = audioBuffers.find(key);
                if (found == audioBuffers.end()) {
                    const auto* asset = source.snapshot.project().findAsset(description.assetId);
                    require(asset != nullptr, "Audio source asset is missing", Error::Unavailable);
                    const auto path = media::resolveImagePath(
                        asset->locator.path, asset->locator.relinkHint, source.assetBaseDirectory);
                    // On every failure path the decoder retires before its scratch reservation.
                    std::shared_ptr<ExportResourceReservationV1> retainedClaim;
                    std::shared_ptr<ExportResourceReservationV1> scratchClaim;
                    std::optional<media::video::VideoDecodeSession> decoder(std::in_place, path);
                    const auto probe = checked(decoder->probe(cancel));
                    require(probe.sourceDigest == asset->contentDigest, "Audio source changed",
                            Error::SourceChanged);
                    const auto* stream =
                        media::video::audioStreamAt(probe, description.audioStream);
                    require(stream != nullptr && stream->duration.denominator > 0,
                            "Audio stream unavailable", Error::Unavailable);
                    const auto frames = media::video::audioClipFrameCount(*stream);
                    require(frames.has_value() &&
                                *frames <= std::numeric_limits<std::uint64_t>::max() /
                                               stream->channelLayout.size(),
                            "Audio source sample count is not representable", Error::Oversized);
                    const auto decodedSampleCount = *frames * stream->channelLayout.size();
                    require(decodedSampleCount <=
                                std::numeric_limits<std::size_t>::max() / sizeof(float),
                            "Audio source exceeds addressable sample storage", Error::Oversized);
                    const auto metadata = sizeof(media::audio::AudioBuffer) +
                                          stream->channelLayout.size() * sizeof(std::vector<float>);
                    require(decodedSampleCount <=
                                (std::numeric_limits<std::uint64_t>::max() - metadata) /
                                    sizeof(float),
                            "Audio source byte count overflows", Error::Oversized);
                    const auto scratchSamples =
                        std::min<std::uint64_t>(*frames, media::provider::Limits::audioSamples) *
                        stream->channelLayout.size();
                    const auto retained = decodedSampleCount * sizeof(float) + metadata;
                    require(scratchSamples <=
                                (std::numeric_limits<std::uint64_t>::max() - retained) /
                                    sizeof(float),
                            "Audio decode scratch byte count overflows", Error::Oversized);
                    const auto scratchBytes = scratchSamples * sizeof(float);
                    if (queueReservation->processingPolicy()) {
                        ExportResourceAdmissionStatusV1 status{};
                        retainedClaim = queueReservation->reserveOwnedHostBacking(
                            retained, status, context.cancellation());
                        require(retainedClaim != nullptr, "Audio backing admission refused",
                                Error::Oversized);
                        scratchClaim = queueReservation->reserveScratch(scratchBytes, status,
                                                                        context.cancellation());
                        require(scratchClaim != nullptr, "Audio decode scratch admission refused",
                                Error::Oversized);
                    } else {
                        // Legacy callers without a processing root retain the existing ledger
                        // envelope and cannot claim physical-sample backing credit.
                        const auto total = retained + scratchBytes;
                        require(total <= std::numeric_limits<std::uint64_t>::max() -
                                             queueReservation->chargedBytes(),
                                "Audio export reservation overflows", Error::Oversized);
                        require(
                            queueReservation->expand(queueReservation->chargedBytes() + total) ==
                                ExportResourceAdmissionStatusV1::Reserved,
                            "Audio decode ledger reservation refused", Error::Oversized);
                    }
                    auto buffer = checked(media::video::decodeAudioClip(
                        *decoder, probe, decodedSampleCount, cancel, description.audioStream));
                    decoder.reset();
                    auto backing = std::make_unique<media::audio::AudioBuffer>(std::move(buffer));
                    std::shared_ptr<const media::audio::AudioBuffer> published;
                    if (retainedClaim) {
                        const auto measured = measuredAudioBacking(*backing);
                        require(measured.has_value(), "Audio backing byte count overflows",
                                Error::Oversized);
                        published = render::adoptProcessingHostBacking(
                            std::move(backing), retainedClaim->ownedHostPhase(),
                            measured->allocated, measured->initialized);
                        require(published != nullptr,
                                "Audio backing exceeds its admitted allocation", Error::Oversized);
                    } else {
                        published =
                            std::shared_ptr<const media::audio::AudioBuffer>(std::move(backing));
                    }
                    scratchClaim.reset();
                    found = audioBuffers
                                .emplace(key, OwnedAudioBuffer{std::move(retainedClaim),
                                                               std::move(published)})
                                .first;
                }
                require(found->second.buffer != nullptr, "Audio source buffer unavailable",
                        Error::Unavailable);
                clips.push_back({.buffer = found->second.buffer,
                                 .startTime = description.startTime,
                                 .level = static_cast<float>(description.level),
                                 .muted = description.muted,
                                 .solo = description.solo,
                                 .endTime = description.endTime,
                                 .mapTime = {}});
                if (!description.timeMappings.empty())
                    clips.back().mapTime = [description](core::RationalTime at) {
                        return runtime::mapAudioClipTime(description, at);
                    };
            }
            mixer->replaceClips(std::move(clips));
            const auto count =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(512, through - samples));
            audioSink->clearCaptured();
            require(!mixer->renderForTesting(count), "Offline mix failed");
            AudioBlock block;
            block.sampleRate = settings().sampleRate;
            block.channelLayout = {"FL", "FR"};
            block.pts = fraction(samples, settings().sampleRate);
            block.channels.resize(2, std::vector<float>(count));
            const auto captured = audioSink->captured();
            require(captured.size() == static_cast<std::size_t>(count) * 2U,
                    "Offline mix sample count differs");
            for (unsigned i = 0; i < count; ++i)
                for (unsigned c = 0; c < 2; ++c)
                    block.channels[c][i] = captured[static_cast<std::size_t>(i) * 2 + c];
            checked(encoder->audio(std::move(block)));
            samples += count;
        }
    }
};
CompositionOutputStreamV1::CompositionOutputStreamV1(
    CompositionOutputSourceV1 source, EncodeSettingsV1 settings, EncodeSessionOptionsV1 worker,
    std::shared_ptr<ExportResourceReservationV1> reservation, platform::ProcessCancellation cancel)
    : state_(std::make_unique<State>(std::move(source), std::move(settings), std::move(worker),
                                     std::move(reservation), std::move(cancel))) {}
CompositionOutputStreamV1::~CompositionOutputStreamV1() = default;
std::optional<Unavailable> CompositionOutputStreamV1::writeFrame(const render::Rgba32fImage& image,
                                                                 std::uint64_t frame,
                                                                 std::uint64_t samplesThrough,
                                                                 runtime::TaskContext& context) {
    try {
        auto& s = *state_;
        s.checkpoint();
        if (!s.started) {
            checked(s.encoder->begin(s.encoding));
            s.started = true;
        }
        if (!s.encoding.videoCodec.empty()) {
            auto prepared = checked(prepareMediaRgba16V1(
                image,
                fraction(frame * static_cast<std::uint64_t>(s.encoding.rate.denominator),
                         static_cast<std::uint64_t>(s.encoding.rate.numerator)),
                s.cancellation, s.source.display.get()));
            checked(s.encoder->video(std::move(prepared)));
        }
        s.audio(context, samplesThrough);
        return {};
    } catch (const Unavailable& error) {
        return error;
    }
}
std::optional<Unavailable> CompositionOutputStreamV1::writePreparedFrame(
    FrameProduct prepared, const std::uint64_t samplesThrough, runtime::TaskContext& context) {
    try {
        auto& s = *state_;
        s.checkpoint();
        if (!s.started) {
            checked(s.encoder->begin(s.encoding));
            s.started = true;
        }
        require(!s.encoding.videoCodec.empty(), "Prepared frames need a video stream");
        checked(s.encoder->video(std::move(prepared)));
        s.audio(context, samplesThrough);
        return {};
    } catch (const Unavailable& error) {
        return error;
    }
}
Result<EncodeQcV1> CompositionOutputStreamV1::finish() { return state_->encoder->finish(); }
Result<EncodedChunkV1> CompositionOutputStreamV1::read(std::uint64_t offset) {
    return state_->encoder->read(offset);
}
std::optional<Unavailable> CompositionOutputStreamV1::close() { return state_->encoder->close(); }
const Handshake& CompositionOutputStreamV1::handshake() const {
    return state_->encoder->handshake();
}
const MediaProviderProvenanceV1& CompositionOutputStreamV1::provenance() const {
    return state_->encoder->provenance();
}
Result<MediaQcEvidenceV1> makeMediaQcEvidenceV1(const MediaOutputAnalysisV1& analysis,
                                                const EncodeQcV1& qc, core::Sha256Digest approval,
                                                const Handshake* served) {
    try {
        MediaQcEvidenceV1 evidence;
        evidence.artifact = qc.artifact;
        evidence.colourTags = qc.colour;
        evidence.container = qc.container;
        evidence.artifactBytes = qc.bytes;
        evidence.snapshot = approval;
        evidence.approval = evidence.snapshot;
        evidence.preset = analysis.digest;
        const bool hardware =
            analysis.settings.videoCodec == "h264" &&
            analysis.implementationNote.find("encoder=vaapi") != std::string::npos;
        // The provider that actually served the attempt (ADR 0035); without one, the FFmpeg
        // worker's declaration for the analysed route.
        const auto hello = served != nullptr && !served->declarations.empty()
                               ? *served
                               : ffmpegHandshake(hardware);
        const bool videotoolbox = hello.execution.provider == "bloom.videotoolbox";
        evidence.execution = checked(digest(hello.execution));
        // Bind all declared ordered export components; no delivery or independent-reader claim.
        PipelineQualificationV1 pipeline;
        pipeline.purpose = Purpose::Export;
        pipeline.profile = "media4-export-v1";
        pipeline.determinism = analysis.determinism;
        pipeline.toleranceProfile = analysis.toleranceProfile;
        pipeline.reopenPolicy = "same-provider-not-independent";
        pipeline.qcProfile = "first-last-layout-time-pcm-v1";
        pipeline.conversionVersions = {"rgba16-ocio-display-v1", "audio-engine-mix-v1"};
        pipeline.result = QcResult::Pass;
        for (const auto role :
             {Role::VideoEncode, Role::AudioEncode, Role::Mux, Role::ReopenDecode}) {
            if ((role == Role::VideoEncode && analysis.settings.videoCodec.empty()) ||
                (role == Role::AudioEncode && analysis.settings.audioCodec.empty()))
                continue;
            const auto matches = [&](const auto& d, const bool delivered) {
                return d.capability.role == role &&
                       (role == Role::VideoEncode
                            ? mediaCodecFamilyV1(d.capability.codec) ==
                                      mediaCodecFamilyV1(analysis.settings.videoCodec) &&
                                  (delivered ? d.capability.bitDepth == qc.bitDepth
                                             : d.capability.profile == analysis.settings.profile)
                        : role == Role::AudioEncode
                            ? d.capability.codec == analysis.settings.audioCodec
                            : d.capability.container == analysis.settings.container);
            };
            auto found = std::ranges::find_if(hello.declarations,
                                              [&](const auto& d) { return matches(d, false); });
            // ADR 0036: when the serving encoder delivered something other than the request (the
            // bundled 8-bit Kvazaar serving HEVC Main 10), the component that served is the
            // codec's declaration at the delivered bit depth; the adjustment is in the evidence.
            if (found == hello.declarations.end() && role == Role::VideoEncode &&
                !qc.adjustment.empty())
                found = std::ranges::find_if(hello.declarations,
                                             [&](const auto& d) { return matches(d, true); });
            require(found != hello.declarations.end(), "Export component not in pinned manifest",
                    Error::IdentityMismatch);
            pipeline.steps.push_back({checked(digest(found->capability)),
                                      checked(digest(found->execution)),
                                      checked(digest(found->evidence))});
        }
        evidence.pipeline = checked(digest(pipeline));
        evidence.tool = hello.execution.provider;
        evidence.version = videotoolbox ? std::string("VideoToolbox") : hello.execution.sdk;
        evidence.profile = "media4-export-v1";
        evidence.coverage = "all frame timestamps/count; stream layout; duration; first/last "
                            "pixels; every PCM sample; staged digest";
        evidence.result = QcResult::Pass;
        evidence.frameCount = qc.frames;
        evidence.audioSamples = qc.audioSamples;
        evidence.duration = qc.duration;
        evidence.firstFrame = qc.firstFrame;
        evidence.lastFrame = qc.lastFrame;
        evidence.audioDigest = qc.audio;
        evidence.determinism = analysis.determinism;
        evidence.toleranceProfile = analysis.toleranceProfile;
        evidence.implementationNote = analysis.implementationNote;
        // The ProRes note names the implementation that actually encoded the artifact.
        if (mediaCodecFamilyV1(analysis.settings.videoCodec) == "prores")
            evidence.implementationNote =
                videotoolbox ? kProResVideoToolboxExportNote : kProResFfmpegExportNote;
        evidence.maximumError = qc.maximumError;
        evidence.meanError = qc.meanError;
        (void)checked(digest(evidence));
        return evidence;
    } catch (const Unavailable& error) {
        return error;
    }
}
} // namespace bloom::output
