#include <bloom/host/frame_range_runner.hpp>

#include <algorithm>
#include <utility>

namespace bloom::host {

std::filesystem::path
FrameRangeRunnerV1::sequenceFramePath(const std::filesystem::path& destination,
                                      const std::uint64_t index, const std::uint64_t lastIndex) {
    auto digits = std::max<std::size_t>(std::to_string(lastIndex).size(), 4);
    auto number = std::to_string(index);
    if (number.size() < digits) {
        number.insert(0, digits - number.size(), '0');
    }
    auto stem = destination.stem().string();
    if (stem.empty()) {
        stem = "frame";
    }
    return destination.parent_path() / (stem + "." + number + destination.extension().string());
}

std::optional<core::RationalTime>
FrameRangeRunnerV1::timeForFrame(const FrameRangeRequestV1& request,
                                 const std::uint64_t index) noexcept {
    const auto mapping = core::FrameTimeMapping::create(
        request.duration, request.frameRate.numerator(), request.frameRate.denominator());
    if (!mapping || index > mapping.value()->maximumFrameIndex()) {
        return std::nullopt;
    }
    const auto result = mapping.value()->timeForFrame(index);
    return result ? std::optional<core::RationalTime>(*result.value()) : std::nullopt;
}

FrameRangeResultV1 FrameRangeRunnerV1::run(const FrameRangeRequestV1& request,
                                           const FrameRangeFrameCallbackV1& callback,
                                           const FrameRangeCancellationV1& cancellation) {
    if (request.destination.empty() || request.lastFrame < request.firstFrame || !callback ||
        !timeForFrame(request, request.firstFrame).has_value() ||
        !timeForFrame(request, request.lastFrame).has_value()) {
        return {.status = FrameRangeStatusV1::Refused,
                .publishedFrames = 0,
                .failedFrame = std::nullopt,
                .diagnostic = "Frame range is invalid"};
    }
    FrameRangeResultV1 result{.status = FrameRangeStatusV1::Published,
                              .publishedFrames = 0,
                              .failedFrame = std::nullopt,
                              .diagnostic = {}};
    for (std::uint64_t index = request.firstFrame;; ++index) {
        if (cancellation && cancellation()) {
            result.status = FrameRangeStatusV1::Cancelled;
            return result;
        }
        const auto time = timeForFrame(request, index);
        if (!time.has_value()) {
            result.status = FrameRangeStatusV1::Failed;
            result.failedFrame = index;
            result.diagnostic = "Frame has no exact composition time";
            return result;
        }
        const FrameRangeFrameV1 frame{
            .index = index,
            .time = *time,
            .path = sequenceFramePath(request.destination, index, request.lastFrame)};
        if (!callback(frame)) {
            result.status = FrameRangeStatusV1::Failed;
            result.failedFrame = index;
            result.diagnostic = "Frame callback failed";
            return result;
        }
        ++result.publishedFrames;
        if (index == request.lastFrame) {
            break;
        }
    }
    return result;
}

} // namespace bloom::host
