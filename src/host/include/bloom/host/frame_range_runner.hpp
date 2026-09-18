#pragma once

#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace bloom::host {

struct FrameRangeRequestV1 final {
    std::filesystem::path destination;
    std::uint64_t firstFrame = 0;
    std::uint64_t lastFrame = 0;
    document::FrameRate frameRate = document::FrameRate::framesPerSecond24();
    core::RationalTime duration = core::RationalTime::fromInteger(1);
};

struct FrameRangeFrameV1 final {
    std::uint64_t index = 0;
    core::RationalTime time;
    std::filesystem::path path;
};

enum class FrameRangeStatusV1 : std::uint8_t {
    Published,
    Cancelled,
    Refused,
    Failed,
};

struct FrameRangeResultV1 final {
    FrameRangeStatusV1 status = FrameRangeStatusV1::Failed;
    std::uint64_t publishedFrames = 0;
    std::optional<std::uint64_t> failedFrame;
    std::string diagnostic;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == FrameRangeStatusV1::Published;
    }
};

using FrameRangeFrameCallbackV1 = std::function<bool(const FrameRangeFrameV1&)>;
using FrameRangeCancellationV1 = std::function<bool()>;

class FrameRangeRunnerV1 final {
  public:
    [[nodiscard]] static std::filesystem::path
    sequenceFramePath(const std::filesystem::path& destination, std::uint64_t index,
                      std::uint64_t lastIndex);
    [[nodiscard]] static std::optional<core::RationalTime>
    timeForFrame(const FrameRangeRequestV1& request, std::uint64_t index) noexcept;
    [[nodiscard]] static FrameRangeResultV1 run(const FrameRangeRequestV1& request,
                                                const FrameRangeFrameCallbackV1& callback,
                                                const FrameRangeCancellationV1& cancellation = {});
};

} // namespace bloom::host
