#include <bloom/media/provider/media_workers.hpp>

#if defined(__APPLE__)
#include <bloom/media/provider/videotoolbox_manifest.hpp>
#include <cstdint>
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <vector>
#include <windows.h>
#endif

namespace bloom::media::provider {

std::filesystem::path hostExecutablePath() {
    std::error_code error;
#if defined(__linux__)
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : path;
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::string buffer(size + 1U, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    auto canonical = std::filesystem::weakly_canonical(buffer, error);
    return error ? std::filesystem::path(buffer) : canonical;
#elif defined(_WIN32)
    std::vector<wchar_t> buffer(512);
    while (buffer.size() <= 32768) {
        const auto length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size())
            return std::filesystem::path(std::wstring(buffer.data(), length));
        buffer.resize(buffer.size() * 2);
    }
    return {};
#else
    (void)error;
    return {};
#endif
}

std::filesystem::path installedMediaWorker(const std::string_view generation,
                                           const std::string_view name) {
    const auto executable = hostExecutablePath();
    if (executable.empty())
        return {};
#if defined(_WIN32)
    return executable.parent_path() / "media" / std::filesystem::path(generation) /
           (std::string(name) + ".exe");
#else
    return executable.parent_path().parent_path() / "libexec" / "bloom" / "media" /
           std::filesystem::path(generation) / std::filesystem::path(name);
#endif
}

bool regularFileExists(const std::string& path) noexcept {
    if (path.empty())
        return false;
    std::error_code error;
    return std::filesystem::is_regular_file(std::filesystem::path(path), error) && !error;
}

namespace {
std::string installedOrBuilt(const std::string_view generation, const std::string_view name,
                             const char* built) {
    const auto installed = installedMediaWorker(generation, name);
    if (!installed.empty() && regularFileExists(installed.string()))
        return installed.string();
    return built != nullptr ? std::string(built) : std::string{};
}
} // namespace

std::string ffmpegWorkerExecutable() {
#ifdef BLOOM_FFMPEG_WORKER_BUILD
    return installedOrBuilt(kFfmpegWorkerGeneration, "bloom-media-worker",
                            BLOOM_FFMPEG_WORKER_BUILD);
#else
    return installedOrBuilt(kFfmpegWorkerGeneration, "bloom-media-worker", nullptr);
#endif
}

std::string nativeWorkerExecutable() {
#if defined(__APPLE__)
#ifdef BLOOM_NATIVE_WORKER_BUILD
    return installedOrBuilt(kVideoToolboxWorkerGeneration, "bloom-media-videotoolbox-worker",
                            BLOOM_NATIVE_WORKER_BUILD);
#else
    return installedOrBuilt(kVideoToolboxWorkerGeneration, "bloom-media-videotoolbox-worker",
                            nullptr);
#endif
#else
    // Windows: the Media Foundation provider slot has no worker yet. Linux has no separate
    // platform framework provider; VA-API is a mode of the FFmpeg worker.
    return {};
#endif
}

std::string_view nativeMediaProviderId() noexcept {
#if defined(__APPLE__)
    return kVideoToolboxProviderId;
#elif defined(_WIN32)
    return kMediaFoundationProviderId;
#else
    return {};
#endif
}

Handshake nativeMediaHandshake() {
#if defined(__APPLE__)
    return videoToolboxHandshake();
#else
    return {};
#endif
}

} // namespace bloom::media::provider
