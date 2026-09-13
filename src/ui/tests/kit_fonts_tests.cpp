#include <bloom/ui/kit/fonts.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QString>
#include <QStringList>

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

void testEveryBundledFaceIsInTheResourcePackAndRegisters(Expectations& expectations) {
    for (const char* path :
         {":/bloom/kit/dejavu-sans/DejaVuSans.ttf", ":/bloom/kit/dejavu-sans/DejaVuSans-Bold.ttf",
          ":/bloom/kit/dejavu-sans/DejaVuSans-Oblique.ttf",
          ":/bloom/kit/geist-mono/GeistMono-Regular.ttf",
          ":/bloom/kit/geist-mono/GeistMono-Medium.ttf"}) {
        expectations.expect(QFile::exists(QString::fromLatin1(path)),
                            std::string{"the bundled face is embedded: "} + path);
    }

    const auto& status = kit::registerBundledFonts();
    expectations.expect(status.diagnostics.isEmpty(),
                        "every bundled face loaded without a diagnostic: " +
                            status.diagnostics.join(QStringLiteral("; ")).toStdString());
    expectations.expect(status.interfaceRegistered, "the interface family registered");
    expectations.expect(status.monospaceRegistered, "the monospaced family registered");

    // Three DejaVu files, ONE family: the faces are styles of DejaVu Sans, not families of their
    // own (unlike the previous interface face, whose Medium and SemiBold cuts each registered a
    // family). So the family list is three names, and the styles are asserted separately below.
    for (const char* family : {"DejaVu Sans", "Geist Mono", "Geist Mono Medium"}) {
        expectations.expect(status.registeredFamilies.contains(QString::fromLatin1(family)),
                            std::string{"Qt registered the family "} + family);
        expectations.expect(QFontDatabase::families().contains(QString::fromLatin1(family)),
                            std::string{"the font database can see "} + family);
    }
    const QStringList interfaceStyles = QFontDatabase::styles(kit::interfaceFontFamily());
    for (const char* style : {"Book", "Bold", "Oblique"}) {
        expectations.expect(interfaceStyles.contains(QString::fromLatin1(style)),
                            std::string{"the interface family offers the style "} + style +
                                " (got " +
                                interfaceStyles.join(QStringLiteral(" | ")).toStdString() + ')');
    }
}

void testRegistrationIsIdempotent(Expectations& expectations) {
    const auto familiesBefore = kit::registerBundledFonts().registeredFamilies;
    const auto& again = kit::registerBundledFonts();
    expectations.expect(again.registeredFamilies == familiesBefore,
                        "re-registering returns the same result rather than duplicating faces");
    expectations.expect(again.registeredFamilies.size() == 3,
                        "the five shipped faces register exactly three distinct families, got " +
                            again.registeredFamilies.join(QStringLiteral(" | ")).toStdString());
    expectations.expect(again.registeredFamilies.count(kit::interfaceFontFamily()) == 1,
                        "the one interface family the three interface faces share is collapsed, "
                        "not listed once per file");
}

void testEveryTypeRoleResolvesToItsBundledFace(Expectations& expectations) {
    struct Case {
        kit::TypeRole role;
        const char* expectedFamily;
        const char* what;
    };
    for (const auto& [role, expectedFamily, what] :
         {Case{kit::TypeRole::Ui, "DejaVu Sans", "the UI role"},
          Case{kit::TypeRole::UiSmall, "DejaVu Sans", "the small UI role"},
          Case{kit::TypeRole::Title, "DejaVu Sans", "the title role"},
          Case{kit::TypeRole::Value, "Geist Mono", "the value role"}}) {
        const QFont font = kit::font(role);
        const QFontInfo info(font);
        // QFontInfo reports the family Qt actually resolved -- the real proof that the bundled
        // face is what will be drawn, rather than a request Qt quietly substituted away from.
        expectations.expect(info.family().startsWith(QString::fromLatin1(expectedFamily)),
                            std::string{what} +
                                " resolves to a bundled face, not a substitute "
                                "(resolved " +
                                info.family().toStdString() + ')');
        expectations.expect(!kit::fontFamiliesForRole(role).isEmpty(),
                            std::string{what} + " asks for at least one family");
    }
}

