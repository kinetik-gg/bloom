#include "checker_main.hpp"
#include "ui_grammar.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--list-violations") {
        const auto root = std::filesystem::current_path();
        for (const auto& path : bloom::quality::repositoryFiles(root)) {
            std::ifstream source(root / path);
            const std::string text{std::istreambuf_iterator<char>(source),
                                   std::istreambuf_iterator<char>()};
            for (const auto& finding : bloom::quality::uiGrammarViolations(path, text))
                std::cout << bloom::quality::uiGrammarEntry(finding) << '\n';
        }
        return 0;
    }
    return bloom::quality::runRepositoryChecker({argv, static_cast<std::size_t>(argc)},
                                                "UI grammar passed", "UI grammar failed",
                                                bloom::quality::scanUiGrammar);
}
