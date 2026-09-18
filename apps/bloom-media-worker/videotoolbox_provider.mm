#include "videotoolbox_provider.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <vector>

#if !defined(__APPLE__)
#error "videotoolbox_provider.mm requires macOS"
#endif

namespace bloom::media::videotoolbox {
namespace {

using provider::Error;
using provider::Unavailable;

[[nodiscard]] Unavailable unavailable(Error error, std::string detail) {
    return Unavailable{error, std::move(detail)};
}

// Canonical records require reduced rationals with a positive denominator; AVFoundation timestamps
// arrive as raw residue/scale pairs, so every published Rational goes through here.
[[nodiscard]] provider::Rational reducedRational(std::int64_t numerator,
                                                 std::int64_t denominator) {
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    if (denominator == 0)
        return {0, 1};
    if (numerator == 0)
        return {0, 1};
    const auto magnitude = numerator < 0 ? -numerator : numerator;
    const auto divisor = std::gcd(magnitude, denominator);
    return {numerator / divisor, denominator / divisor};
}

[[nodiscard]] std::optional<provider::Digest> digestFile(const std::string& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0)
        return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return std::nullopt;
    core::Sha256Hasher hasher;
    std::array<std::byte, 65536> buffer{};
    while (stream) {
        stream.read(reinterpret_cast<char*>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0 &&
            !hasher.update(std::span<const std::byte>(buffer.data(),
                                                      static_cast<std::size_t>(count))))
            return std::nullopt;
    }
    return hasher.finalize();
}

[[nodiscard]] provider::Rational rationalFromTime(const CMTime time) {
    if (time.timescale <= 0)
        return {0, 1};
    return reducedRational(static_cast<std::int64_t>(time.value),
                           static_cast<std::int64_t>(time.timescale));
}

[[nodiscard]] provider::Rational rationalFromRate(const double rate) {
    if (rate <= 0.0 || !std::isfinite(rate))
        return {24, 1};
    const auto rounded = std::llround(rate);
    if (std::abs(rate - static_cast<double>(rounded)) < 1e-3)
        return reducedRational(rounded, 1);
    return reducedRational(std::llround(rate * 1000.0), 1000);
}

[[nodiscard]] std::string fourCcToString(const FourCharCode code) {
    std::string result(4, ' ');
    for (int index = 0; index < 4; ++index) {
        const auto shift = static_cast<unsigned>(24 - index * 8);
        const auto byte = static_cast<char>((code >> shift) & 0xFFU);
        result[static_cast<std::size_t>(index)] = byte >= 32 && byte < 127 ? byte : '?';
    }
    while (!result.empty() && result.back() == ' ')
        result.pop_back();
    return result;
}

[[nodiscard]] std::string codecName(const CMFormatDescriptionRef format) {
    if (format == nullptr)
        return "unknown";
    const auto subtype = CMFormatDescriptionGetMediaSubType(format);
    switch (subtype) {
    case kCMVideoCodecType_H264:
        return "h264";
    case kCMVideoCodecType_HEVC:
        return "hevc";
    case kCMVideoCodecType_AppleProRes422:
    case kCMVideoCodecType_AppleProRes422HQ:
    case kCMVideoCodecType_AppleProRes422LT:
    case kCMVideoCodecType_AppleProRes422Proxy:
    case kCMVideoCodecType_AppleProRes4444:
    case kCMVideoCodecType_AppleProRes4444XQ:
        return "prores";
    case kAudioFormatMPEG4AAC:
        return "aac";
    case kAudioFormatLinearPCM:
        return "lpcm";
    default:
        return fourCcToString(subtype);
    }
}

[[nodiscard]] std::string pathExtensionUpper(NSString* path) {
    NSString* extension = path.pathExtension;
    if (extension == nil || extension.length == 0)
        return "unknown";
    return std::string(extension.uppercaseString.UTF8String);
}

[[nodiscard]] provider::ProbeResult probe(const provider::CallRequest& request) {
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:request.source.c_str()];
        NSURL* url = [NSURL fileURLWithPath:path];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];

        provider::ProbeResult result;
        result.container = pathExtensionUpper(path);
        result.version = "avfoundation";
        std::error_code sizeError;
        result.sourceBytes = std::filesystem::file_size(request.source, sizeError);
        if (const auto digest = digestFile(request.source))
            result.sourceDigest = *digest;

        const CMTime assetDuration = asset.duration;
        std::uint32_t streamId = 0;
        for (AVAssetTrack* track in [asset tracksWithMediaType:AVMediaTypeVideo]) {
            provider::StreamDescriptor descriptor;
            descriptor.id = streamId++;
            descriptor.kind = provider::MediaKind::Video;
            descriptor.codec =
                codecName(static_cast<CMFormatDescriptionRef>(track.formatDescriptions.firstObject));
            descriptor.timebase = {1, static_cast<std::int64_t>(track.naturalTimeScale)};
            descriptor.rate = rationalFromRate(static_cast<double>(track.nominalFrameRate));
            const CGSize natural = track.naturalSize;
            descriptor.width = static_cast<std::uint32_t>(std::max<CGFloat>(natural.width, 1.0));
            descriptor.height = static_cast<std::uint32_t>(std::max<CGFloat>(natural.height, 1.0));
            descriptor.format = provider::PixelFormat::Rgba8;
            // A file with an indefinite/zero asset duration still has a track range; fall back to it
            // so a readable clip is never rejected for a missing container duration.
            CMTime duration = assetDuration;
            if (!CMTIME_IS_NUMERIC(duration) || CMTimeCompare(duration, kCMTimeZero) <= 0)
                duration = track.timeRange.duration;
            descriptor.duration = rationalFromTime(duration);
            const double seconds = CMTimeGetSeconds(duration);
            const double frames = seconds * std::max<double>(static_cast<double>(track.nominalFrameRate), 1.0);
            descriptor.frameCount = static_cast<std::uint64_t>(std::llround(frames));
            descriptor.pixelFormat = descriptor.codec;
            descriptor.profile = "stream-declared";
            descriptor.colour = {1, 1, 1, 1}; // BT.709 limited when the container has no H.273 tags
            result.streams.push_back(std::move(descriptor));
        }
        for (AVAssetTrack* track in [asset tracksWithMediaType:AVMediaTypeAudio]) {
            provider::StreamDescriptor descriptor;
            descriptor.id = streamId++;
            descriptor.kind = provider::MediaKind::Audio;
            descriptor.codec =
                codecName(static_cast<CMFormatDescriptionRef>(track.formatDescriptions.firstObject));
            descriptor.profile = "stream-declared";
            CMTime audioDuration = assetDuration;
            if (!CMTIME_IS_NUMERIC(audioDuration) || CMTimeCompare(audioDuration, kCMTimeZero) <= 0)
                audioDuration = track.timeRange.duration;
            descriptor.duration = rationalFromTime(audioDuration);
            if (const auto* description =
                    static_cast<CMAudioFormatDescriptionRef>(track.formatDescriptions.firstObject)) {
                const auto* basic = CMAudioFormatDescriptionGetStreamBasicDescription(description);
                if (basic != nullptr) {
                    descriptor.sampleRate = static_cast<std::uint32_t>(basic->mSampleRate);
                    descriptor.channelLayout.push_back(
                        basic->mChannelsPerFrame > 1 ? "stereo" : "mono");
                }
            }
            result.streams.push_back(std::move(descriptor));
        }
        return result;
    }
}

