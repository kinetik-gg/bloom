#include <bloom/ui/kit/fonts.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QJsonDocument>
#include <QJsonObject>
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
         {":/bloom/kit/inter/Inter-Regular.ttf", ":/bloom/kit/inter/Inter-Medium.ttf",
          ":/bloom/kit/inter/Inter-SemiBold.ttf", ":/bloom/kit/geist-mono/GeistMono-Regular.ttf",
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

    for (const char* family :
         {"Inter", "Inter Medium", "Inter SemiBold", "Geist Mono", "Geist Mono Medium"}) {
        expectations.expect(status.registeredFamilies.contains(QString::fromLatin1(family)),
                            std::string{"Qt registered the family "} + family);
    }
    const QStringList interfaceStyles = QFontDatabase::styles(kit::interfaceFontFamily());
    for (const char* style : {"Regular", "Medium", "SemiBold"}) {
        expectations.expect(interfaceStyles.contains(QString::fromLatin1(style)),
                            std::string{"the interface family offers the style "} + style +
                                " (got " +
                                interfaceStyles.join(QStringLiteral(" | ")).toStdString() + ')');
    }
}

void testIntakeDigests(Expectations& expectations) {
    QFile manifest(":/bloom/kit/inter/manifest.json");
    expectations.expect(manifest.open(QIODevice::ReadOnly), "Inter manifest is embedded");
    const auto files = QJsonDocument::fromJson(manifest.readAll()).object()["files"].toObject();
    expectations.expect(files.size() == 4, "manifest covers three faces and the license");
    for (auto it = files.begin(); it != files.end(); ++it) {
        QFile file(":/bloom/kit/inter/" + it.key());
        expectations.expect(file.open(QIODevice::ReadOnly), "manifest member is embedded");
        const auto bytes = file.readAll();
        const auto entry = it.value().toObject();
        expectations.expect(bytes.size() == entry["bytes"].toInteger(), "intake byte size matches");
        expectations.expect(
            QString::fromLatin1(
                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) ==
                entry["sha256"].toString(),
            "intake SHA-256 matches");
    }
}

void testRegistrationIsIdempotent(Expectations& expectations) {
    const auto familiesBefore = kit::registerBundledFonts().registeredFamilies;
    const auto& again = kit::registerBundledFonts();
    expectations.expect(again.registeredFamilies == familiesBefore,
                        "re-registering returns the same result rather than duplicating faces");
    expectations.expect(again.registeredFamilies.size() == 5,
                        "the five shipped faces register exactly five distinct families, got " +
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
         {Case{kit::TypeRole::Ui, "Inter", "the UI role"},
          Case{kit::TypeRole::UiSmall, "Inter", "the small UI role"},
          Case{kit::TypeRole::Title, "Inter", "the title role"},
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
    expectations.expect(kit::fontFamiliesForRole(kit::TypeRole::Ui).first() ==
                            QStringLiteral("Inter Medium"),
                        "UI asks for the bundled Medium static family");
    expectations.expect(kit::fontFamiliesForRole(kit::TypeRole::Title).first() ==
                            QStringLiteral("Inter SemiBold"),
                        "Title asks for the bundled SemiBold static family");
    const QStringList valueFamilies = kit::fontFamiliesForRole(kit::TypeRole::Value);
    expectations.expect(valueFamilies.first() == QStringLiteral("Geist Mono Medium"),
                        "the monospaced value role asks for the Medium face first");

    // Compare distinct static face advances; QFontInfo weight alone echoes the request.
    const QString sample = QStringLiteral("Bloom Handgloves 0123");
    QFont bookRequest = kit::font(kit::TypeRole::Title);
    bookRequest.setFamilies({kit::interfaceFontFamily()});
    bookRequest.setWeight(QFont::Normal);
    const qreal boldAdvance =
        QFontMetricsF(kit::font(kit::TypeRole::Title)).horizontalAdvance(sample);
    const qreal bookAdvance = QFontMetricsF(bookRequest).horizontalAdvance(sample);
    expectations.expect(
        boldAdvance > bookAdvance,
        "the 600-weight title role rasterizes the bundled SemiBold face, not Regular "
        "(bold advance " +
            std::to_string(boldAdvance) + ", book advance " + std::to_string(bookAdvance) + ')');
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
    testIntakeDigests(expectations);
    testEveryTypeRoleResolvesToItsBundledFace(expectations);
    testHeavierWeightsResolveToTheirOwnStaticFace(expectations);
    testEveryRoleEndsInAPlatformFallback(expectations);
    testTheInstallerRegistersFontsBeforeSettingTheApplicationFont(application, expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
