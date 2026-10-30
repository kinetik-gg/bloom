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
    // `stagedOutput` >= 0: the inherited staged artifact descriptor the first encode session
    // writes into (ADR 0033 section 16).
    explicit Encoder(bool hardware = false, int stagedOutput = -1);
    ~Encoder();
    [[nodiscard]] provider::Payload call(provider::MessageKind, const provider::Payload&);

  private:
    struct State;
    std::unique_ptr<State> state_;
    bool hardware_ = false;
    int stagedOutput_ = -1;
};
[[nodiscard]] provider::Payload call(const provider::CallRequest& request, bool hardware = false);
[[nodiscard]] bool hardwareEncodeAvailable();
// `--probe-vaapi`: prints "bloom-vaapi-probe-v1" and one "<render node> <h264> <hevc>" line per
// VA-API render node (1 when the encode entry point opens) to stdout, then the worker exits.
[[nodiscard]] bool probeVaapiEncode();
[[nodiscard]] bool generateFixtures(const std::string& directory);
} // namespace bloom::media::ffmpeg