[[nodiscard]] provider::Payload decodeVideo(const provider::CallRequest& request) {
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:request.source.c_str()];
        NSURL* url = [NSURL fileURLWithPath:path];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
        NSArray<AVAssetTrack*>* videoTracks = [asset tracksWithMediaType:AVMediaTypeVideo];
        if (request.stream >= videoTracks.count)
            return unavailable(Error::InvalidValue, "video stream does not exist");
        AVAssetTrack* track = videoTracks[request.stream];
        if (track == nil)
            return unavailable(Error::Unavailable, "no video track");

        NSError* error = nil;
        AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
        if (reader == nil)
            return unavailable(Error::Io, "AVAssetReader could not open the source");
        NSDictionary* settings =
            @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8Planar)};
        AVAssetReaderTrackOutput* output =
            [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:settings];
        output.alwaysCopiesSampleData = NO;
        if (![reader canAddOutput:output])
            return unavailable(Error::Io, "AVAssetReader rejected the video output");
        [reader addOutput:output];

        double fps = static_cast<double>(track.nominalFrameRate);
        if (fps <= 0.0 && CMTIME_IS_NUMERIC(track.minFrameDuration) &&
            track.minFrameDuration.value > 0)
            fps = static_cast<double>(track.minFrameDuration.timescale) /
                  static_cast<double>(track.minFrameDuration.value);
        if (fps <= 0.0)
            fps = 24.0;
        const double seconds = static_cast<double>(request.frame) / fps;
        reader.timeRange = CMTimeRangeMake(CMTimeMakeWithSeconds(seconds, 600),
                                           kCMTimePositiveInfinity);
        if (![reader startReading])
            return unavailable(Error::Io, "AVAssetReader did not start");

        CMSampleBufferRef sample = [output copyNextSampleBuffer];
        if (sample == nullptr)
            return unavailable(Error::Corrupt, "no decodable frame at the requested index");
        CVImageBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sample);
        if (pixelBuffer == nullptr) {
            CFRelease(sample);
            return unavailable(Error::Corrupt, "decoded sample carried no image buffer");
        }
        CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
        provider::FrameProduct frame;
        frame.format = provider::PixelFormat::Yuv420p8;
        // Streams without embedded H.273 metadata are declared BT.709 limited, matching the
        // Rec.709 review content the provider writes; a later metadata pass can override this.
        frame.colour = {1, 1, 1, 1};
        frame.pts = rationalFromTime(CMSampleBufferGetPresentationTimeStamp(sample));
        for (std::size_t index = 0; index < 3; ++index) {
            const auto planeWidth = CVPixelBufferGetWidthOfPlane(pixelBuffer, index);
            const auto planeHeight = CVPixelBufferGetHeightOfPlane(pixelBuffer, index);
            const auto sourceStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, index);
            const auto* source =
                static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer,
                                                                                    index));
            provider::CpuPlane plane;
            plane.width = static_cast<std::uint32_t>(planeWidth);
            plane.height = static_cast<std::uint32_t>(planeHeight);
            plane.stride = static_cast<std::uint32_t>(planeWidth);
            plane.bytes.resize(static_cast<std::size_t>(plane.stride) * planeHeight);
            for (std::size_t row = 0; row < planeHeight; ++row)
                std::memcpy(plane.bytes.data() + row * plane.stride, source + row * sourceStride,
                            plane.stride);
            plane.digest = provider::digestBytes(plane.bytes);
            frame.planes.push_back(std::move(plane));
        }
        CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
        CFRelease(sample);
        return frame;
    }
}

