#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace bloom::platform {

// Resolves the platform's per-user cache root for Bloom-owned derived data (currently the media
// disk cache; see docs/architecture/media-io.md "Disk cache"). The directory itself is NOT
// created by this function -- callers create it (and its subdirectories) lazily on first use.
//
// Linux: `$XDG_CACHE_HOME/<appSubdirectory>` when `XDG_CACHE_HOME` is set to an absolute path,
// else `$HOME/.cache/<appSubdirectory>`.
// Windows: `%LOCALAPPDATA%/<appSubdirectory>/Cache`.
// macOS and any platform without a resolvable home/local-app-data variable: `nullopt`. Derived
// caches are always optional runtime state (docs/architecture/media-io.md), so a caller must
// treat a missing directory as "disk cache unavailable" rather than fabricate a relative path.
[[nodiscard]] std::optional<std::filesystem::path>
userCacheDirectory(std::string_view appSubdirectory = "bloom");

} // namespace bloom::platform
