#include "schema_checks.hpp"

#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] auto parseRoot(const std::span<const char* const> arguments)
    -> std::filesystem::path {
    auto root = std::filesystem::current_path();
    for (auto index = std::size_t{1}; index < arguments.size(); ++index) {
        const std::string_view argument{arguments[index]};
        if (argument != "--root") {
            throw std::invalid_argument("unknown argument: " + std::string{argument});
        }
        ++index;
        if (index == arguments.size()) {
            throw std::invalid_argument("--root requires a path");
        }
        root = arguments[index];
    }
    return root;
}

} // namespace

auto main(const int count, const char* const* values) -> int {
    try {
        bloom::quality::checkProjectSchemas(
            parseRoot(std::span<const char* const>{values, static_cast<std::size_t>(count)}));
        std::cout << "Project schema check passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Project schema check failed: " << error.what() << '\n';
        return 1;
    }
}