void testHeavierWeightsResolveToTheirOwnStaticFace(Expectations& expectations) {
    // Two different upstream cuts, two different rules. The interface faces are styles of one
    // family, so a role names the family and its own weight picks the face -- the failure to catch
    // here is a Title that silently renders at Book weight. The monospaced Medium face registers a
    // family of its own, so the value role must still name that face first or silently get Regular.
    const QStringList uiFamilies = kit::fontFamiliesForRole(kit::TypeRole::Ui);
    expectations.expect(uiFamilies.first() == kit::interfaceFontFamily(),
                        "the interface roles ask for the one interface family first");
    const QStringList titleFamilies = kit::fontFamiliesForRole(kit::TypeRole::Title);
    expectations.expect(titleFamilies.first() == kit::interfaceFontFamily(),
                        "the title role asks for the same interface family, not a weight-named one "
                        "the font database never registered");
    const QStringList valueFamilies = kit::fontFamiliesForRole(kit::TypeRole::Value);
    expectations.expect(valueFamilies.first() == QStringLiteral("Geist Mono Medium"),
                        "the monospaced value role asks for the Medium face first");

    // QFontInfo::weight() echoes the requested weight here, so it cannot prove which face Qt
    // rasterized. Advance width can: same family, same size, only the weight differs, and the Bold
    // face is wider than Book. If the Bold file had failed to register, both requests would land on
    // Book and these advances would be equal.
    const QString sample = QStringLiteral("Bloom Handgloves 0123");
    QFont bookRequest = kit::font(kit::TypeRole::Title);
    bookRequest.setWeight(QFont::Normal);
    const qreal boldAdvance =
        QFontMetricsF(kit::font(kit::TypeRole::Title)).horizontalAdvance(sample);
    const qreal bookAdvance = QFontMetricsF(bookRequest).horizontalAdvance(sample);
    expectations.expect(boldAdvance > bookAdvance,
                        "the 600-weight title role rasterizes the bundled Bold face, not Book "
                        "(bold advance " +
                            std::to_string(boldAdvance) + ", book advance " +
                            std::to_string(bookAdvance) + ')');
}

void testEveryRoleEndsInAPlatformFallback(Expectations& expectations) {
    // The contract is that a role's family list always terminates in something the platform is
    // guaranteed to have, so a bundled face that fails to load degrades instead of vanishing.
    const QString generalFallback = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    const QString fixedFallback = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    expectations.expect(kit::fontFamiliesForRole(kit::TypeRole::Ui).last() == generalFallback,
                        "the UI role falls back to the platform sans-serif family");
    expectations.expect(kit::fontFamiliesForRole(kit::TypeRole::Title).last() == generalFallback,
                        "the title role falls back to the platform sans-serif family");
    expectations.expect(kit::fontFamiliesForRole(kit::TypeRole::Value).last() == fixedFallback,
                        "the value role falls back to the platform monospace family");
    expectations.expect(kit::fontFamiliesForRole(kit::TypeRole::Ui).size() >= 2,
                        "a role never depends on the bundled asset alone");
}

void testTheInstallerRegistersFontsBeforeSettingTheApplicationFont(QApplication& application,
                                                                   Expectations& expectations) {
    kit::installKinetikTheme(application);
    const QFont applied = QApplication::font();
    expectations.expect(applied.families() == kit::fontFamiliesForRole(kit::TypeRole::Ui),
                        "the application font is the UI role's resolved family list");
    expectations.expect(QFontInfo(applied).family().startsWith(kit::interfaceFontFamily()),
                        "the very first widget already renders in the bundled interface family");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testEveryBundledFaceIsInTheResourcePackAndRegisters(expectations);
    testRegistrationIsIdempotent(expectations);
    testEveryTypeRoleResolvesToItsBundledFace(expectations);
    testHeavierWeightsResolveToTheirOwnStaticFace(expectations);
    testEveryRoleEndsInAPlatformFallback(expectations);
    testTheInstallerRegistersFontsBeforeSettingTheApplicationFont(application, expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
