#include <bloom/platform/user_data_directory.hpp>

#include <cstdlib>

namespace bloom::platform {
namespace {
std::optional<std::filesystem::path> env(const char* name) {
    const auto* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
    if (value == nullptr || *value == '\0')
        return std::nullopt;
    return std::filesystem::path(value);
}
} // namespace

std::optional<std::filesystem::path> userDataDirectory(const std::string_view subdirectory) {
    if (subdirectory.empty())
        return std::nullopt;
#if defined(_WIN32)
    if (const auto local = env("LOCALAPPDATA"))
        return *local / subdirectory;
    return std::nullopt;
#elif defined(__APPLE__)
    if (const auto home = env("HOME"))
        return *home / "Library/Application Support" / subdirectory;
    return std::nullopt;
#else
    if (const auto data = env("XDG_DATA_HOME"); data && data->is_absolute())
        return *data / subdirectory;
    if (const auto home = env("HOME"))
        return *home / ".local/share" / subdirectory;
    return std::nullopt;
#endif
}
} // namespace bloom::platform
