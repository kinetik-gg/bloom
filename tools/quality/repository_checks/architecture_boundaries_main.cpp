#include "checker_main.hpp"

#include <span>

auto main(const int argumentCount, const char* const* arguments) -> int {
    return bloom::quality::runRepositoryChecker(
        std::span{arguments, static_cast<std::size_t>(argumentCount)},
        "Architecture boundary check passed", "Architecture boundary check failed",
        static_cast<bloom::quality::RepositoryScanner>(bloom::quality::scanArchitectureBoundaries));
}
