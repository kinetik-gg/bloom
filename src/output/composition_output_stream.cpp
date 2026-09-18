#include <algorithm>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/media/image.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/output/composition_output_stream.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <cmath>
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
} // namespace
struct CompositionOutputStreamV1::State {
    CompositionOutputSourceV1 source;
    EncodeSettingsV1 encoding;
    std::shared_ptr<ExportResourceReservationV1> queueReservation;
    platform::ProcessCancellation cancellation;
    std::unique_ptr<EncodeSessionV1> encoder;
    std::unique_ptr<AudioEngine> mixer;
    NullBackend* audioSink = nullptr;
    std::map<document::AssetId, std::shared_ptr<const media::audio::AudioBuffer>> audioBuffers;
    std::uint64_t samples = 0;
    bool started = false;
    State(CompositionOutputSourceV1 input, EncodeSettingsV1 settings, EncodeSessionOptionsV1 worker,
          std::shared_ptr<ExportResourceReservationV1> reservation,
          platform::ProcessCancellation cancel)
        : source(std::move(input)), encoding(std::move(settings)),
          queueReservation(std::move(reservation)), cancellation(std::move(cancel)),
          encoder(std::make_unique<EncodeSessionV1>(std::move(worker), cancellation)) {}
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
                auto found = audioBuffers.find(description.assetId);
                if (found == audioBuffers.end()) {
                    const auto* asset = source.snapshot.project().findAsset(description.assetId);
                    require(asset != nullptr, "Audio source asset is missing", Error::Unavailable);
                    const auto path = media::resolveImagePath(
                        asset->locator.path, asset->locator.relinkHint, source.assetBaseDirectory);
                    media::video::VideoDecodeSession decoder(path);
                    const auto probe = checked(decoder.probe(cancel));
                    require(probe.sourceDigest == asset->contentDigest, "Audio source changed",
                            Error::SourceChanged);
                    const auto stream =
                        std::ranges::find(probe.streams, MediaKind::Audio, &StreamDescriptor::kind);
                    require(stream != probe.streams.end() && stream->duration.denominator > 0,
                            "Audio stream unavailable", Error::Unavailable);
                    const double count = std::ceil(
                        static_cast<double>(stream->duration.numerator) /
                        static_cast<double>(stream->duration.denominator) * stream->sampleRate);
                    require(std::isfinite(count) && count > 0 && !stream->channelLayout.empty() &&
                                count <=
                                    static_cast<double>(
                                        media::audio::AudioDecodeLimits::kDefaultSampleBudget) /
                                        static_cast<double>(stream->channelLayout.size()),
                            "Audio source exceeds export budget", Error::Oversized);
                    const auto retained = static_cast<std::uint64_t>(count) *
                                          stream->channelLayout.size() * sizeof(float);
                    require(queueReservation->expand(queueReservation->chargedBytes() + retained) ==
                                output::ExportResourceAdmissionStatusV1::Reserved,
                            "Audio decode ledger reservation refused", Error::Oversized);
                    auto buffer = checked(media::video::decodeAudioClip(decoder, probe, cancel));
                    found = audioBuffers
                                .emplace(description.assetId,
                                         std::make_shared<const media::audio::AudioBuffer>(
                                             std::move(buffer)))
                                .first;
                }
                require(found->second != nullptr, "Audio source buffer unavailable",
                        Error::Unavailable);
                clips.push_back({.buffer = found->second,
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
                s.cancellation));
            checked(s.encoder->video(std::move(prepared)));
        }
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
Result<MediaQcEvidenceV1> makeMediaQcEvidenceV1(const MediaOutputAnalysisV1& analysis,
                                                const EncodeQcV1& qc, core::Sha256Digest approval) {
    try {
        MediaQcEvidenceV1 evidence;
        evidence.artifact = qc.artifact;
        evidence.artifactBytes = qc.bytes;
        evidence.snapshot = approval;
        evidence.approval = evidence.snapshot;
        evidence.preset = analysis.digest;
        evidence.execution = checked(digest(ffmpegHandshake().execution));
        // Bind all declared ordered export components; no delivery or independent-reader claim.
        PipelineQualificationV1 pipeline;
        pipeline.purpose = Purpose::Export;
        pipeline.profile = "media4-export-v1";
        pipeline.determinism = analysis.determinism;
        pipeline.toleranceProfile = analysis.toleranceProfile;
        pipeline.reopenPolicy = "same-provider-not-independent";
        pipeline.qcProfile = "first-last-layout-time-pcm-v1";
        pipeline.conversionVersions = {"rgba16-srgb-v1", "audio-engine-mix-v1"};
        pipeline.result = QcResult::Pass;
        for (const auto role :
             {Role::VideoEncode, Role::AudioEncode, Role::Mux, Role::ReopenDecode}) {
            if ((role == Role::VideoEncode && analysis.settings.videoCodec.empty()) ||
                (role == Role::AudioEncode && analysis.settings.audioCodec.empty()))
                continue;
            const auto hello = ffmpegHandshake();
            const auto found = std::ranges::find_if(hello.declarations, [&](const auto& d) {
                return d.capability.role == role &&
                       (role == Role::VideoEncode
                            ? d.capability.codec == analysis.settings.videoCodec &&
                                  d.capability.profile == analysis.settings.profile
                        : role == Role::AudioEncode
                            ? d.capability.codec == analysis.settings.audioCodec
                            : d.capability.container == analysis.settings.container);
            });
            require(found != hello.declarations.end(), "Export component not in pinned manifest",
                    Error::IdentityMismatch);
            pipeline.steps.push_back({checked(digest(found->capability)),
                                      checked(digest(found->execution)),
                                      checked(digest(found->evidence))});
        }
        evidence.pipeline = checked(digest(pipeline));
        evidence.tool = "bloom.ffmpeg";
        evidence.version = "FFmpeg-8.1.2";
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
        evidence.maximumError = qc.maximumError;
        evidence.meanError = qc.meanError;
        (void)checked(digest(evidence));
        return evidence;
    } catch (const Unavailable& error) {
        return error;
    }
}
} // namespace bloom::output
