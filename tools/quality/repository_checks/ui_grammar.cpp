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
std::string codeOnly(std::string_view input, const bool keepStrings = false) {
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
                if (!(keepStrings && c == '"'))
                    code[i] = ' ';
            }
        } else {
            if (c != '\n' && !(keepStrings && state == State::String))
                code[i] = ' ';
            if (state == State::Line && c == '\n')
                state = State::Code;
            else if (state == State::Block && c == '*' && next == '/') {
                code[++i] = ' ';
                state = State::Code;
            } else if (state == State::String || state == State::Character) {
                if (c == '\\' && i + 1 < code.size()) {
                    ++i;
                    if (!(keepStrings && state == State::String))
                        code[i] = ' ';
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
// The kit's own implementation files hold the same pixel-literal rule as application code, except
// the file that defines the tokens.
bool kitSource(const std::string& path) {
    return path.starts_with("src/ui/kit/") && path.ends_with(".cpp") &&
           path != "src/ui/kit/tokens.cpp";
}
// The top-level arguments of the call whose '(' precedes `argumentsBegin`.
std::vector<std::string> callArguments(const std::string& code, std::size_t argumentsBegin) {
    std::vector<std::string> arguments;
    auto start = argumentsBegin;
    int depth = 1;
    for (auto index = argumentsBegin; index < code.size() && depth > 0; ++index) {
        const char c = code[index];
        if (c == '(' || c == '{' || c == '[')
            ++depth;
        if (c == ')' || c == '}' || c == ']')
            --depth;
        if ((c == ',' && depth == 1) || depth == 0) {
            arguments.push_back(code.substr(start, index - start));
            start = index + 1;
        }
    }
    return arguments;
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
} // namespace

std::vector<RepositoryFinding> uiGrammarViolations(const std::filesystem::path& path,
                                                   std::string_view source) {
    std::vector<RepositoryFinding> findings;
    // Inline implementation parts are held to the font rules only: chrome text everywhere takes
    // its font from a kit type role.
    const auto generic = path.generic_string();
    const bool inlinePart = generic.ends_with(".ipp") &&
                            applicationSource(generic.substr(0, generic.size() - 4) + ".cpp");
    const bool kitFile = kitSource(generic);
    if (!applicationSource(generic) && !inlinePart && !kitFile)
        return findings;
    const auto code = codeOnly(source);
    const auto lineAt = [&](const std::size_t offset) {
        return 1U + static_cast<std::size_t>(std::count(
                        code.begin(), code.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
    };
    // Dimension literals in a call's arguments: a nonzero literal is a token missing. Inside the
    // kit the literals 1 and 2 stay (a one-pixel minimum, both sides of a margin).
    const auto extentLiterals = [&](const bool inKit) {
        const std::regex extentCall(
            R"(\b(?:setFixed\w*|setMinimum(?:Size|Width|Height)|setMaximum(?:Size|Width|Height)|setContentsMargins|setSpacing|setGeometry|setIconSize|QSizeF?|QRectF?)\s*\()");
        const std::regex literal(R"(^\s*-?[1-9][0-9]*(?:\.[0-9]+)?\s*$)");
        const std::regex mathConstant(R"(^\s*-?[12](?:\.0)?\s*$)");
        for (std::sregex_iterator it(code.begin(), code.end(), extentCall), end; it != end; ++it) {
            const auto start = static_cast<std::size_t>(it->position());
            bool violation = false;
            for (const auto& argument :
                 callArguments(code, start + static_cast<std::size_t>(it->length())))
                violation = violation ||
                            (std::regex_match(argument, literal) &&
                             !(inKit && std::regex_match(argument, mathConstant)));
            if (violation)
                findings.push_back(
                    {path, "dimension", "UI grammar: use the kit contract", lineAt(start)});
        }
    };
    if (kitFile) {
        extentLiterals(true);
        const std::regex scaled(R"(\b(?:[3-9]|[1-9][0-9]+)(?:\.[0-9]+)?\s*\*\s*(?:kit::)?px\s*\()");
        for (std::sregex_iterator it(code.begin(), code.end(), scaled), end; it != end; ++it)
            findings.push_back({path, "dimension", "UI grammar: use the kit contract",
                                lineAt(static_cast<std::size_t>(it->position()))});
        return findings;
    }
    const auto appendIn = [&](const std::regex& pattern, const std::string& kind,
                              const std::string& text) {
        for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
            const auto offset = static_cast<std::size_t>(it->position());
            const auto line =
                1U + static_cast<std::size_t>(std::count(
                         code.begin(), code.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
            if (kind == "paint") {
                const auto matched = it->str();
                // Exact canvas owners, never blanket file exemptions.
                const std::regex canvas(
                    R"((ViewerEditor|TimelineRuler|TimelineLaneRegion|TimelineGraphView|TimelineWorkAreaRow|TimelineWorkAreaStrip|TimelineNavigator)::paintEvent)");
                if (std::regex_search(matched, canvas))
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
    const auto append = [&](const std::regex& pattern, const std::string& kind) {
        appendIn(pattern, kind, code);
    };
    // Construction through new, a temporary, a stack variable or a standard smart-pointer factory.
    append(
        std::regex(
            R"(\bnew\s+(?:QToolButton|QPushButton|QComboBox|QLineEdit|QCheckBox|QSlider|QLabel|QMenu|QPlainTextEdit|QTextEdit)\s*;)"),
        "raw-control");
    append(
        std::regex(
            R"(\b(?:new\s+)?(?:QToolButton|QPushButton|QComboBox|QLineEdit|QCheckBox|QSlider|QLabel|QMenu|QPlainTextEdit|QTextEdit)\s*(?:[({]|\s+[A-Za-z_]\w*\s*[({])|\bmake_(?:unique|shared)\s*<\s*(?:QToolButton|QPushButton|QComboBox|QLineEdit|QCheckBox|QSlider|QLabel|QMenu|QPlainTextEdit|QTextEdit)\s*>)"),
        "raw-control");
    append(std::regex(R"(\bQFont\s*[({]|\bQFont\s+[A-Za-z_]\w*\s*[({])"), "font");
    // A font is a role: no family, size, weight or style is set outside the kit, and no platform
    // font is asked for (visual-language.md, "Type roles").
    append(
        std::regex(
            R"((?:\.|->)set(?:Family|Families|PointSizeF?|PixelSize|Weight|Bold|Italic|StyleHint|VariableAxis|LetterSpacing|WordSpacing|Capitalization)\s*\(|\bQFontDatabase\s*::\s*(?:systemFont|addApplicationFont\w*)\b)"),
        "font");
    // Bloom owns its dialogs: the Qt convenience dialogs and spin boxes are never used outside the
    // kit (confirm, promptText, promptRange, chooseFile and the task dialog replace them).
    append(
        std::regex(
            R"(\bQ(?:MessageBox|InputDialog|FileDialog|ColorDialog|FontDialog|DialogButtonBox|SpinBox|DoubleSpinBox)\b)"),
        "native-dialog");
    // Colours come from tokens: no numeric QColor, no literal colour string, no Qt colour name.
    append(std::regex(R"(\bQColor(?:\s+[A-Za-z_]\w*)?\s*[({]\s*(?:[0-9]|0x)|\bQColor::fromRgb\w*\s*\(\s*[0-9]|\bqRgba?\s*\(\s*[0-9]|\bQt::(?:black|white|red|green|blue|cyan|magenta|yellow|gray|darkGray|lightGray)\b)"),
           "colour-literal");
    appendIn(std::regex(R"("\s*(?:#[0-9a-fA-F]{3,8}|rgba?\s*\()[^"]*")"), "colour-literal",
             codeOnly(source, true));
    // A colour is never derived by hand, and the palette is never overridden: surfaces fill from a
    // token (kit::KSurface) and states come from the kit's colour functions.
    append(std::regex(R"((?:\.|->)\s*(?:darker|lighter)\s*\()"), "colour-derive");
    append(std::regex(R"(\bsetPalette\s*\(|\.\s*setColor\s*\(\s*QPalette::)"), "palette");
    append(std::regex(R"(\bsetStyleSheet\s*\()"), "stylesheet");
    // A popup is a kit::KPopupWindow; a raw Qt::Popup/Qt::ToolTip window is not.
    append(std::regex(R"(\bQt::(?:Popup|ToolTip)\b)"), "popup");
    // Ghost is the disclosure-caret exception only (ui-grammar.md, "Buttons").
    {
        const std::regex ghost(R"(\bVariant::Ghost\b)");
        const std::regex disclosure(
            R"((\w+)\s*(?:\.|->)\s*setVariant\s*\(\s*(?:\w+::)*$)");
        for (std::sregex_iterator it(code.begin(), code.end(), ghost), end; it != end; ++it) {
            const auto offset = static_cast<std::size_t>(it->position());
            const auto head = code.substr(offset > 160 ? offset - 160 : 0,
                                          offset > 160 ? 160 : offset);
            std::smatch receiver;
            if (std::regex_search(head, receiver, disclosure) &&
                std::regex_search(receiver[1].str(),
                                  std::regex(R"(^(?:expand|chevron|caret|disclosure)\w*$)")))
                continue;
            findings.push_back({path, "ghost", "UI grammar: use the kit contract", lineAt(offset)});
        }
    }
    // A pen width is a stroke token (kit::kHairlineWidth, kEmphasisStroke ...), never a literal.
    {
        const std::regex penCall(R"(\bQPen\s*(?:[A-Za-z_]\w*\s*)?\()");
        const std::regex number(R"(^\s*[0-9]+(?:\.[0-9]+)?f?\s*$)");
        for (std::sregex_iterator it(code.begin(), code.end(), penCall), end; it != end; ++it) {
            const auto arguments = callArguments(
                code, static_cast<std::size_t>(it->position() + it->length()));
            if (arguments.size() > 1 && std::regex_match(arguments[1], number))
                findings.push_back({path, "pen-width", "UI grammar: use the kit contract",
                                    lineAt(static_cast<std::size_t>(it->position()))});
        }
        append(std::regex(R"((?:\.|->)\s*setWidthF?\s*\(\s*[0-9]+(?:\.[0-9]+)?\s*\))"), "pen-width");
    }
    append(std::regex(R"(\bresize\s*\(\s*[1-9][0-9]*\s*,)"), "resize");
    if (inlinePart) {
        std::erase_if(findings, [](const RepositoryFinding& finding) {
            return finding.category == "raw-control" || finding.category == "dimension" ||
                   finding.category == "paint";
        });
        return findings;
    }
    append(
        std::regex(R"(\bvoid\s+(?:[A-Za-z_]\w*::)?paintEvent\s*\([^;{}]*\)\s*(?:override\s*)?\{)"),
        "paint");
    // Zero is a structural origin/empty margin. Nonzero geometric literals require tokens.
    append(
        std::regex(
            R"(\b(?:setFixed\w*|setMinimum(?:Size|Width|Height)|setMaximum(?:Size|Width|Height)|setContentsMargins|setSpacing|setGeometry|setIconSize|QSizeF?|QRectF?)\s*\([^;{}()]*\b[1-9][0-9]*(?:\.[0-9]+)?\b)"),
        "dimension");
    append(std::regex(R"(\b[1-9][0-9]*(?:\.[0-9]+)?\s*\*\s*(?:kit::)?px\s*\()"), "dimension");
    append(
        std::regex(
            R"(\b(?:width|height|left|right|top|bottom|x|y)\s*\(\s*\)\s*[+\-]\s*[1-9][0-9]*(?:\.[0-9]+)?\b)"),
        "dimension");
    append(
        std::regex(
            R"(\b(?:int|qreal|double|float)\s+k?\w*(?:Width|Height|Size|Pitch|Gap|Inset|Radius|Padding|Extent)\w*\s*=\s*[1-9][0-9]*(?:\.[0-9]+)?\b)"),
        "dimension");
    extentLiterals(false);
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
    if (!allowed.empty())
        result.push_back({"tools/quality/ui_grammar_allowlist.txt", "allowlist",
                          "UI grammar debt must remain zero", std::nullopt});
    for (const auto& path : files) {
        for (const auto& finding : uiGrammarViolations(path, read(root / path))) {
            result.push_back(finding);
        }
    }
    return result;
}
} // namespace bloom::quality
