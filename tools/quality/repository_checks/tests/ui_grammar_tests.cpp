#include "ui_grammar.hpp"
#include <iostream>
int main() {
    using bloom::quality::uiGrammarViolations;
    int failures = 0;
    const auto check = [&](std::string_view source, const std::string& kind, std::size_t count) {
        const auto findings = uiGrammarViolations("src/ui/example.cpp", source);
        std::size_t actual = 0;
        for (const auto& finding : findings)
            if (finding.category == kind)
                ++actual;
        if (actual != count) {
            std::cerr << "Expected " << count << ' ' << kind << " findings; got " << actual
                      << " in " << source << '\n';
            ++failures;
        }
    };
    check("new QToolButton(this);\nQLabel label{parent};\nauto p = std::make_unique<QMenu>();",
          "raw-control", 3);
    check("// new QComboBox(this)\n/* QFont(12) */\nauto text = \"new QSlider(parent)\";",
          "raw-control", 0);
    check("QLabel* label = nullptr;\nkit::makeMenu(this);\nnew kit::KDropdown(this);",
          "raw-control", 0);
    check("new\nQPushButton\n(parent);", "raw-control", 1);
    check("QFont tickFont() { return kit::font(role); }", "font", 0);
    check("QFont(\"foo\");\nQFont face{\"foo\"};", "font", 2);
    check("setFixedHeight(26);\nQSize(16, 20);\nconst int kRowWidth = 42;\nheight() - 8;",
          "dimension", 4);
    check("setFixedHeight(kit::px(kit::Size::Control));\nsetContentsMargins(0, 0, 0, 0);",
          "dimension", 0);
    check("void ViewerEditor::paintEvent(QPaintEvent*) {}\nvoid "
          "Arbitrary::paintEvent(QPaintEvent*) {}",
          "paint", 1);
    check("void paintEvent(QPaintEvent*) override {}", "paint", 1);
    if (!uiGrammarViolations("src/ui/kit/example.cpp", "new QToolButton(this);").empty())
        ++failures;
    if (!uiGrammarViolations("src/ui/tests/example.cpp", "new QToolButton(this);").empty())
        ++failures;
    const auto lines =
        uiGrammarViolations("apps/bloom/example.cpp", "// comment\n\nnew QSlider(this);");
    if (lines.size() != 1 || lines.front().line != 3)
        ++failures;
    return failures == 0 ? 0 : 1;
}
