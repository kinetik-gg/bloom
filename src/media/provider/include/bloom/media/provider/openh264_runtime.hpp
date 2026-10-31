#pragma once

#include <string>

namespace bloom::media::provider {

// OpenH264 is built from source and linked into the FFmpeg worker's libavcodec on every platform
// (dependencies/licenses/openh264/review.md): Bloom never downloads, verifies or installs an
// encoder binary at run time. This header only keeps the desktop's former "Install OpenH264" flow
// compiling until the UI removes it; nothing in the engine calls it, and no value here describes a
// download.
class OpenH264Runtime final {
  public:
    // The bundled release; the worker handshake's software H.264 encoder.
    [[nodiscard]] static std::string version() { return "2.6.0"; }
    // There is no download and no separate binary licence.
    [[nodiscard]] static std::string downloadUrl() { return {}; }
    [[nodiscard]] static std::string binaryLicenseUrl() { return {}; }
    [[nodiscard]] static std::string binaryLicenseText() { return {}; }
};

} // namespace bloom::media::provider
