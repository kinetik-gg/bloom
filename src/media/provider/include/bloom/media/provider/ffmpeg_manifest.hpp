#pragma once
#include <bloom/media/provider/protocol.hpp>
namespace bloom::media::provider {
// Codec-free declaration shared by the host and the isolated executable.
[[nodiscard]] Handshake ffmpegHandshake(bool hardware = false, bool openh264 = false,
                                        std::string_view openh264Version = {},
                                        std::string_view openh264Digest = {});
[[nodiscard]] PipelineQualificationV1 ffmpegPipeline(const ProviderDeclaration& declaration);
// Presentation note for a ProRes deliverable. On macOS ProRes is the operating system's own codec
// through AVFoundation/VideoToolbox -- the native, Apple-authorized implementation -- so the
// FFmpeg non-authorization caveat does not apply and must not be shown.
#if defined(__APPLE__)
inline constexpr const char* kProResPreviewNote =
    "Apple ProRes decoded by AVFoundation/VideoToolbox";
inline constexpr const char* kProResExportNote =
    "Apple ProRes encoded by AVFoundation/VideoToolbox; native Apple implementation";
#else
inline constexpr const char* kProResPreviewNote =
    "Decoded by FFmpeg; not an Apple-authorized ProRes implementation";
inline constexpr const char* kProResExportNote =
    "Decoded/encoded by FFmpeg; not an Apple-authorized ProRes implementation";
#endif
} // namespace bloom::media::provider