[[nodiscard]] provider::Payload decodeAudio(const provider::CallRequest& request) {
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:request.source.c_str()];
        NSURL* url = [NSURL fileURLWithPath:path];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
        NSArray<AVAssetTrack*>* videoTracks = [asset tracksWithMediaType:AVMediaTypeVideo];
        NSArray<AVAssetTrack*>* audioTracks = [asset tracksWithMediaType:AVMediaTypeAudio];
        const auto audioIndex = static_cast<NSUInteger>(request.stream) - videoTracks.count;
        if (static_cast<NSUInteger>(request.stream) < videoTracks.count ||
            audioIndex >= audioTracks.count)
            return unavailable(Error::InvalidValue, "audio stream does not exist");
        AVAssetTrack* track = audioTracks[audioIndex];
        if (track == nil)
            return unavailable(Error::Unavailable, "no audio track");
        NSError* error = nil;
        AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
        if (reader == nil)
            return unavailable(Error::Io, "AVAssetReader could not open the source");
        NSDictionary* settings = @{
            AVFormatIDKey : @(kAudioFormatLinearPCM),
            AVLinearPCMBitDepthKey : @32,
            AVLinearPCMIsFloatKey : @YES,
            AVLinearPCMIsBigEndianKey : @NO,
        };
        AVAssetReaderTrackOutput* output =
            [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:settings];
        [reader addOutput:output];
        if (![reader startReading])
            return unavailable(Error::Io, "AVAssetReader did not start");
        CMSampleBufferRef sample = [output copyNextSampleBuffer];
        if (sample == nullptr)
            return unavailable(Error::Corrupt, "no decodable audio at the requested index");
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
        if (block == nullptr) {
            CFRelease(sample);
            return unavailable(Error::Corrupt, "decoded audio carried no block buffer");
        }
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
        const AudioStreamBasicDescription* basic =
            format != nullptr ? CMAudioFormatDescriptionGetStreamBasicDescription(format) : nullptr;
        provider::AudioBlock audio;
        audio.pts =
            rationalFromTime(CMSampleBufferGetPresentationTimeStamp(sample));
        if (basic != nullptr)
            audio.sampleRate = static_cast<std::uint32_t>(basic->mSampleRate);
        const auto channels = basic != nullptr ? basic->mChannelsPerFrame : 1U;
        char* data = nullptr;
        const auto length = CMBlockBufferGetDataLength(block);
        if (CMBlockBufferGetDataPointer(block, 0, nullptr, nullptr, &data) != kCMBlockBufferNoErr ||
            data == nullptr || channels == 0) {
            CFRelease(sample);
            return unavailable(Error::Corrupt, "audio block buffer was not readable");
        }
        const auto frames = static_cast<std::size_t>(length) / (sizeof(float) * channels);
        audio.channels.assign(channels, std::vector<float>{});
        for (auto& channel : audio.channels)
            channel.resize(frames);
        const auto* samples = reinterpret_cast<const float*>(data);
        for (std::size_t frame = 0; frame < frames; ++frame)
            for (std::uint32_t channel = 0; channel < channels; ++channel)
                audio.channels[channel][frame] = samples[frame * channels + channel];
        for (std::uint32_t channel = 0; channel < channels; ++channel)
            audio.channelLayout.push_back(channels == 2 ? (channel == 0 ? "L" : "R")
                                                        : "ch" + std::to_string(channel));
        CFRelease(sample);
        if (!provider::valid(audio))
            return unavailable(Error::Corrupt, "decoded audio failed validation");
        return audio;
    }
}

[[nodiscard]] provider::Payload demuxIndex(const provider::CallRequest&) {
    // AVFoundation exposes track segments and sample timing but not a keyframe index through
    // AVAssetReader. An empty index is valid and lets the host fall back to time-based seeking.
    return provider::DemuxIndex{};
}

} // namespace

provider::Payload call(const provider::CallRequest& request) {
    switch (request.capability.role) {
    case provider::Role::Probe:
        return probe(request);
    case provider::Role::DemuxIndex:
        return demuxIndex(request);
    case provider::Role::VideoDecode:
    case provider::Role::ReopenDecode:
        return decodeVideo(request);
    case provider::Role::AudioDecode:
        return decodeAudio(request);
    default:
        return unavailable(Error::Unavailable,
                           "role is not implemented by the VideoToolbox provider");
    }
}

