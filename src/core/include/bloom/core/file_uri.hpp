#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::core {

// RFC 8089 absolute file URI. A UNC authority is represented by the server name;
// UTF-8 path bytes are percent encoded. Neither operation accesses the filesystem.
[[nodiscard]] std::optional<std::string> fileUriFromAbsolutePath(const std::filesystem::path& path);
// Portable syntax validation, including locations belonging to another operating system.
// Persistence must use this rather than testing whether the current host can resolve the URI.
[[nodiscard]] bool isAbsoluteFileUri(std::string_view uri);
// Returns no path when the URI names a location the current host cannot represent.
[[nodiscard]] std::optional<std::filesystem::path> absolutePathFromFileUri(std::string_view uri);

} // namespace bloom::core
