#include <bloom/ui/kit/surfaces.hpp>
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
    // Dead rules stay dead: the maximize button paints itself (KIconButton) and the custom title
    // bar is not installed, so the sheet carries neither.
    expectations.expect(!sheet.contains(QStringLiteral("maximizeAreaButton")),
                        "no rule for the self-painted maximize button");
    expectations.expect(!sheet.contains(QStringLiteral("kinetikTitleBar")),
                        "no rule for the uninstalled title bar");
    expectations.expect(!sheet.contains(QStringLiteral("font-size")) &&
                            !sheet.contains(QStringLiteral("font-weight")),
                        "type comes from TypeRole, never from the sheet");
}

// The panel border is drawn once, by KPanelFrame (Accent while active); the sheet gives the panel
// its background and no border of its own, so the two can never double up.
void testThePanelBorderIsDrawnOnce(Expectations& expectations) {
    const QString sheet = kit::kinetikStyleSheet();
    const qsizetype restRule = sheet.indexOf(QStringLiteral("QFrame#editorArea {"));
    expectations.expect(restRule >= 0, "the resting-area rule exists");
    if (restRule >= 0) {
        const QString restBlock = sheet.mid(restRule, sheet.indexOf('}', restRule) - restRule);
        expectations.expect(restBlock.contains(QStringLiteral("solid transparent;")),
                            "the sheet's panel border is transparent: it keeps the content inset "
                            "and draws nothing");
    }
    expectations.expect(!sheet.contains(QStringLiteral("QFrame#editorArea[active")),
                        "the active state is the frame overlay's, not a sheet rule");

    QWidget panel;
    panel.resize(40, 30);
    kit::KPanelFrame frame(&panel);
    const auto inkAt = [&frame](const QPoint point) {
        return frame.grab().toImage().pixelColor(point);
    };
    expectations.expect(inkAt({20, 0}) == kit::color(kit::Color::Border),
                        "a resting panel's single border is Border");
    frame.setActive(true);
    expectations.expect(inkAt({20, 0}) == kit::color(kit::Color::Accent) &&
                            inkAt({0, 15}) == kit::color(kit::Color::Accent),
                        "an active panel's single border is Accent");
    expectations.expect(inkAt({20, 15}).alpha() == 0, "the frame paints nothing inside the panel");
}

// The stock buttons wear the ONE button recipe: the generated QPushButton rules are the Secondary
// recipe, a dialog's default button is the Primary one, and keyboard focus has a rule of its own.
void testStockButtonsWearTheKitButtonRecipe(Expectations& expectations) {
    const QString sheet = kit::kinetikStyleSheet();
    // The generated state rules follow the template's box rules, so the LAST block for a selector
    // is the recipe's.
    const auto block = [&sheet](const QString& selector) {
        const qsizetype at = sheet.lastIndexOf(selector + QStringLiteral(" {"));
        return at < 0 ? QString{} : sheet.mid(at, sheet.indexOf('}', at) - at);
    };
    const QString rest = block(QStringLiteral("QPushButton"));
    expectations.expect(rest.contains(kit::hex(kit::Color::SurfaceRaised)) &&
                            rest.contains(kit::hex(kit::Color::Border)),
                        "a QPushButton rests on the Secondary plate and hairline");
    expectations.expect(
        block(QStringLiteral("QPushButton:default")).contains(kit::hex(kit::Color::Accent)),
        "the default push button wears the Primary Accent plate");
    expectations.expect(block(QStringLiteral("QPushButton[bloomKeyboardFocus=\"true\"]"))
                            .contains(kit::hex(kit::Color::Accent)),
                        "a keyboard-focused push button's border turns Accent");
    expectations.expect(block(QStringLiteral("QPushButton:default[bloomKeyboardFocus=\"true\"]"))
                            .contains(kit::hex(kit::Color::Foreground)),
                        "a focused default button's border is Foreground, never Accent on Accent");
    expectations.expect(block(QStringLiteral("QToolButton[headerMenuButton=\"true\"]:hover"))
                            .contains(kit::hex(kit::Color::Accent)),
                        "a menu-strip button hovers to the Accent bar, from the MenuItem recipe");
    expectations.expect(
        !block(
             QStringLiteral("QToolButton[headerMenuButton=\"true\"][bloomKeyboardFocus=\"true\"]"))
             .isEmpty(),
        "a menu-strip button has a keyboard-focus rule");
    // The dropdown closed field keeps ControlSurface and the Border hairline.
    const QString combo = block(QStringLiteral("QComboBox"));
    expectations.expect(combo.contains(kit::hex(kit::Color::ControlSurface)) &&
                            combo.contains(kit::hex(kit::Color::Border)),
                        "the dropdown closed field is ControlSurface with a Border hairline");
}

// Scrollbars keep one track width in every state, so hovering never relayouts a view; the thumb
// changes colour and fills the track instead.
void testScrollbarsKeepOneWidth(Expectations& expectations) {
    const QString sheet = kit::kinetikStyleSheet();
    expectations.expect(!sheet.contains(QStringLiteral("QScrollBar:vertical:hover")) &&
                            !sheet.contains(QStringLiteral("QScrollBar:horizontal:hover")),
                        "no scrollbar rule changes the bar's own width on hover");
    expectations.expect(sheet.contains(QStringLiteral("QScrollBar::handle:hover")) &&
                            sheet.contains(QStringLiteral("QScrollBar::handle:pressed")),
                        "the thumb has hover and pressed states");
    const qsizetype tip = sheet.indexOf(QStringLiteral("QToolTip {"));
    const QString tipBlock = tip < 0 ? QString{} : sheet.mid(tip, sheet.indexOf('}', tip) - tip);
    expectations.expect(tipBlock.contains(QStringLiteral("border-radius")),
                        "tooltips round with the popup radius");
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
    testThePanelBorderIsDrawnOnce(expectations);
    testStockButtonsWearTheKitButtonRecipe(expectations);
    testScrollbarsKeepOneWidth(expectations);
    testTokenExpansionResolvesNumbersAndColors(expectations);
    testThemedWidgetsResolveTheirTokenColors(application, expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
