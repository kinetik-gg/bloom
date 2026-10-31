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
    // `nvenc`: the NVIDIA NVENC generation (h264_nvenc, hevc_nvenc); `hardware`: VA-API.
    explicit Encoder(bool hardware = false, int stagedOutput = -1, bool nvenc = false);
    ~Encoder();
    [[nodiscard]] provider::Payload call(provider::MessageKind, const provider::Payload&);

  private:
    struct State;
    std::unique_ptr<State> state_;
    bool hardware_ = false;
    bool nvenc_ = false;
    int stagedOutput_ = -1;
};
[[nodiscard]] provider::Payload call(const provider::CallRequest& request, bool hardware = false);
[[nodiscard]] bool hardwareEncodeAvailable();
// `--probe-vaapi`: prints "bloom-vaapi-probe-v1" and one "<render node> <h264> <hevc>" line per
// VA-API render node (1 when the encode entry point opens) to stdout, then the worker exits.
[[nodiscard]] bool probeVaapiEncode();
// Whether the NVIDIA driver's NVENC opens an H.264 encode session (the driver library is loaded at
// run time; a computer without it answers false).
[[nodiscard]] bool nvencEncodeAvailable();
// `--probe-nvenc`: prints "bloom-nvenc-probe-v1" and one "<h264> <hevc> <hevc-main10>" line
// (1 when that NVENC session opens) to stdout, then the worker exits.
[[nodiscard]] bool probeNvencEncode();
[[nodiscard]] bool generateFixtures(const std::string& directory);
} // namespace bloom::media::ffmpeg
