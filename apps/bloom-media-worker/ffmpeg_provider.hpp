#pragma once
#include <bloom/media/provider/protocol.hpp>
#include <memory>
namespace bloom::media::ffmpeg {
// Worker-local decoder for one immutable inherited source generation. The host bounds its
// lifetime, request count and cancellation with ProcessSupervisor.
class DecodeSession final {
  public:
    explicit DecodeSession(bool hardware = false);
    ~DecodeSession();
    [[nodiscard]] provider::Payload call(const provider::CallRequest& request);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
class Encoder final {
  public:
    explicit Encoder(bool hardware = false);
    ~Encoder();
    [[nodiscard]] provider::Payload call(provider::MessageKind, const provider::Payload&);

  private:
    struct State;
    std::unique_ptr<State> state_;
    bool hardware_ = false;
};
[[nodiscard]] provider::Payload call(const provider::CallRequest& request, bool hardware = false);
[[nodiscard]] bool hardwareEncodeAvailable();
[[nodiscard]] bool generateFixtures(const std::string& directory);
} // namespace bloom::media::ffmpeg
