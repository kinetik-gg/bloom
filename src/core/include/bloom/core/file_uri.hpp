#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::core {

// RFC 8089 absolute file URI. A UNC authority is represented by the server name;
// UTF-8 path bytes are percent encoded. Neither operation accesses the filesystem.
[[nodiscard]] std::optional<std::string>
fileUriFromAbsolutePath(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::filesystem::path>
absolutePathFromFileUri(std::string_view uri);

} // namespace bloom::core
