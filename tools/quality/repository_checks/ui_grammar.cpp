#include "ui_grammar.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>

namespace bloom::quality {
namespace {
std::string codeOnly(std::string_view input) {
    std::string code(input);
    enum class State { Code, Line, Block, String, Character } state = State::Code;
    for (std::size_t i = 0; i < code.size(); ++i) {
        const char c = input[i];
        const char next = i + 1 < code.size() ? input[i + 1] : '\0';
        if (state == State::Code) {
            if (c == '/' && (next == '/' || next == '*')) {
                state = next == '/' ? State::Line : State::Block;
                code[i] = code[i + 1] = ' ';
                ++i;
            } else if (c == '"' || c == '\'') {
                state = c == '"' ? State::String : State::Character;
                code[i] = ' ';
            }
        } else {
            if (c != '\n')
                code[i] = ' ';
            if (state == State::Line && c == '\n')
                state = State::Code;
            else if (state == State::Block && c == '*' && next == '/') {
                code[++i] = ' ';
                state = State::Code;
            } else if (state == State::String || state == State::Character) {
                if (c == '\\' && i + 1 < code.size()) {
                    code[++i] = ' ';
                } else if ((state == State::String && c == '"') ||
                           (state == State::Character && c == '\''))
                    state = State::Code;
            }
        }
    }
    return code;
}
bool applicationSource(const std::string& path) {
    return (path.starts_with("src/ui/") || path.starts_with("apps/")) &&
           path.find("/tests/") == std::string::npos && !path.starts_with("src/ui/kit/") &&
           !path.starts_with("src/ui/include/bloom/ui/kit/") &&
           (path.ends_with(".cpp") || path.ends_with(".hpp"));
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
} // namespace

std::vector<RepositoryFinding> uiGrammarViolations(const std::filesystem::path& path,
                                                   std::string_view source) {
    std::vector<RepositoryFinding> findings;
    if (!applicationSource(path.generic_string()))
        return findings;
    const auto code = codeOnly(source);
    const auto append = [&](const std::regex& pattern, const std::string& kind) {
        for (std::sregex_iterator it(code.begin(), code.end(), pattern), end; it != end; ++it) {
            const auto offset = static_cast<std::size_t>(it->position());
            const auto line =
                1U + static_cast<std::size_t>(std::count(
                         code.begin(), code.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
            if (kind == "paint") {
                const auto text = it->str();
                // Exact canvas owners, never blanket file exemptions.
                const std::regex canvas(
                    R"((ViewerEditor|TimelineRuler|TimelineLaneRegion|TimelineWorkAreaRow|TimelineWorkAreaStrip|TimelineNavigator)::paintEvent)");
                if (std::regex_search(text, canvas))
                    continue;
                const auto prefix = code.substr(0, offset);
                const std::regex classes(R"(class\s+(\w+)\s+(?:final\s*)?:[^{}]*\{)");
                std::string owner;
                for (std::sregex_iterator cls(prefix.begin(), prefix.end(), classes), last;
                     cls != last; ++cls)
                    owner = (*cls)[1].str();
                if (owner == "TimelineKeyframeRow")
                    continue;
            }
            if (kind == "font" && std::regex_search(it->str(), std::regex(R"(QFont\s+\w+\s*\()"))) {
                const auto tail = code.substr(offset);
                if (std::regex_search(tail, std::regex(R"(^QFont\s+\w+\s*\([^;{}]*\)\s*\{)")))
                    continue;
            }
            findings.push_back({path, kind, "UI grammar: use the kit contract", line});
        }
    };
    // Construction through new, a temporary, a stack variable or a standard smart-pointer factory.
    append(
        std::regex(
            R"(\b(?:new\s+)?(?:QToolButton|QPushButton|QComboBox|QLineEdit|QCheckBox|QSlider|QLabel|QMenu)\s*(?:[({]|\s+[A-Za-z_]\w*\s*[({])|\bmake_(?:unique|shared)\s*<\s*(?:QToolButton|QPushButton|QComboBox|QLineEdit|QCheckBox|QSlider|QLabel|QMenu)\s*>)"),
        "raw-control");
    append(std::regex(R"(\bQFont\s*[({]|\bQFont\s+[A-Za-z_]\w*\s*[({])"), "font");
    append(
        std::regex(R"(\bvoid\s+(?:[A-Za-z_]\w*::)?paintEvent\s*\([^;{}]*\)\s*(?:override\s*)?\{)"),
        "paint");
    // Zero is a structural origin/empty margin. Nonzero geometric literals require tokens.
    append(
        std::regex(
            R"(\b(?:setFixed\w*|setMinimum(?:Size|Width|Height)|setMaximum(?:Size|Width|Height)|setContentsMargins|setSpacing|setGeometry|setIconSize|QSizeF?|QRectF?)\s*\([^;{}]*\b[1-9][0-9]*(?:\.[0-9]+)?\b)"),
        "dimension");
    append(std::regex(R"(\b[1-9][0-9]*(?:\.[0-9]+)?\s*\*\s*(?:kit::)?px\s*\()"), "dimension");
    append(
        std::regex(
            R"(\b(?:width|height|left|right|top|bottom|x|y)\s*\(\s*\)\s*[+\-*/]\s*[1-9][0-9]*(?:\.[0-9]+)?\b)"),
        "dimension");
    append(
        std::regex(
            R"(\b(?:int|qreal|double|float)\s+k?\w*(?:Width|Height|Size|Pitch|Gap|Inset|Radius|Padding|Extent)\w*\s*=\s*[1-9][0-9]*(?:\.[0-9]+)?\b)"),
        "dimension");
    std::ranges::sort(findings, {}, [](const auto& f) { return uiGrammarEntry(f); });
    const auto duplicates =
        std::ranges::unique(findings, {}, [](const auto& f) { return uiGrammarEntry(f); });
    findings.erase(duplicates.begin(), duplicates.end());
    return findings;
}
std::string uiGrammarEntry(const RepositoryFinding& finding) {
    return finding.path.generic_string() + ':' + std::to_string(finding.line.value_or(0)) + ':' +
           finding.category;
}
std::vector<RepositoryFinding> scanUiGrammar(const std::filesystem::path& root,
                                             std::span<const std::filesystem::path> files) {
    std::set<std::string> allowed;
    std::istringstream list(read(root / "tools/quality/ui_grammar_allowlist.txt"));
    std::string line;
    while (std::getline(list, line)) {
        if (line.empty() || line.starts_with('#'))
            continue;
        allowed.insert(line);
    }
    std::cout << "UI grammar allowlist: " << allowed.size() << " remaining entries (GRAMMAR-2)\n";
    std::vector<RepositoryFinding> result;
    for (const auto& path : files) {
        for (const auto& finding : uiGrammarViolations(path, read(root / path))) {
            if (!allowed.contains(uiGrammarEntry(finding)))
                result.push_back(finding);
        }
    }
    return result;
}
} // namespace bloom::quality
