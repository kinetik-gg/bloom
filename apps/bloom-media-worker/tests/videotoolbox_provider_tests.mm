#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include "videotoolbox_provider.hpp"

#include <bloom/media/provider/protocol.hpp>
#include <bloom/media/provider/videotoolbox_manifest.hpp>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <numeric>
#include <string>
#include <variant>

namespace {
int failures = 0;
void check(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

// A real H.264 clip generated with the same framework the provider uses; nothing is checked in.
bool writeFixture(const std::string& path) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
        NSError* error = nil;
        AVAssetWriter* writer = [[AVAssetWriter alloc] initWithURL:url
                                                          fileType:AVFileTypeQuickTimeMovie
                                                             error:&error];
        if (writer == nil)
            return false;
        NSDictionary* settings = @{
            AVVideoCodecKey : AVVideoCodecTypeH264,
            AVVideoWidthKey : @(64),
            AVVideoHeightKey : @(48),
        };
        AVAssetWriterInput* input =
            [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo outputSettings:settings];
        input.expectsMediaDataInRealTime = NO;
        NSDictionary* attributes = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
        AVAssetWriterInputPixelBufferAdaptor* adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc]
            initWithAssetWriterInput:input
          sourcePixelBufferAttributes:attributes];
        if (![writer canAddInput:input])
            return false;
        [writer addInput:input];
        if (![writer startWriting])
            return false;
        [writer startSessionAtSourceTime:kCMTimeZero];
        for (int frame = 0; frame < 3; ++frame) {
            CVPixelBufferRef buffer = nullptr;
            CVPixelBufferCreate(kCFAllocatorDefault, 64, 48, kCVPixelFormatType_32BGRA, nullptr,
                                &buffer);
            CVPixelBufferLockBaseAddress(buffer, 0);
            auto* base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
            const auto stride = CVPixelBufferGetBytesPerRow(buffer);
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 64; ++x) {
                    auto* pixel = base + static_cast<std::size_t>(y) * stride +
                                  static_cast<std::size_t>(x) * 4;
                    pixel[0] = static_cast<std::uint8_t>(x * 4);
                    pixel[1] = static_cast<std::uint8_t>(y * 5);
                    pixel[2] = static_cast<std::uint8_t>(frame * 80);
                    pixel[3] = 255;
                }
            CVPixelBufferUnlockBaseAddress(buffer, 0);
            while (!input.isReadyForMoreMediaData)
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            [adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(frame, 24)];
            CVPixelBufferRelease(buffer);
        }
        [input markAsFinished];
        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{
            dispatch_semaphore_signal(semaphore);
        }];
        while (dispatch_semaphore_wait(semaphore, DISPATCH_TIME_NOW) != 0)
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        return writer.status == AVAssetWriterStatusCompleted;
    }
}

bloom::media::provider::Rational reduced(std::int64_t numerator, std::int64_t denominator) {
    if (numerator == 0)
        return {0, 1};
    const auto divisor = std::gcd(numerator < 0 ? -numerator : numerator, denominator);
    return {numerator / divisor, denominator / divisor};
}

