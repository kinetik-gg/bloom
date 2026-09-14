#pragma once
#include "repository_checks.hpp"
#include <string_view>
namespace bloom::quality {
// Source-only violations; comments and string contents cannot manufacture findings.
[[nodiscard]] std::vector<RepositoryFinding> uiGrammarViolations(const std::filesystem::path& path,
                                                                 std::string_view source);
[[nodiscard]] std::vector<RepositoryFinding>
scanUiGrammar(const std::filesystem::path& root, std::span<const std::filesystem::path> files);
[[nodiscard]] std::string uiGrammarEntry(const RepositoryFinding& finding);
} // namespace bloom::quality
