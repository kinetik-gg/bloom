#pragma once
#include <bloom/media/provider/protocol.hpp>
namespace bloom::media::ffmpeg {
[[nodiscard]] provider::Payload call(const provider::CallRequest& request, bool hardware = false);
[[nodiscard]] bool generateFixtures(const std::string& directory);
} // namespace bloom::media::ffmpeg
