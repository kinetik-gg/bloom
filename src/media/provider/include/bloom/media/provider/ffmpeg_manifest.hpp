#pragma once
#include <bloom/media/provider/protocol.hpp>
namespace bloom::media::provider {
// Codec-free declaration shared by the host and the isolated executable.
[[nodiscard]] Handshake ffmpegHandshake(bool hardware = false);
[[nodiscard]] PipelineQualificationV1 ffmpegPipeline(const ProviderDeclaration& declaration);
inline constexpr const char* kProResPreviewNote =
    "Decoded by FFmpeg; not an Apple-authorized ProRes implementation";
inline constexpr const char* kProResExportNote =
    "Decoded/encoded by FFmpeg; not an Apple-authorized ProRes implementation";
} // namespace bloom::media::provider
