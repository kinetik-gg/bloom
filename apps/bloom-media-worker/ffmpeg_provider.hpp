#pragma once
#include <bloom/media/provider/protocol.hpp>
#include <memory>
namespace bloom::media::ffmpeg {
class Encoder final {
  public:
    Encoder();
    ~Encoder();
    [[nodiscard]] provider::Payload call(provider::MessageKind, const provider::Payload&);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
[[nodiscard]] provider::Payload call(const provider::CallRequest& request, bool hardware = false);
[[nodiscard]] bool generateFixtures(const std::string& directory);
} // namespace bloom::media::ffmpeg
