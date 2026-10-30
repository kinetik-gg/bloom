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
    check("font.setPointSizeF(15);\nlabel->font().setBold(true);\nf.setFamilies({});\n"
          "f.setVariableAxis(tag, 1);\nQFontDatabase::systemFont(QFontDatabase::FixedFont);",
          "font", 5);
    check("layout->setStretch(0, 1);\nsetWeightedSizes(splitter, weights);", "font", 0);
    if (uiGrammarViolations("src/ui/example.ipp", "font.setPixelSize(9);\nsetFixedHeight(26);")
            .size() != 1)
        ++failures;
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
    check("QRect(0, row, width(), pitch), color(row % 2);", "dimension", 0);
    check("width() / 2.0; event->angleDelta().y() / 120;", "dimension", 0);
    check("new QLabel;\nnew QLineEdit;", "raw-control", 2);
    check("QSize(kit::px(kit::Size::Control), 20);", "dimension", 1);
    // Native dialogs and spin boxes.
    check("QMessageBox::warning(this, a, b);\nQFileDialog dialog;\nauto* c = new QColorDialog;\n"
          "QInputDialog::getText(this, a, b);\nQDialogButtonBox box;\nQSpinBox* s = nullptr;",
          "native-dialog", 6);
    check("kit::confirm(this, a);\nkit::chooseFile(options);", "native-dialog", 0);
    check("new QPlainTextEdit(this);\nQPlainTextEdit* base = nullptr;\nnew kit::KTextArea;",
          "raw-control", 1);
    // Colour literals.
    check("QColor(0, 0, 0, 150);\nQColor c(255, 0, 0);\nauto d = Qt::white;\n"
          "p.setPen(Qt::black);\nQColor::fromRgb(1, 2, 3);\nqRgb(1, 2, 3);",
          "colour-literal", 6);
    check("QColor(\"#112233\");\nauto s = \"rgb(1, 2, 3)\";\nauto t = \"rgba(1, 2, 3, 4)\";",
          "colour-literal", 3);
    check("kit::color(kit::Color::Accent);\nQColor(base);\nQt::transparent;\nQt::NoPen;\n"
          "auto s = \"#heading\"; // \"#112233\"",
          "colour-literal", 0);
    // Derived colours, palette overrides, style sheets.
    check("c.darker(120);\nc.lighter(135);\np->lighter(110);\nkit::pressedFillFor(c);",
          "colour-derive", 3);
    check("setPalette(colors);\ncolors.setColor(QPalette::Window, c);\npalette().setColor(QPalette::Text, c);",
          "palette", 3);
    check("palette();\nm.setColor(kit::Color::Accent);", "palette", 0);
    check("w->setStyleSheet(\"a\");\nsetStyleSheet(QString());", "stylesheet", 2);
    // Popups.
    check("QWidget(parent, Qt::Popup);\nQWidget(p, Qt::ToolTip | Qt::FramelessWindowHint);",
          "popup", 2);
    check("setFocus(Qt::PopupFocusReason);\nrole == Qt::ToolTipRole;", "popup", 0);
    // Ghost is the disclosure caret only.
    check("expand->setVariant(kit::KButton::Variant::Ghost);\n"
          "chevron_->setVariant(KButton::Variant::Ghost);",
          "ghost", 0);
    check("button->setVariant(kit::KButton::Variant::Ghost);\nauto v = Variant::Ghost;", "ghost", 2);
    check("button->setVariant(kit::KButton::Variant::Secondary);", "ghost", 0);
    // Literal pen widths.
    check("QPen pen(c, 1.0, Qt::DashLine);\nQPen(kit::color(Color::Accent), 2);\n"
          "p.setWidthF(1.5);\nstroke.setWidth(3);",
          "pen-width", 4);
    check("QPen pen(c, kit::kHairlineWidth);\nQPen(c);\nQPen(Qt::NoPen);\n"
          "QPen(c, kit::px(kit::Size::Hairline));\nstroke.setWidth(kit::px(kit::Size::NodeLinkHit));",
          "pen-width", 0);
    // Literal window sizes.
    check("resize(1600, 1000);\nw->resize(10, 20);\nbytes.resize(4U + n);\nresize(size);",
          "resize", 2);
    // The inline parts hold the same colour, palette and popup rules.
    if (uiGrammarViolations("src/ui/example.ipp", "QColor(0, 0, 0, 150);\nsetPalette(p);").size() != 2)
        ++failures;
    // The kit holds pixel literals to the token rule, except 1 and 2 and the tokens file.
    const auto kitCheck = [&](const std::string& file, std::string_view source, std::size_t count) {
        const auto found = uiGrammarViolations(file, source).size();
        if (found != count) {
            std::cerr << "Expected " << count << " kit findings in " << file << "; got " << found
                      << " for " << source << '\n';
            ++failures;
        }
    };
    kitCheck("src/ui/kit/example.cpp", "setFixedHeight(28);\nQSize(1, 1);\n3 * px(x);", 2);
    kitCheck("src/ui/kit/example.cpp", "setFixedHeight(px(Size::Control));\n2 * px(Spacing::XS);\n"
             "new QToolButton(this);\nQColor(1, 2, 3);", 0);
    kitCheck("src/ui/kit/tokens.cpp", "setFixedHeight(28);", 0);
    kitCheck("src/ui/include/bloom/ui/kit/tokens.hpp", "setFixedHeight(28);", 0);
    const auto lines =
        uiGrammarViolations("apps/bloom/example.cpp", "// comment\n\nnew QSlider(this);");
    if (lines.size() != 1 || lines.front().line != 3)
        ++failures;
    return failures == 0 ? 0 : 1;
}
