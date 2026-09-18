#include <bloom/media/provider/ffmpeg_launch.hpp>

#include <bloom/media/provider/openh264_runtime.hpp>

namespace bloom::media::provider {
void configureFfmpegWorkerEnvironment(platform::ProcessOptions& options,
                                      const std::string& executable,
                                      std::filesystem::path verifiedOpenH264) {
#if defined(__linux__)
    if (verifiedOpenH264.empty()) {
        const auto status = OpenH264Runtime().verify();
        if (status.installed)
            verifiedOpenH264 = status.directory;
    }
    std::error_code error;
    const auto worker = std::filesystem::absolute(executable, error);
    const auto privateLibrary = error ? std::filesystem::path{} : worker.parent_path() / "lib";
    std::string path = verifiedOpenH264.string();
    if (!path.empty() && !privateLibrary.empty())
        path += ':';
    path += privateLibrary.string();
    if (!path.empty())
        options.environment.push_back("LD_LIBRARY_PATH=" + path);
#else
    (void)options;
    (void)executable;
    (void)verifiedOpenH264;
#endif
}
} // namespace bloom::media::provider