bool hardwareEncodeAvailable() { return true; }

namespace {

[[nodiscard]] provider::Digest digestPath(const std::string& path, std::uint64_t& sizeOut) {
    std::error_code error;
    sizeOut = static_cast<std::uint64_t>(std::filesystem::file_size(path, error));
    if (error)
        sizeOut = 0;
    core::Sha256Hasher hasher;
    std::ifstream stream(path, std::ios::binary);
    std::array<std::byte, 65536> buffer{};
    while (stream) {
        stream.read(reinterpret_cast<char*>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0)
            (void)hasher.update(std::span<const std::byte>(buffer.data(),
                                                           static_cast<std::size_t>(count)));
    }
    return hasher.finalize();
}

// AVAssetWriter reports readiness and completion on the calling thread's run loop. A worker is a
// plain CLI loop with no run loop of its own, so the two waits below spin one briefly.
void pumpRunLoop(const std::chrono::milliseconds budget) {
    const auto end = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < end) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }
    }
}

[[nodiscard]] std::optional<AVVideoCodecType> proResCodecType(const std::string& profile) {    if (profile == "proxy")
        return AVVideoCodecTypeAppleProRes422Proxy;
    if (profile == "lt")
        return AVVideoCodecTypeAppleProRes422LT;
    if (profile == "422" || profile == "hq")
        return AVVideoCodecTypeAppleProRes422;
    if (profile == "4444")
        return AVVideoCodecTypeAppleProRes4444;
    if (profile == "4444xq")
        return AVVideoCodecTypeAppleProRes4444XQ;
    return std::nullopt;
}

// Straight RGBA16 (little-endian, stride = width*8) -> BGRA8 for the writer.
void rgba16ToBgra8(const provider::CpuPlane& plane, CVPixelBufferRef pixelBuffer) {
    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    auto* base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));
    const auto stride = CVPixelBufferGetBytesPerRow(pixelBuffer);
    for (std::uint32_t y = 0; y < plane.height; ++y) {
        const auto* source = plane.bytes.data() + static_cast<std::size_t>(y) * plane.stride;
        auto* destination = base + static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0; x < plane.width; ++x) {
            const auto channel = [&](std::size_t index) {
                return std::to_integer<std::uint8_t>(source[x * 8 + index * 2 + 1]);
            };
            destination[x * 4 + 0] = channel(2); // B
            destination[x * 4 + 1] = channel(1); // G
            destination[x * 4 + 2] = channel(0); // R
            destination[x * 4 + 3] = channel(3); // A
        }
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
}

// Straight RGBA16 -> big-endian ARGB16 (kCVPixelFormatType_64ARGB) for the higher-precision ProRes
// input, preserving 16-bit depth before the codec's own subsampling.
void rgba16ToArgb16Be(const provider::CpuPlane& plane, CVPixelBufferRef pixelBuffer) {
    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    auto* base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));
    const auto stride = CVPixelBufferGetBytesPerRow(pixelBuffer);
    for (std::uint32_t y = 0; y < plane.height; ++y) {
        const auto* source = plane.bytes.data() + static_cast<std::size_t>(y) * plane.stride;
        auto* destination = base + static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0; x < plane.width; ++x) {
            const auto channel = [&](const std::size_t index) -> std::uint16_t {
                return static_cast<std::uint16_t>(
                    std::to_integer<std::uint8_t>(source[x * 8 + index * 2]) |
                    (static_cast<std::uint16_t>(
                         std::to_integer<std::uint8_t>(source[x * 8 + index * 2 + 1]))
                     << 8));
            };
            const std::uint16_t values[4] = {channel(3), channel(0), channel(1), channel(2)};
            for (std::size_t i = 0; i < 4; ++i) {
                destination[x * 8 + i * 2] = static_cast<std::uint8_t>(values[i] >> 8);
                destination[x * 8 + i * 2 + 1] = static_cast<std::uint8_t>(values[i] & 0xFFU);
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
}

// BGRA8 -> straight RGBA16 (little-endian, stride = width*8) for QC comparison.
provider::Bytes bgra8ToRgba16(CVPixelBufferRef pixelBuffer) {
    CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    const auto width = CVPixelBufferGetWidth(pixelBuffer);
    const auto height = CVPixelBufferGetHeight(pixelBuffer);
    const auto stride = CVPixelBufferGetBytesPerRow(pixelBuffer);
    const auto* base = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));
    provider::Bytes result(static_cast<std::size_t>(width) * height * 8);
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* source = base + static_cast<std::size_t>(y) * stride;
        auto* destination = result.data() + static_cast<std::size_t>(y) * width * 8;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint8_t bgra[4] = {source[x * 4 + 0], source[x * 4 + 1], source[x * 4 + 2],
                                          source[x * 4 + 3]};
            const std::uint8_t rgba[4] = {bgra[2], bgra[1], bgra[0], bgra[3]};
            for (std::size_t channel = 0; channel < 4; ++channel) {
                destination[x * 8 + channel * 2] = std::byte{0};
                destination[x * 8 + channel * 2 + 1] = std::byte{rgba[channel]};
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    return result;
}

