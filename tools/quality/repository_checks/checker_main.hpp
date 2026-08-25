#pragma once

#include "repository_checks.hpp"

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace bloom::quality {

using RepositoryScanner = auto (*)(const std::filesystem::path&,
                                   std::span<const std::filesystem::path>)
    -> std::vector<RepositoryFinding>;

[[nodiscard]] auto runRepositoryChecker(std::span<const char* const> arguments,
                                        std::string_view successMessage,
                                        std::string_view failureMessage, RepositoryScanner scanner)
    -> int;

} // namespace bloom::quality
