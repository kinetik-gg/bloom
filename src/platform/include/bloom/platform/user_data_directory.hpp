#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace bloom::platform {

// Resolves a per-user application-data root. The directory is created lazily by its caller.
[[nodiscard]] std::optional<std::filesystem::path>
userDataDirectory(std::string_view appSubdirectory = "Kinetik/Bloom");

} // namespace bloom::platform
