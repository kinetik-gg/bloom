#include <algorithm>
#include <bloom/media/video/audio.hpp>
#include <cmath>
#include <limits>
namespace bloom::media::video {
std::optional<std::uint64_t>
audioClipFrameCount(const provider::StreamDescriptor& stream) noexcept {
    if (stream.duration.numerator <= 0 || stream.duration.denominator <= 0 ||
        stream.sampleRate == 0 || stream.channelLayout.empty() ||
        stream.channelLayout.size() > provider::Limits::channels)
        return {};
    const auto samples = static_cast<long double>(stream.duration.numerator) /
                         static_cast<long double>(stream.duration.denominator) * stream.sampleRate;
    const auto ceiling = static_cast<long double>(std::min<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::size_t>::max()));
    if (!std::isfinite(samples) || samples <= 0.0L || samples >= ceiling)
        return {};
    // A positive fractional tail is still an audible frame in range/export playback.
    return static_cast<std::uint64_t>(std::ceil(samples));
}
const provider::StreamDescriptor* audioStreamAt(const provider::ProbeResult& probe,
                                                const std::uint32_t ordinal) noexcept {
    std::uint32_t seen = 0;
    for (const auto& stream : probe.streams)
        if (stream.kind == provider::MediaKind::Audio && seen++ == ordinal)
            return &stream;
    return nullptr;
}
provider::Result<audio::AudioBuffer> decodeAudioClip(VideoDecodeSession& session,
                                                     const provider::ProbeResult& probe,
                                                     const std::uint64_t admittedSamples,
                                                     const Cancel& cancel,
                                                     const std::uint32_t audioStream) {
    const auto* stream = audioStreamAt(probe, audioStream);
    if (stream == nullptr)
        return provider::Unavailable{provider::Error::Unavailable,
                                     audioStream == 0 ? "Video has no audio stream"
                                                      : "The selected audio stream does not exist"};
    const auto frames = audioClipFrameCount(*stream);
    if (!frames || *frames > admittedSamples / stream->channelLayout.size() ||
        *frames > std::numeric_limits<std::size_t>::max() /
                      (stream->channelLayout.size() * sizeof(float)))
        return provider::Unavailable{provider::Error::Oversized,
                                     "Video audio exceeds its admitted sample storage"};
    audio::AudioBuffer buffer;
    buffer.rate = stream->sampleRate;
    buffer.channels = static_cast<std::uint32_t>(stream->channelLayout.size());
    buffer.frames = *frames;
    buffer.planes.resize(buffer.channels);
    for (auto& channel : buffer.planes)
        channel.reserve(static_cast<std::size_t>(buffer.frames));
    for (std::uint64_t start = 0; start < buffer.frames;) {
        const auto count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(provider::Limits::audioSamples, buffer.frames - start));
        auto decoded = session.audio(probe, stream->id, start, count, cancel);
        if (const auto* error = std::get_if<provider::Unavailable>(&decoded))
            return *error;
        auto& block = std::get<provider::AudioBlock>(decoded);
        if (block.sampleRate != buffer.rate || block.channels.size() != buffer.channels ||
            block.channels.front().size() != count)
            return provider::Unavailable{provider::Error::IdentityMismatch,
                                         "Video audio descriptor changed"};
        for (std::size_t channel = 0; channel < block.channels.size(); ++channel)
            buffer.planes[channel].insert(buffer.planes[channel].end(),
                                          block.channels[channel].begin(),
                                          block.channels[channel].end());
        start += count;
    }
    return buffer;
}
} // namespace bloom::media::video
