#pragma once
#include <bloom/media/provider/provider_chain.hpp>
#include <filesystem>
#include <string>
#include <string_view>

// Worker executables and the per-platform provider chains (ADR 0035). Every operating system
// ships the bundled LGPL FFmpeg worker as the last provider; the platform media framework comes
// first where Bloom has a provider for it:
//   Windows: Media Foundation (no provider yet; the slot reports NotInstalled) -> FFmpeg
//   macOS:   VideoToolbox/AVFoundation -> FFmpeg
//   Linux:   FFmpeg with VA-API -> FFmpeg software
namespace bloom::media::provider {

// Stable provenance identities of the providers.
inline constexpr std::string_view kMediaFoundationProviderId = "media-foundation";
inline constexpr std::string_view kVideoToolboxProviderId = "videotoolbox";
inline constexpr std::string_view kFfmpegVaapiProviderId = "ffmpeg-vaapi";
inline constexpr std::string_view kFfmpegProviderId = "ffmpeg";

// Worker generation directories.
inline constexpr std::string_view kFfmpegWorkerGeneration = "ffmpeg-v1";
inline constexpr std::string_view kVideoToolboxWorkerGeneration = "videotoolbox-v1";

// The running executable as the operating system reports it (not argv[0]); empty if unknown.
[[nodiscard]] std::filesystem::path hostExecutablePath();
// Where an installed worker of one generation lives relative to the running executable:
//   Linux and macOS: <executable dir>/../libexec/bloom/media/<generation>/<name>
//                    (inside Bloom.app on macOS: Contents/libexec/...)
//   Windows:         <executable dir>\media\<generation>\<name>.exe, with its DLLs beside it.
[[nodiscard]] std::filesystem::path installedMediaWorker(std::string_view generation,
                                                         std::string_view name);
// The bundled FFmpeg worker: the installed copy, else this build tree's worker, else empty.
[[nodiscard]] std::string ffmpegWorkerExecutable();
// The platform media framework's worker (VideoToolbox on macOS); empty when this platform has no
// native provider worker in this build.
[[nodiscard]] std::string nativeWorkerExecutable();
// The platform's native provider id ("videotoolbox", "media-foundation"), or empty on Linux.
[[nodiscard]] std::string_view nativeMediaProviderId() noexcept;
// The native provider's handshake for this platform; empty declarations when there is none.
[[nodiscard]] Handshake nativeMediaHandshake();

[[nodiscard]] bool regularFileExists(const std::string& path) noexcept;

} // namespace bloom::media::provider
