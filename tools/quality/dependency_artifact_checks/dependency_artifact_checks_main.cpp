#include "dependency_artifact_checks.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

[[nodiscard]] auto repositoryRoot(const int argumentCount, char** arguments)
    -> std::filesystem::path {
    if (argumentCount == 1) {
        return std::filesystem::current_path();
    }
    if (argumentCount == 3 && std::string_view(arguments[1]) == "--root") {
        return arguments[2];
    }
    throw std::runtime_error("usage: bloom_dependency_artifact_check [--root <repository>]");
}

} // namespace

int main(const int argumentCount, char** arguments) {
    try {
        const auto result =
            bloom::quality::dependencies::checkRepository(repositoryRoot(argumentCount, arguments));
        std::cout << "Synthetic dependency contract check passed (lock " << result.lockVector
                  << ", prefix " << result.prefixVector << ")\n";
        if (result.productionLockPresent) {
            std::cout << "Production dependency lock validated (identity "
                      << result.productionLockIdentity << ")\n";
        } else {
            std::cout << "Production dependency lock absent; Unicode 15.1 bootstrap allowlist "
                         "verified\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dependency artifact check failed: " << error.what() << '\n';
        return 1;
    }
}
