// End-to-end decode test for the macOS VideoToolbox worker. It generates a real H.264 clip with
// AVAssetWriter, then drives the host VideoDecodeSession, which launches the real bloom-media-worker
// process, so failures in the handshake, probe, stream selection, decode, or frame validation show
// up here rather than only inside the running application. Nothing binary is checked in.
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <bloom/media/video/session.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <variant>

namespace {
int failures = 0;
void check(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

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
} // namespace

int main() {
    using namespace bloom::media;
    @autoreleasepool {
        const auto path =
            (std::filesystem::temp_directory_path() / "bloom-vt-decode-test.mov").string();
        if (!writeFixture(path)) {
            std::printf("fixture write FAILED\n");
            return 1;
        }
        video::VideoDecodeSession session(path);
        auto probed = session.probe();
        const auto* probe = std::get_if<provider::ProbeResult>(&probed);
        check(probe != nullptr, "probe returns a result");
        if (probe != nullptr) {
            check(!probe->streams.empty(), "probe returns a stream");
            if (!probe->streams.empty()) {
                check(probe->streams[0].id == 0, "stream id is zero-based");
                check(probe->streams[0].codec == "h264", "probe reports h264");
                check(probe->streams[0].width == 64 && probe->streams[0].height == 48,
                      "probe reports 64x48");
            }
            check(!session.verifySource(probe->sourceDigest), "source digest verifies");

            auto decoded = session.frame(*probe, 0, 1, 0, nullptr);
            const auto* frame =
                std::get_if<std::shared_ptr<const provider::FrameProduct>>(&decoded);
            check(frame != nullptr, "decode returns a frame");
            if (frame != nullptr && *frame) {
                check((*frame)->format == provider::PixelFormat::Yuv420p8,
                      "decode returns YUV420P8");
                check((*frame)->planes.size() == 3, "decode returns three planes");
                check((*frame)->planes[0].width == 64 && (*frame)->planes[0].height == 48,
                      "decoded luma plane is 64x48");
                check((*frame)->pts == provider::Rational{1, 24}, "decoded frame 1 is at 1/24s");
            }

            // A stream the file does not have is a typed refusal, not a crash.
            auto missing = session.frame(*probe, 5, 0, 0, nullptr);
            check(std::get_if<provider::Unavailable>(&missing) != nullptr,
                  "a nonexistent stream is refused");
        }
        std::filesystem::remove(path);
    }
    if (failures == 0)
        std::printf("videotoolbox decode: all checks passed\n");
    return failures;
}