void testProbeAndDecode(const std::string& path) {
    using namespace bloom::media;
    provider::CallRequest probeRequest;
    probeRequest.capability.role = provider::Role::Probe;
    probeRequest.source = path;
    auto probePayload = videotoolbox::call(probeRequest);
    const auto* probe = std::get_if<provider::ProbeResult>(&probePayload);
    check(probe != nullptr && !probe->streams.empty(), "probe returns a stream");
    if (probe != nullptr && !probe->streams.empty()) {
        check(probe->streams[0].codec == "h264", "probe reports h264");
        check(probe->streams[0].id == 0, "probe stream ids are zero-based");
        check(probe->streams[0].width == 64 && probe->streams[0].height == 48,
              "probe reports 64x48");
    }

    provider::CallRequest decodeRequest;
    decodeRequest.capability.role = provider::Role::VideoDecode;
    decodeRequest.capability.codec = "h264";
    decodeRequest.source = path;
    decodeRequest.frame = 1;
    auto framePayload = videotoolbox::call(decodeRequest);
    const auto* frame = std::get_if<provider::FrameProduct>(&framePayload);
    check(frame != nullptr && frame->planes.size() == 3, "decode returns three planes");
    if (frame != nullptr && !frame->planes.empty()) {
        check(frame->format == provider::PixelFormat::Yuv420p8, "decode returns YUV420P8");
        check(frame->planes.size() == 3, "decode returns three planes");
        check(frame->planes[0].width == 64 && frame->planes[0].height == 48 &&
                  frame->planes[0].stride == 64,
              "decoded luma plane is 64x48");
        check(frame->planes[1].width == 32 && frame->planes[1].height == 24,
              "decoded chroma plane is 32x24");
        check(frame->pts == reduced(1, 24), "decoded frame 1 is at 1/24s");
    }

    // A stream index the file does not have is a typed refusal, never a crash or a wrong frame.
    provider::CallRequest missingStream;
    missingStream.capability.role = provider::Role::VideoDecode;
    missingStream.capability.codec = "h264";
    missingStream.source = path;
    missingStream.stream = 5;
    auto missingPayload = videotoolbox::call(missingStream);
    check(std::get_if<provider::Unavailable>(&missingPayload) != nullptr,
          "a nonexistent stream is refused");
}

void testEncodeEdgeCases() {
    using namespace bloom::media;
    // H.264 with odd dimensions is refused up front rather than failing mid-encode.
    provider::EncodeSettingsV1 odd;
    odd.container = "mov";
    odd.videoCodec = "h264";
    odd.profile = "high";
    odd.width = 65;
    odd.height = 49;
    odd.rate = {24, 1};
    odd.frames = 1;
    videotoolbox::Encoder encoder;
    auto beginPayload = encoder.call(provider::MessageKind::EncodeBegin, odd);
    check(std::get_if<provider::Unavailable>(&beginPayload) != nullptr,
          "odd H.264 dimensions are refused with a typed error");
}