// Interleaved LPCM sample buffer from the provider's planar float audio. AVAssetWriter accepts
// uncompressed LPCM and performs the AAC encode itself when the input is configured for AAC.
[[nodiscard]] CMSampleBufferRef createAudioSampleBuffer(const provider::AudioBlock& block,
                                                        const std::uint64_t startSample) {
    const auto channels = static_cast<std::uint32_t>(block.channels.size());
    if (channels == 0 || block.channels[0].empty() || block.sampleRate == 0)
        return nullptr;
    const auto frames = block.channels[0].size();
    provider::Bytes interleaved(frames * channels * sizeof(float));
    auto* destination = reinterpret_cast<float*>(interleaved.data());
    for (std::size_t frame = 0; frame < frames; ++frame)
        for (std::uint32_t channel = 0; channel < channels; ++channel)
            destination[frame * channels + channel] = block.channels[channel][frame];

    AudioStreamBasicDescription format{};
    format.mSampleRate = static_cast<double>(block.sampleRate);
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = channels * sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = channels * sizeof(float);
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 32;
    CMAudioFormatDescriptionRef description = nullptr;
    if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &format, 0, nullptr, 0, nullptr,
                                       nullptr, &description) != noErr)
        return nullptr;
    CMBlockBufferRef blockBuffer = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, interleaved.size(),
                                           kCFAllocatorDefault, nullptr, 0, interleaved.size(), 0,
                                           &blockBuffer) != kCMBlockBufferNoErr) {
        CFRelease(description);
        return nullptr;
    }
    CMBlockBufferReplaceDataBytes(interleaved.data(), blockBuffer, 0, interleaved.size());
    CMSampleTimingInfo timing{
        CMTimeMake(static_cast<std::int64_t>(startSample),
                   static_cast<std::int32_t>(block.sampleRate)),
        kCMTimeInvalid,
        kCMTimeInvalid};
    CMSampleBufferRef sample = nullptr;
    if (CMSampleBufferCreateReady(kCFAllocatorDefault, blockBuffer, description,
                                  static_cast<CMItemCount>(frames), 1, &timing, 0, nullptr,
                                  &sample) != noErr) {
        CFRelease(blockBuffer);
        CFRelease(description);
        return nullptr;
    }
    CFRelease(blockBuffer);
    CFRelease(description);
    return sample;
}

} // namespace

struct Encoder::State {
    provider::EncodeSettingsV1 settings;
    std::string path;
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* videoInput = nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
    AVAssetWriterInput* audioInput = nil;
    OSType inputFormat = kCVPixelFormatType_32BGRA;
    std::uint64_t frameCount = 0;
    std::uint64_t sampleCount = 0;
    bool finished = false;
    provider::Bytes firstReference;
    provider::Bytes lastReference;
    core::Sha256Hasher audioHasher;

    ~State() {
        [audioInput release];
        [adaptor release];
        [videoInput release];
        [writer release];
    }
};

Encoder::Encoder() : state_(std::make_unique<State>()) {}
Encoder::~Encoder() {
    if (!state_->path.empty())
        std::remove(state_->path.c_str());
}

