#include <bloom/platform/cache_directory.hpp>

#include <cstdlib>

namespace bloom::platform {

namespace {
[[nodiscard]] std::optional<std::filesystem::path> environmentPath(const char* name) {
    const auto* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
    if (value == nullptr || *value == '\0')
        return std::nullopt;
    return std::filesystem::path(value);
}
} // namespace

std::optional<std::filesystem::path> userCacheDirectory(const std::string_view appSubdirectory) {
    if (appSubdirectory.empty())
        return std::nullopt;
#if defined(_WIN32)
    if (const auto local = environmentPath("LOCALAPPDATA"))
        return *local / appSubdirectory / "Cache";
    return std::nullopt;
#else
    if (const auto xdg = environmentPath("XDG_CACHE_HOME"); xdg && xdg->is_absolute())
        return *xdg / appSubdirectory;
    if (const auto home = environmentPath("HOME"))
        return *home / ".cache" / appSubdirectory;
    return std::nullopt;
#endif
}

} // namespace bloom::platform