void testEncodeRoundTrip() {
    using namespace bloom::media;
    provider::EncodeSettingsV1 settings;
    settings.container = "mov";
    settings.videoCodec = "prores";
    settings.profile = "hq";
    settings.width = 64;
    settings.height = 48;
    settings.rate = {24, 1};
    settings.frames = 3;
    settings.audioCodec = "pcm_s16le";
    settings.sampleRate = 48000;
    settings.channels = 2;
    settings.audioSamples = 6000;

    videotoolbox::Encoder encoder;
    auto beginPayload = encoder.call(provider::MessageKind::EncodeBegin, settings);
    check(!std::get_if<provider::Unavailable>(&beginPayload), "encode begin accepted");
    for (std::uint64_t index = 0; index < settings.frames; ++index) {
        provider::FrameProduct frame;
        frame.format = provider::PixelFormat::Rgba16;
        frame.pts = reduced(static_cast<std::int64_t>(index) * settings.rate.denominator,
                            settings.rate.numerator);
        provider::CpuPlane plane;
        plane.width = 64;
        plane.height = 48;
        plane.stride = 64 * 8;
        plane.bytes.assign(64U * 48U * 8U, std::byte{0});
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x) {
                auto* pixel = reinterpret_cast<unsigned char*>(
                    plane.bytes.data() + static_cast<std::size_t>(y) * plane.stride +
                    static_cast<std::size_t>(x) * 8);
                // Half saturated red, half black: the hard chroma edge the ProRes tolerance profile
                // is most likely to stress, plus a green/blue ramp for coverage.
                const bool red = x < 32;
                pixel[0] = red ? 255 : 0;
                pixel[1] = red ? 255 : 0;
                pixel[3] = static_cast<unsigned char>(y * 5);
                pixel[5] = static_cast<unsigned char>(index * 80);
                pixel[7] = 255;
            }
        plane.digest = provider::digestBytes(plane.bytes);
        frame.planes.push_back(std::move(plane));
        auto framePayload = encoder.call(provider::MessageKind::Frame, frame);
        check(!std::get_if<provider::Unavailable>(&framePayload), "encode frame accepted");
    }
    auto audioBlock = provider::AudioBlock{};
    audioBlock.pts = reduced(0, 1);
    audioBlock.sampleRate = settings.sampleRate;
    audioBlock.channelLayout = {"L", "R"};
    audioBlock.channels.assign(2, std::vector<float>(6000, 0.0F));
    for (std::size_t index = 0; index < audioBlock.channels[0].size(); ++index) {
        audioBlock.channels[0][index] = static_cast<float>(index % 100) / 100.0F;
        audioBlock.channels[1][index] = audioBlock.channels[0][index];
    }
    auto audioPayload = encoder.call(provider::MessageKind::Audio, audioBlock);
    check(!std::get_if<provider::Unavailable>(&audioPayload), "encode audio accepted");

    auto qcPayload = encoder.call(provider::MessageKind::EncodeFinish, std::monostate{});
    const auto* qc = std::get_if<provider::EncodeQcV1>(&qcPayload);
    check(qc != nullptr, "encode finish returns QC");
    if (qc != nullptr) {
        check(qc->frames == settings.frames, "reopened frame count matches");
        check(qc->audioSamples == settings.audioSamples, "encoded audio sample count matches");
        check(qc->bytes > 0, "artifact is not empty");
        check(qc->meanError <= 1967, "reopened pixels meet the ProRes mean tolerance");
        check(qc->duration == reduced(3, 24), "reopened duration is 3/24s");
        auto chunkPayload = encoder.call(provider::MessageKind::EncodeRead, std::uint64_t{0});
        const auto* chunk = std::get_if<provider::EncodedChunkV1>(&chunkPayload);
        check(chunk != nullptr && chunk->offset == 0 && !chunk->bytes.empty(),
              "encode read returns the artifact head");
    }
}
void testRegistryQualification() {
    using namespace bloom::media;
    const auto hello = provider::videoToolboxHandshake();
    provider::CapabilityRegistry registry;
    for (const auto& declaration : hello.declarations)
        check(std::holds_alternative<provider::Digest>(registry.registerProvider(declaration)),
              "provider declaration registers");
    const auto video = std::find_if(
        hello.declarations.begin(), hello.declarations.end(), [](const auto& declaration) {
            return declaration.capability.role == provider::Role::VideoDecode &&
                   declaration.capability.codec == "h264" &&
                   declaration.capability.purpose == provider::Purpose::Preview;
        });
    check(video != hello.declarations.end(), "h264 preview decode is declared");
    if (video != hello.declarations.end()) {
        const auto pipeline = provider::videoToolboxPipeline(*video);
        check(std::holds_alternative<provider::Digest>(registry.qualifyPipeline(pipeline)),
              "h264 preview decode pipeline qualifies");
        check(std::holds_alternative<provider::MediaAttempt>(registry.begin(pipeline)),
              "h264 preview decode pipeline can begin (the import path)");
    }
}
} // namespace

int main() {
    @autoreleasepool {
        const auto path = (std::filesystem::temp_directory_path() / "bloom-vt-provider-test.mov")
                              .string();
        if (writeFixture(path)) {
            check(true, "fixture generated");
        } else {
            check(false, "fixture could not be generated");
        }
        testProbeAndDecode(path);
        testEncodeRoundTrip();
        testEncodeEdgeCases();
        testRegistryQualification();
        std::filesystem::remove(path);
    }
    if (failures == 0)
        std::printf("videotoolbox provider: all checks passed\n");
    return failures;
}