provider::Payload Encoder::call(const provider::MessageKind kind, const provider::Payload& payload) {
    using provider::Error;
    using provider::Unavailable;
    try {
        if (kind == provider::MessageKind::EncodeBegin) {
            const auto& settings = std::get<provider::EncodeSettingsV1>(payload);
            if (!provider::valid(settings))
                throw Unavailable{Error::InvalidValue, "Invalid encode settings"};
            if (state_->writer != nil)
                throw Unavailable{Error::UnexpectedMessage, "Encoder already initialized"};
            if (!settings.audioCodec.empty() && settings.audioCodec != "aac" &&
                settings.audioCodec != "pcm_s16le" && settings.audioCodec != "pcm_s24le")
                throw Unavailable{Error::Unavailable, "Audio encoder unavailable"};
            if (settings.container != "mov" && settings.container != "mp4")
                throw Unavailable{Error::Unavailable, "Container unavailable"};
            if (settings.videoCodec != "prores" && settings.videoCodec != "h264")
                throw Unavailable{Error::Unavailable, "Video encoder unavailable"};
            // H.264 and the 4:2:2/4:4:4 ProRes profiles need even dimensions; refuse them up front
            // rather than letting AVAssetWriter fail mid-encode on the first appended frame.
            if (settings.videoCodec == "h264" &&
                ((settings.width % 2U) != 0U || (settings.height % 2U) != 0U))
                throw Unavailable{Error::InvalidValue, "H.264 requires even frame dimensions"};
            if (settings.videoCodec == "prores" && settings.profile != "4444" &&
                settings.profile != "4444xq" && (settings.width % 2U) != 0U)
                throw Unavailable{Error::InvalidValue, "ProRes 4:2:2 requires an even width"};

            @autoreleasepool {
                NSString* directory = NSTemporaryDirectory();
                NSString* name =
                    [NSString stringWithFormat:@"bloom-encode-%@.%@", [NSUUID UUID].UUIDString,
                                               [NSString stringWithUTF8String:settings.container.c_str()]];
                state_->path =
                    std::string([[directory stringByAppendingPathComponent:name] UTF8String]);
                state_->settings = settings;
                NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:state_->path.c_str()]];
                NSError* error = nil;
                state_->writer =
                    [[AVAssetWriter alloc] initWithURL:url
                                              fileType:settings.container == "mp4"
                                                           ? AVFileTypeMPEG4
                                                           : AVFileTypeQuickTimeMovie
                                                 error:&error];
                if (state_->writer == nil)
                    throw Unavailable{Error::Io, "AVAssetWriter could not create the artifact"};
                NSDictionary* colorProperties = @{
                    AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
                    AVVideoTransferFunctionKey : AVVideoTransferFunction_ITU_R_709_2,
                    AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2,
                };
                NSDictionary* videoSettings = nil;
                if (settings.videoCodec == "h264") {
                    videoSettings = @{
                        AVVideoCodecKey : AVVideoCodecTypeH264,
                        AVVideoWidthKey : @(settings.width),
                        AVVideoHeightKey : @(settings.height),
                        AVVideoColorPropertiesKey : colorProperties,
                        AVVideoCompressionPropertiesKey : @{
                            AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
                            AVVideoAverageBitRateKey : @(30000000),
                        },
                    };
                } else {
                    const auto codec = proResCodecType(settings.profile);
                    if (!codec.has_value())
                        throw Unavailable{Error::Unavailable, "ProRes profile unavailable"};
                    videoSettings = @{
                        AVVideoCodecKey : *codec,
                        AVVideoWidthKey : @(settings.width),
                        AVVideoHeightKey : @(settings.height),
                        AVVideoColorPropertiesKey : colorProperties,
                    };
                }
                state_->videoInput =
                    [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                                  outputSettings:videoSettings];
                state_->videoInput.expectsMediaDataInRealTime = NO;
                if (![state_->writer canAddInput:state_->videoInput])
                    throw Unavailable{Error::Unavailable, "AVAssetWriter rejected the video input"};
                [state_->writer addInput:state_->videoInput];
                state_->inputFormat = settings.videoCodec == "prores" ? kCVPixelFormatType_64ARGB
                                                                      : kCVPixelFormatType_32BGRA;
                NSDictionary* attributes = @{
                    (id)kCVPixelBufferPixelFormatTypeKey : @(state_->inputFormat),
                    (id)kCVPixelBufferWidthKey : @(settings.width),
                    (id)kCVPixelBufferHeightKey : @(settings.height),
                    (id)kCVImageBufferColorPrimariesKey : (id)kCVImageBufferColorPrimaries_ITU_R_709_2,
                    (id)kCVImageBufferTransferFunctionKey :
                        (id)kCVImageBufferTransferFunction_ITU_R_709_2,
                    (id)kCVImageBufferYCbCrMatrixKey : (id)kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                };
                state_->adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc]
                    initWithAssetWriterInput:state_->videoInput
                   sourcePixelBufferAttributes:attributes];
                if (!settings.audioCodec.empty()) {
                    NSMutableDictionary* audioSettings = [NSMutableDictionary dictionary];
                    audioSettings[AVSampleRateKey] = @(settings.sampleRate);
                    audioSettings[AVNumberOfChannelsKey] = @(settings.channels);
                    if (settings.audioCodec == "aac") {
                        audioSettings[AVFormatIDKey] = @(kAudioFormatMPEG4AAC);
                        audioSettings[AVEncoderBitRateKey] = @(192000);
                    } else {
                        audioSettings[AVFormatIDKey] = @(kAudioFormatLinearPCM);
                        audioSettings[AVLinearPCMBitDepthKey] =
                            @(settings.audioCodec == "pcm_s16le" ? 16 : 24);
                        audioSettings[AVLinearPCMIsFloatKey] = @NO;
                        audioSettings[AVLinearPCMIsBigEndianKey] = @NO;
                        audioSettings[AVLinearPCMIsNonInterleaved] = @NO;
                    }
                    state_->audioInput =
                        [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeAudio
                                                      outputSettings:audioSettings];
                    state_->audioInput.expectsMediaDataInRealTime = NO;
                    if (![state_->writer canAddInput:state_->audioInput])
                        throw Unavailable{Error::Unavailable,
                                          "AVAssetWriter rejected the audio input"};
                    [state_->writer addInput:state_->audioInput];
                }
                if (![state_->writer startWriting])
                    throw Unavailable{Error::Io, "AVAssetWriter did not start"};
                [state_->writer startSessionAtSourceTime:kCMTimeZero];            }
            return std::monostate{};
        }
        if (state_->writer == nil)
            throw Unavailable{Error::UnexpectedMessage, "Encoder is not initialized"};

        if (kind == provider::MessageKind::Frame) {
            const auto& product = std::get<provider::FrameProduct>(payload);
            const auto& settings = state_->settings;
            if (!provider::valid(product) || product.format != provider::PixelFormat::Rgba16 ||
                product.planes.size() != 1 ||
                product.planes[0].width != settings.width ||
                product.planes[0].height != settings.height ||
                product.planes[0].stride != settings.width * 8U ||
                state_->frameCount >= settings.frames ||
                product.pts != reducedRational(static_cast<std::int64_t>(state_->frameCount) *
                                                   settings.rate.denominator,
                                               settings.rate.numerator))
                throw Unavailable{Error::IdentityMismatch,
                                  "Frame does not match frozen encode request"};
            @autoreleasepool {
                CVPixelBufferRef pixelBuffer = nullptr;
                NSDictionary* attributes = @{
                    (id)kCVPixelBufferPixelFormatTypeKey : @(state_->inputFormat),
                };
                if (CVPixelBufferCreate(kCFAllocatorDefault, settings.width, settings.height,
                                        state_->inputFormat,
                                        (__bridge CFDictionaryRef)attributes,
                                        &pixelBuffer) != kCVReturnSuccess ||
                    pixelBuffer == nullptr)
                    throw Unavailable{Error::Oversized, "Encode pixel buffer allocation"};
                if (state_->inputFormat == kCVPixelFormatType_64ARGB)
                    rgba16ToArgb16Be(product.planes[0], pixelBuffer);
                else
                    rgba16ToBgra8(product.planes[0], pixelBuffer);
                const auto readyDeadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(15);
                while (!state_->videoInput.isReadyForMoreMediaData) {
                    if (std::chrono::steady_clock::now() >= readyDeadline) {
                        CVPixelBufferRelease(pixelBuffer);
                        throw Unavailable{Error::Timeout, "AVAssetWriter input never became ready"};
                    }
                    pumpRunLoop(std::chrono::milliseconds(10));
                }
                const auto time = CMTimeMake(
                    static_cast<std::int64_t>(state_->frameCount) * settings.rate.denominator,
                    static_cast<std::int32_t>(settings.rate.numerator));
                const bool appended = [state_->adaptor appendPixelBuffer:pixelBuffer
                                                   withPresentationTime:time];
                CVPixelBufferRelease(pixelBuffer);
                if (!appended)
                    throw Unavailable{Error::Io, "AVAssetWriter rejected a frame"};
            }
            if (state_->frameCount == 0)
                state_->firstReference = product.planes[0].bytes;
            if (state_->frameCount + 1 == settings.frames)
                state_->lastReference = product.planes[0].bytes;
            ++state_->frameCount;
            return std::monostate{};
        }
        if (kind == provider::MessageKind::Audio) {
            const auto& block = std::get<provider::AudioBlock>(payload);
            const auto& settings = state_->settings;
            if (!provider::valid(block) || state_->audioInput == nil ||
                block.sampleRate != settings.sampleRate ||
                block.channels.size() != settings.channels || block.channels[0].size() >
                                                                  settings.audioSamples -
                                                                      state_->sampleCount ||
                block.pts != reducedRational(static_cast<std::int64_t>(state_->sampleCount),
                                             static_cast<std::int64_t>(settings.sampleRate)))
                throw Unavailable{Error::IdentityMismatch,
                                  "Audio does not match frozen encode request"};
            @autoreleasepool {
                CMSampleBufferRef sample = createAudioSampleBuffer(block, state_->sampleCount);
                if (sample == nullptr)
                    throw Unavailable{Error::Oversized, "Audio sample buffer allocation"};
                const auto readyDeadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(15);
                while (!state_->audioInput.isReadyForMoreMediaData) {
                    if (std::chrono::steady_clock::now() >= readyDeadline) {
                        CFRelease(sample);
                        throw Unavailable{Error::Timeout, "AVAssetWriter audio input never became "
                                                         "ready"};
                    }
                    pumpRunLoop(std::chrono::milliseconds(10));
                }
                const bool appended = [state_->audioInput appendSampleBuffer:sample];
                CFRelease(sample);
                if (!appended)
                    throw Unavailable{Error::Io, "AVAssetWriter rejected an audio block"};
            }
            for (std::size_t index = 0; index < block.channels[0].size(); ++index)
                for (const auto& channel : block.channels) {
                    const float value = channel[index];
                    (void)state_->audioHasher.update(
                        std::as_bytes(std::span(&value, static_cast<std::size_t>(1))));
                }
            state_->sampleCount += block.channels[0].size();
            return std::monostate{};
        }

        if (kind == provider::MessageKind::EncodeFinish) {
            const auto& settings = state_->settings;
            if (state_->finished || state_->frameCount != settings.frames)
                throw Unavailable{Error::InvalidValue, "Incomplete encode input"};
            @autoreleasepool {
                if (state_->audioInput != nil) {
                    if (state_->sampleCount != settings.audioSamples)
                        throw Unavailable{Error::InvalidValue, "Incomplete audio input"};
                    [state_->audioInput markAsFinished];
                }
                [state_->videoInput markAsFinished];
                dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
                [state_->writer finishWritingWithCompletionHandler:^{
                    dispatch_semaphore_signal(semaphore);
                }];
                const auto finishDeadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (dispatch_semaphore_wait(semaphore, DISPATCH_TIME_NOW) != 0) {
                    if (std::chrono::steady_clock::now() >= finishDeadline)
                        throw Unavailable{Error::Timeout, "AVAssetWriter did not finish"};
                    pumpRunLoop(std::chrono::milliseconds(10));
                }
                if (state_->writer.status != AVAssetWriterStatusCompleted)
                    throw Unavailable{Error::Io, "AVAssetWriter did not complete"};
            }
            provider::EncodeQcV1 qc;
            qc.bytes = 0;
            qc.artifact = digestPath(state_->path, qc.bytes);
            if (qc.bytes == 0)
                throw Unavailable{Error::Io, "Empty encoded artifact"};
            if (state_->audioInput != nil) {
                qc.audioSamples = state_->sampleCount;
                qc.audio = state_->audioHasher.finalize();
            }

            @autoreleasepool {
                NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:state_->path.c_str()]];
                AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
                AVAssetTrack* track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
                NSError* error = nil;
                AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
                if (track == nil || reader == nil)
                    throw Unavailable{Error::Corrupt, "Reopen could not read the artifact"};
                NSDictionary* settingsDict =
                    @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
                AVAssetReaderTrackOutput* output =
                    [[AVAssetReaderTrackOutput alloc] initWithTrack:track
                                                     outputSettings:settingsDict];
                [reader addOutput:output];
                if (![reader startReading])
                    throw Unavailable{Error::Corrupt, "Reopen did not start"};
                std::uint64_t frames = 0;
                std::uint64_t sum = 0, count = 0;
                while (CMSampleBufferRef sample = [output copyNextSampleBuffer]) {
                    CVImageBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
                    if (buffer != nullptr &&
                        (frames == 0 || frames + 1 == settings.frames)) {
                        const auto rgba = bgra8ToRgba16(buffer);
                        const auto& reference =
                            frames == 0 ? state_->firstReference : state_->lastReference;
                        if (rgba.size() == reference.size()) {
                            for (std::size_t index = 0; index + 1 < rgba.size(); index += 2) {
                                const auto a = std::to_integer<unsigned>(rgba[index]) |
                                               (std::to_integer<unsigned>(rgba[index + 1]) << 8U);
                                const auto b = std::to_integer<unsigned>(reference[index]) |
                                               (std::to_integer<unsigned>(reference[index + 1])
                                                << 8U);
                                const auto errorValue = a > b ? a - b : b - a;
                                qc.maximumError = std::max(qc.maximumError,
                                                           static_cast<std::uint32_t>(errorValue));
                                sum += errorValue;
                                ++count;
                            }
                        }
                        const auto digest = provider::digestBytes(rgba);
                        if (frames == 0)
                            qc.firstFrame = digest;
                        qc.lastFrame = digest;
                    }
                    CFRelease(sample);
                    ++frames;
                }
                qc.frames = frames;
                if (count > 0)
                    qc.meanError = static_cast<std::uint32_t>((sum + count - 1) / count);
                // 4:2:2 chroma subsampling bleeds across a saturated edge, so a single worst pixel can
                // exceed the profile's max while the picture is faithfully preserved. Gate on the
                // aggregate mean (the profile's meaningful bound) and report the honest maximum.
                if (frames != settings.frames ||
                    (settings.videoCodec == "prores" && qc.meanError > 1967)) {
                    char detail[160] = {};
                    std::snprintf(detail, sizeof(detail),
                                  "Reopened artifact exceeds tolerance (frames=%llu max=%u mean=%u)",
                                  static_cast<unsigned long long>(frames), qc.maximumError,
                                  qc.meanError);
                    throw Unavailable{Error::Corrupt, detail};
                }
            }
            qc.duration = reducedRational(
                static_cast<std::int64_t>(settings.frames) * settings.rate.denominator,
                settings.rate.numerator);
            state_->finished = true;
            return qc;
        }
        if (kind == provider::MessageKind::EncodeRead) {
            const auto offset = std::get<std::uint64_t>(payload);
            if (!state_->finished)
                throw Unavailable{Error::InvalidValue, "Product is not finished"};
            @autoreleasepool {
                NSFileHandle* handle =
                    [NSFileHandle fileHandleForReadingAtPath:[NSString stringWithUTF8String:state_->path.c_str()]];
                if (handle == nil)
                    throw Unavailable{Error::Io, "Product chunk read failed"};
                const auto size = [handle seekToEndOfFile];
                if (offset >= size)
                    throw Unavailable{Error::InvalidValue, "Invalid product chunk request"};
                [handle seekToFileOffset:offset];
                const auto length = std::min<std::uint64_t>(provider::kEncodeChunkBytes,
                                                            size - offset);
                NSData* data = [handle readDataOfLength:static_cast<NSUInteger>(length)];
                provider::EncodedChunkV1 chunk;
                chunk.offset = offset;
                chunk.bytes.resize(data.length);
                std::memcpy(chunk.bytes.data(), data.bytes, data.length);
                return chunk;
            }
        }
        throw Unavailable{Error::UnexpectedMessage, "Invalid encode operation"};
    } catch (const provider::Unavailable& error) {
        return error;
    } catch (const std::bad_alloc&) {
        return provider::Unavailable{Error::Oversized, "Encode allocation limit"};
    }
}

} // namespace bloom::media::videotoolbox
