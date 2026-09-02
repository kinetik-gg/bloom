#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QPalette>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using namespace bloom::ui;

void testInstalledPaletteAgreesWithTheColorTokens(QApplication& application,
                                                  Expectations& expectations) {
    kit::installKinetikTheme(application);
    const QPalette palette = QApplication::palette();

    const auto expectRole = [&expectations, &palette](const QPalette::ColorRole role,
                                                      const kit::Color token, const char* what) {
        expectations.expect(palette.color(QPalette::Active, role) == kit::color(token),
                            std::string{"the installed palette's "} + what + " is its token");
    };

    expectRole(QPalette::Window, kit::Color::Background, "Window");
    expectRole(QPalette::WindowText, kit::Color::Foreground, "WindowText");
    expectRole(QPalette::Base, kit::Color::Background, "Base");
    expectRole(QPalette::AlternateBase, kit::Color::Surface, "AlternateBase");
    expectRole(QPalette::Text, kit::Color::Foreground, "Text");
    expectRole(QPalette::Button, kit::Color::Surface, "Button");
    expectRole(QPalette::ButtonText, kit::Color::Foreground, "ButtonText");
    expectRole(QPalette::Highlight, kit::Color::Accent, "Highlight");
    expectRole(QPalette::HighlightedText, kit::Color::Foreground, "HighlightedText");
    expectRole(QPalette::PlaceholderText, kit::Color::Faint, "PlaceholderText");
    expectRole(QPalette::ToolTipBase, kit::Color::SurfaceRaised, "ToolTipBase");
    expectRole(QPalette::Link, kit::Color::Accent, "Link");

    // timeline_ruler.cpp is a palette-clean citizen: it draws its ruler baseline and its
    // keyframe-panel separator with QPalette::Mid against QPalette::Base. Mid must therefore stay
    // legible on Background -- Faint, never Border.
    expectRole(QPalette::Mid, kit::Color::Faint, "Mid");
    expectations.expect(palette.color(QPalette::Active, QPalette::Mid) !=
                            palette.color(QPalette::Active, QPalette::Base),
                        "the ruler separator role is distinguishable from the ruler background");

    const QColor disabledInk = palette.color(QPalette::Disabled, QPalette::WindowText);
    expectations.expect(disabledInk.rgb() == kit::color(kit::Color::Foreground).rgb(),
                        "disabled ink is the normal ink, faded");
    expectations.expect(
        std::abs(static_cast<double>(disabledInk.alphaF()) - kit::kDisabledOpacity) < 1e-3,
        "disabled ink is the normal ink at 40%");
}

void testTheInstallerIsReRunnable(QApplication& application, Expectations& expectations) {
    kit::installKinetikTheme(application);
    const QPalette first = QApplication::palette();
    const QString firstSheet = application.styleSheet();
    kit::installKinetikTheme(application);
    expectations.expect(QApplication::palette() == first,
                        "installing twice produces the same palette");
    expectations.expect(application.styleSheet() == firstSheet,
                        "installing twice produces the same stylesheet, not an accumulated one");
    expectations.expect(!firstSheet.isEmpty(), "the installer actually installs a stylesheet");
}

void testTheStyleSheetIsGeneratedEntirelyFromTokens(Expectations& expectations) {
    const QString sheet = kit::kinetikStyleSheet();

    expectations.expect(!sheet.contains('{') || !sheet.contains(QStringLiteral("{color.")),
                        "no color placeholder survives expansion");
    expectations.expect(!sheet.contains(QStringLiteral("{space.")),
                        "no spacing placeholder survives expansion");
    expectations.expect(!sheet.contains(QStringLiteral("{radius.")),
                        "no radius placeholder survives expansion");
    expectations.expect(!sheet.contains(QStringLiteral("{size.")),
                        "no size placeholder survives expansion");
    expectations.expect(!sheet.contains(QStringLiteral("{border.")),
                        "no border placeholder survives expansion");

    // Every hex value the sheet carries has to be a token value. This is the mechanical form of
    // "no duplicated hex literals anywhere": a rule cannot invent a color the tokens do not name.
    QSet<QString> tokenHexes;
    for (int index = 0; index <= static_cast<int>(kit::Color::DataAudio); ++index) {
        tokenHexes.insert(kit::hex(static_cast<kit::Color>(index)));
    }
    static const QRegularExpression hexPattern(QStringLiteral("#[0-9a-fA-F]{3,8}"));
    auto matches = hexPattern.globalMatch(sheet);
    int seen = 0;
    while (matches.hasNext()) {
        const QString found = matches.next().captured().toLower();
        ++seen;
        expectations.expect(tokenHexes.contains(found),
                            "stylesheet hex " + found.toStdString() + " is a design token");
    }
    expectations.expect(seen > 0, "the stylesheet does carry token colors");
}

void testTheStyleSheetPreservesEveryStyledObjectName(Expectations& expectations) {
    const QString sheet = kit::kinetikStyleSheet();
    // The objectNames the replaced MainWindow::applyFoundationTheme() blob keyed on are test
    // contracts elsewhere in the suite; the theme move may restate their appearance but must never
    // rename or drop one.
    for (const char* objectName :
         {"#editorArea", "#editorHeader", "#readOnlyPlaceholderPage", "#readOnlyPlaceholderHeading",
          "#readOnlyPlaceholderFileName", "#readOnlyPlaceholderBody"}) {
        expectations.expect(sheet.contains(QString::fromLatin1(objectName)),
                            std::string{"the generated sheet still styles "} + objectName);
    }
    expectations.expect(sheet.contains(QStringLiteral("QFrame#editorArea[active=\"true\"]")),
                        "the active-area border rule survives, property selector and all");
}

