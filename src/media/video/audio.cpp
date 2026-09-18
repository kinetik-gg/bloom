#include <algorithm>
#include <bloom/media/video/audio.hpp>
#include <cmath>
namespace bloom::media::video {
provider::Result<audio::AudioBuffer> decodeAudioClip(VideoDecodeSession& session,
                                                     const provider::ProbeResult& probe,
                                                     const Cancel& cancel) {
    const auto stream = std::ranges::find(probe.streams, provider::MediaKind::Audio,
                                          &provider::StreamDescriptor::kind);
    if (stream == probe.streams.end())
        return provider::Unavailable{provider::Error::Unavailable, "Video has no audio stream"};
    const auto samples = static_cast<double>(stream->duration.numerator) /
                         static_cast<double>(stream->duration.denominator) * stream->sampleRate;
    if (!std::isfinite(samples) || samples <= 0 || stream->channelLayout.empty() ||
        stream->channelLayout.size() > 32 ||
        samples > static_cast<double>(audio::AudioDecodeLimits::kDefaultSampleBudget) /
                      static_cast<double>(stream->channelLayout.size()))
        return provider::Unavailable{provider::Error::Oversized,
                                     "Video audio exceeds the preview sample budget"};
    audio::AudioBuffer buffer;
    buffer.rate = stream->sampleRate;
    buffer.channels = static_cast<std::uint32_t>(stream->channelLayout.size());
    buffer.frames = static_cast<std::uint64_t>(std::llround(samples));
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
