#include <bloom/host/frame_range_runner.hpp>

#include <iostream>
#include <vector>

namespace {

int expect(const bool condition, const char* message) {
    if (condition) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    using bloom::core::RationalTime;
    using bloom::document::FrameRate;
    using bloom::host::FrameRangeRequestV1;
    using bloom::host::FrameRangeRunnerV1;
    using bloom::host::FrameRangeStatusV1;

    const FrameRangeRequestV1 request{.destination = "render/frame.png",
                                      .firstFrame = 12,
                                      .lastFrame = 17,
                                      .frameRate = FrameRate::framesPerSecond24(),
                                      .duration = RationalTime::fromInteger(48)};
    std::vector<bloom::host::FrameRangeFrameV1> frames;
    const auto result = FrameRangeRunnerV1::run(request, [&](const auto& frame) {
        frames.push_back(frame);
        return true;
    });
    if (expect(result.status == FrameRangeStatusV1::Published && result.publishedFrames == 6,
               "frame range published six frames") != 0) {
        return 1;
    }
    if (expect(frames.size() == 6, "frame range callback received six frames") != 0) {
        return 1;
    }
    const auto frameTwelve = RationalTime::create(12, 24);
    const auto frameSeventeen = RationalTime::create(17, 24);
    if (expect(frameTwelve.has_value() && frameSeventeen.has_value() &&
                   frames.front().index == 12 && frames.front().time == *frameTwelve &&
                   frames.front().path.filename() == "frame.0012.png",
               "frame range first frame has the exact mapped time and path") != 0) {
        return 1;
    }
    if (expect(frameSeventeen.has_value() && frames.back().index == 17 &&
                   frames.back().time == *frameSeventeen &&
                   frames.back().path.filename() == "frame.0017.png",
               "frame range last frame matches the UI sequence contract") != 0) {
        return 1;
    }

    std::size_t callbacks = 0;
    const auto cancelled = FrameRangeRunnerV1::run(
        request,
        [&](const auto&) {
            ++callbacks;
            return true;
        },
        [&] { return callbacks == 2; });
    return expect(cancelled.status == FrameRangeStatusV1::Cancelled && callbacks == 2,
                  "frame range cancellation stops before the next publication");
}
