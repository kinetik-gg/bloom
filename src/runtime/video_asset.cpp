#include <algorithm>
#include <bloom/runtime/video_asset.hpp>
#include <cctype>
#include <cmath>
namespace bloom::runtime::video {
namespace provider = media::provider;
provider::ProbeResult probeMetadata(const document::AssetRecord& asset) {
    provider::ProbeResult result;
    result.container = "captured-video";
    result.version = "document-1.17";
    result.sourceDigest = asset.contentDigest;
    for (const auto& stream : asset.videoStreams) {
        provider::StreamDescriptor descriptor;
        descriptor.id = stream.id;
        descriptor.kind = static_cast<provider::MediaKind>(stream.kind);
        descriptor.codec = stream.codec;
        descriptor.profile = stream.profile;
        descriptor.pixelFormat = stream.pixelFormat;
        descriptor.timecode = stream.timecode;
        descriptor.timebase = {stream.timebase.numerator(), stream.timebase.denominator()};
        descriptor.rate = {stream.framePeriod.denominator(), stream.framePeriod.numerator()};
        descriptor.duration = {stream.duration.numerator(), stream.duration.denominator()};
        descriptor.width = stream.width;
        descriptor.height = stream.height;
        descriptor.sampleRate = stream.sampleRate;
        descriptor.channelLayout = stream.channelLayout;
        descriptor.colour = {
            static_cast<std::int32_t>(stream.primaries), static_cast<std::int32_t>(stream.transfer),
            static_cast<std::int32_t>(stream.matrix), static_cast<std::int32_t>(stream.range)};
        descriptor.format = stream.pixelFormat == "yuv420p" ? provider::PixelFormat::Yuv420p8
                                                            : provider::PixelFormat::Yuva444p16;
        descriptor.frameCount = descriptor.kind == provider::MediaKind::Video ? asset.frames : 0;
        result.streams.push_back(std::move(descriptor));
    }
    return result;
}
} // namespace bloom::runtime::video
