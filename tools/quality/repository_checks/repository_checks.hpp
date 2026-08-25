#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::quality {

struct RepositoryFinding {
    std::filesystem::path path;
    std::string category;
    std::string message;
    std::optional<std::size_t> line;

    [[nodiscard]] auto render() const -> std::string;
};

[[nodiscard]] auto repositoryFiles(const std::filesystem::path& root)
    -> std::vector<std::filesystem::path>;
[[nodiscard]] auto repositoryFilesFromNullManifest(const std::filesystem::path& root,
                                                   const std::filesystem::path& manifest)
    -> std::vector<std::filesystem::path>;

[[nodiscard]] auto scanRepositoryHygiene(const std::filesystem::path& root)
    -> std::vector<RepositoryFinding>;
[[nodiscard]] auto scanRepositoryHygiene(const std::filesystem::path& root,
                                         std::span<const std::filesystem::path> files)
    -> std::vector<RepositoryFinding>;

[[nodiscard]] auto scanArchitectureBoundaries(const std::filesystem::path& root)
    -> std::vector<RepositoryFinding>;
[[nodiscard]] auto scanArchitectureBoundaries(const std::filesystem::path& root,
                                              std::span<const std::filesystem::path> files)
    -> std::vector<RepositoryFinding>;

} // namespace bloom::quality
