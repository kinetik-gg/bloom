#pragma once

#include <bloom/media/provider/protocol.hpp>

#include <memory>
#include <string>

namespace bloom::media::videotoolbox {

// Encode/mux via AVAssetWriter + VideoToolbox. Implemented in a later slice; until then every
// encode call returns a typed Unavailable rather than a silent or partial result.
class Encoder final {
  public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    [[nodiscard]] provider::Payload call(provider::MessageKind, const provider::Payload&);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

// Read path: Probe, DemuxIndex, VideoDecode, AudioDecode and ReopenDecode via AVFoundation.
[[nodiscard]] provider::Payload call(const provider::CallRequest& request);

[[nodiscard]] bool hardwareEncodeAvailable();

} // namespace bloom::media::videotoolbox