// task U8, issue #131, fix 1 and fix 2: the active-panel border is the new neutral BorderActive
// token, never Accent, and panel bodies take the smallest corner radius.
void testTheActivePanelBorderIsNeutralNotAccent(Expectations& expectations) {
    const QString sheet = kit::kinetikStyleSheet();
    const qsizetype activeRule =
        sheet.indexOf(QStringLiteral("QFrame#editorArea[active=\"true\"]"));
    expectations.expect(activeRule >= 0, "the active-area rule exists");
    if (activeRule < 0) {
        return;
    }
    const QString block = sheet.mid(activeRule, 80);
    expectations.expect(block.contains(kit::hex(kit::Color::BorderActive)),
                        "the active border paints BorderActive (#444444, formal amendment 1's "
                        "final value), not Accent");
    expectations.expect(!block.contains(kit::hex(kit::Color::Accent)),
                        "no accent color leaks into panel chrome");

    const qsizetype restRule = sheet.indexOf(QStringLiteral("QFrame#editorArea {"));
    expectations.expect(restRule >= 0, "the resting-area rule exists");
    if (restRule < 0) {
        return;
    }
    const QString restBlock = sheet.mid(restRule, 160);
    // task U8, formal amendment 1, A3: panel bodies moved from Radius::Small to their own
    // Radius::Panel (4) -- still the smallest step reserved for the panel body specifically.
    expectations.expect(
        restBlock.contains(
            QStringLiteral("border-radius: %1px;").arg(kit::radiusPx(kit::Radius::Panel, 0))),
        "panel bodies take Radius::Panel");
    expectations.expect(restBlock.contains(kit::hex(kit::Color::Border)),
                        "the resting border is the plain Border token");
}

// task U8, issue #131, formal amendment 1, A2: ControlSurface backs header-variant icon buttons
// and every dropdown closed field (here, the app-wide QComboBox QSS); Border (#222222) still
// borders panels, the header button, and the dropdown closed field, verified as asked.
void testControlSurfaceBacksHeaderButtonsAndDropdownFieldsBorderStaysConsistent(
    Expectations& expectations) {
    const QString sheet = kit::kinetikStyleSheet();

    const qsizetype maximizeRule =
        sheet.indexOf(QStringLiteral("QToolButton#maximizeAreaButton {"));
    expectations.expect(maximizeRule >= 0, "the header maximize button rule exists");
    if (maximizeRule >= 0) {
        const QString block = sheet.mid(maximizeRule, 160);
        expectations.expect(block.contains(kit::hex(kit::Color::ControlSurface)),
                            "the header button's background is ControlSurface");
        expectations.expect(block.contains(kit::hex(kit::Color::Border)),
                            "the header button borders with Border (#222222)");
    }

    const qsizetype comboRule = sheet.indexOf(QStringLiteral("QComboBox {"));
    expectations.expect(comboRule >= 0, "the app-wide QComboBox rule exists");
    if (comboRule >= 0) {
        const QString block = sheet.mid(comboRule, 220);
        expectations.expect(block.contains(kit::hex(kit::Color::ControlSurface)),
                            "the dropdown closed field's background is ControlSurface");
        expectations.expect(block.contains(kit::hex(kit::Color::Border)),
                            "the dropdown closed field borders with Border (#222222)");
    }

    const qsizetype panelRule = sheet.indexOf(QStringLiteral("QFrame#editorArea {"));
    expectations.expect(panelRule >= 0, "the panel rule exists");
    if (panelRule >= 0) {
        const QString block = sheet.mid(panelRule, 160);
        expectations.expect(block.contains(kit::hex(kit::Color::Border)),
                            "the panel borders with Border (#222222)");
    }
}

void testTokenExpansionResolvesNumbersAndColors(Expectations& expectations) {
    const QString expanded = kit::expandTokens(
        QStringLiteral("a{color.Accent}b{space.M}c{radius.Medium}d{size.Control}"));
    expectations.expect(expanded == QStringLiteral("a%1b12c6d26").arg(kit::hex(kit::Color::Accent)),
                        "token expansion resolves colors and numbers in place (got " +
                            expanded.toStdString() + ')');
}

void testThemedWidgetsResolveTheirTokenColors(QApplication& application,
                                              Expectations& expectations) {
    kit::installKinetikTheme(application);
    QWidget probe;
    probe.setObjectName(QStringLiteral("kitThemeProbe"));
    expectations.expect(probe.palette().color(QPalette::Window) ==
                            kit::color(kit::Color::Background),
                        "a freshly constructed widget inherits the Kinetik window color");
    expectations.expect(QApplication::font().families().contains(kit::interfaceFontFamily()),
                        "the installer sets the interface font application-wide");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testInstalledPaletteAgreesWithTheColorTokens(application, expectations);
    testTheInstallerIsReRunnable(application, expectations);
    testTheStyleSheetIsGeneratedEntirelyFromTokens(expectations);
    testTheStyleSheetPreservesEveryStyledObjectName(expectations);
    testTheActivePanelBorderIsNeutralNotAccent(expectations);
    testControlSurfaceBacksHeaderButtonsAndDropdownFieldsBorderStaysConsistent(expectations);
    testTokenExpansionResolvesNumbersAndColors(expectations);
    testThemedWidgetsResolveTheirTokenColors(application, expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
