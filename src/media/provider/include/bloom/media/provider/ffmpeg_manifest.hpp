#pragma once
#include <bloom/media/provider/protocol.hpp>
namespace bloom::media::provider {
// Codec-free declaration shared by the host and the isolated executable.
// `hardware` selects the VA-API encode generation; the software generation always declares the
// bundled OpenH264 (H.264), Kvazaar (HEVC), libvpx (VP9), SVT-AV1 (AV1) and libopus (Opus)
// encoders.
[[nodiscard]] Handshake ffmpegHandshake(bool hardware = false);
// The NVIDIA NVENC encode generation (`--nvenc`): H.264 High and HEVC Main/Main 10 through
// h264_nvenc/hevc_nvenc, with the driver loaded at run time. Hardware execution key, no
// determinism claim; every export still passes the reopen check.
[[nodiscard]] Handshake ffmpegNvencHandshake();
// Preview-only VA-API decode for H.264/HEVC with run-time render-node discovery and a
// per-worker software parity gate. Software decode remains the reference and fallback.
[[nodiscard]] Handshake ffmpegHardwareDecodeHandshake();
[[nodiscard]] PipelineQualificationV1 ffmpegPipeline(const ProviderDeclaration& declaration);
// Presentation notes for a ProRes deliverable, by the implementation that served it. On macOS
// ProRes is the operating system's own codec through AVFoundation/VideoToolbox -- the native,
// Apple-authorized implementation -- so the FFmpeg non-authorization caveat does not apply to it.
inline constexpr const char* kProResVideoToolboxPreviewNote =
    "Apple ProRes decoded by AVFoundation/VideoToolbox";
inline constexpr const char* kProResVideoToolboxExportNote =
    "Apple ProRes encoded by AVFoundation/VideoToolbox; native Apple implementation";
inline constexpr const char* kProResFfmpegPreviewNote =
    "Decoded by FFmpeg; not an Apple-authorized ProRes implementation";
inline constexpr const char* kProResFfmpegExportNote =
    "Decoded/encoded by FFmpeg; not an Apple-authorized ProRes implementation";
// The note for this platform's preferred provider (VideoToolbox first on macOS).
#if defined(__APPLE__)
inline constexpr const char* kProResPreviewNote = kProResVideoToolboxPreviewNote;
inline constexpr const char* kProResExportNote = kProResVideoToolboxExportNote;
#else
inline constexpr const char* kProResPreviewNote = kProResFfmpegPreviewNote;
inline constexpr const char* kProResExportNote = kProResFfmpegExportNote;
#endif
} // namespace bloom::media::provider
