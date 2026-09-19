#pragma once

#include <bloom/media/provider/protocol.hpp>

namespace bloom::media::provider {

// The macOS worker's VideoToolbox/AVFoundation provider handshake. Mirrors ffmpegHandshake's shape
// (and is consumed by the same host validation) but declares the Apple framework identities:
// implementation Hardware, trust domain apple-videotoolbox, and no bundled third-party codec.
[[nodiscard]] Handshake videoToolboxHandshake(bool hardwareEncoder = true);

// The exact pipeline digest for one declared VideoToolbox capability.
[[nodiscard]] PipelineQualificationV1 videoToolboxPipeline(const ProviderDeclaration& declaration);

} // namespace bloom::media::provider
