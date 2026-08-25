#include "checker_main.hpp"

#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace bloom::quality {
namespace {

struct CheckerArguments {
    std::filesystem::path root = std::filesystem::current_path();
    std::optional<std::filesystem::path> filesFrom;
};

[[nodiscard]] auto parseArguments(std::span<const char* const> arguments) -> CheckerArguments {
    auto result = CheckerArguments{};
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view argument{arguments[index]};
        if (argument == "--root") {
            ++index;
            if (index == arguments.size()) {
                throw std::invalid_argument("--root requires a path");
            }
            result.root = arguments[index];
            continue;
        }
        if (argument == "--files-from") {
            ++index;
            if (index == arguments.size()) {
                throw std::invalid_argument("--files-from requires a path");
            }
            result.filesFrom = arguments[index];
            continue;
        }
        throw std::invalid_argument("unknown argument: " + std::string{argument});
    }
    return result;
}

} // namespace

auto runRepositoryChecker(const std::span<const char* const> arguments,
                          const std::string_view successMessage,
                          const std::string_view failureMessage, const RepositoryScanner scanner)
    -> int {
    try {
        const auto options = parseArguments(arguments);
        const auto files = options.filesFrom
                               ? repositoryFilesFromNullManifest(options.root, *options.filesFrom)
                               : repositoryFiles(options.root);
        const auto findings = scanner(options.root, files);
        if (findings.empty()) {
            std::cout << successMessage << '\n';
            return 0;
        }

        std::cerr << failureMessage << " with " << findings.size() << " finding(s):\n";
        for (const auto& finding : findings) {
            std::cerr << "  " << finding.render() << '\n';
        }
        return 1;
    } catch (const std::exception& error) {
        std::cerr << failureMessage << ": " << error.what() << '\n';
        return 2;
    }
}

} // namespace bloom::quality
