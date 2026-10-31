#include <bloom/media/provider/ffmpeg_launch.hpp>

namespace bloom::media::provider {
void configureFfmpegWorkerEnvironment(platform::ProcessOptions& options,
                                      const std::string& executable) {
#if defined(__linux__)
    std::error_code error;
    const auto worker = std::filesystem::absolute(executable, error);
    if (!error)
        options.environment.push_back("LD_LIBRARY_PATH=" + (worker.parent_path() / "lib").string());
#else
    (void)options;
    (void)executable;
#endif
}
} // namespace bloom::media::provider
