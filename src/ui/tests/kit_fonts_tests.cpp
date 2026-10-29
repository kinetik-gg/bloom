#include <bloom/ui/kit/fonts.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHeaderView>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenuBar>
#include <QPainter>
#include <QRawFont>
#include <QScreen>
#include <QString>
#include <QStringList>

#include <cmath>
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

constexpr auto kInterface = ":/bloom/kit/inter/InterVariable.ttf";
constexpr auto kMonospace = ":/bloom/kit/geist-mono/GeistMono-Variable.ttf";

[[nodiscard]] QByteArray resourceBytes(const char* path) {
    QFile file(QString::fromLatin1(path));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

void testEveryBundledFaceIsInTheResourcePackAndRegisters(Expectations& expectations) {
    for (const char* path : {kInterface, kMonospace})
        expectations.expect(QFile::exists(QString::fromLatin1(path)),
                            std::string{"the bundled face is embedded: "} + path);
    // The static Inter faces are text-layer content for the render module only.
    for (const char* path :
         {":/bloom/kit/inter/Inter-Regular.ttf", ":/bloom/kit/inter/Inter-Medium.ttf",
          ":/bloom/kit/inter/Inter-SemiBold.ttf", ":/bloom/kit/geist-mono/GeistMono-Regular.ttf",
          ":/bloom/kit/geist-mono/GeistMono-Medium.ttf"})
        expectations.expect(!QFile::exists(QString::fromLatin1(path)),
                            std::string{"no static face is an interface resource: "} + path);

    const auto& status = kit::registerBundledFonts();
    expectations.expect(status.diagnostics.isEmpty(),
                        "every bundled face loaded without a diagnostic: " +
                            status.diagnostics.join(QStringLiteral("; ")).toStdString());
    expectations.expect(status.interfaceRegistered, "the interface family registered");
    expectations.expect(status.monospaceRegistered, "the monospaced family registered");
    expectations.expect(kit::interfaceFontFamily() == QStringLiteral("Inter Variable") &&
                            kit::monospaceFontFamily() == QStringLiteral("Geist Mono"),
                        "the chrome families are Inter Variable and Geist Mono");
    expectations.expect(status.registeredFamilies ==
                            QStringList({kit::interfaceFontFamily(), kit::monospaceFontFamily()}),
                        "exactly the two variable families register, got " +
                            status.registeredFamilies.join(QStringLiteral(" | ")).toStdString());
    for (const auto role : {kit::TypeRole::Ui, kit::TypeRole::UiSmall, kit::TypeRole::UiSmallStrong,
                            kit::TypeRole::Title}) {
        expectations.expect(kit::fontFamiliesForRole(role).first() == kit::interfaceFontFamily(),
                            "each interface role asks for Inter Variable first");
    }
    expectations.expect(kit::fontFamiliesForRole(kit::TypeRole::Value).first() ==
                            kit::monospaceFontFamily(),
                        "the value role asks for Geist Mono first");
    expectations.expect(kit::isBundledFontFamily(kit::interfaceFontFamily()) &&
                            kit::isBundledFontFamily(kit::monospaceFontFamily()) &&
                            !kit::isBundledFontFamily(QStringLiteral("DejaVu Serif")),
                        "the bundled-family check names exactly the two families");
}

// Every manifest member: the interface files from the resource pack, every file from the source
// tree (the static Inter faces live there for the render module only).
void testIntakeDigests(Expectations& expectations) {
    for (const char* component : {"inter", "geist-mono"}) {
        const QString name = QString::fromLatin1(component);
        QFile manifest(QStringLiteral(":/bloom/kit/%1/manifest.json").arg(name));
        expectations.expect(manifest.open(QIODevice::ReadOnly),
                            "the " + name.toStdString() + " manifest is embedded");
        const auto files = QJsonDocument::fromJson(manifest.readAll()).object()["files"].toObject();
        expectations.expect(!files.isEmpty(), "the manifest lists its files");
        for (auto it = files.begin(); it != files.end(); ++it) {
            const auto entry = it.value().toObject();
            QStringList sources{
                QStringLiteral(BLOOM_KIT_THIRD_PARTY_DIR "/%1/%2").arg(name, it.key())};
            if (entry["consumer"].toString() == QStringLiteral("ui"))
                sources.append(QStringLiteral(":/bloom/kit/%1/%2").arg(name, it.key()));
            for (const QString& source : sources) {
                QFile file(source);
                expectations.expect(file.open(QIODevice::ReadOnly),
                                    "manifest member is present: " + source.toStdString());
                const auto bytes = file.readAll();
                expectations.expect(bytes.size() == entry["bytes"].toInteger(),
                                    "intake byte size matches: " + source.toStdString());
                expectations.expect(
                    QString::fromLatin1(
                        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) ==
                        entry["sha256"].toString(),
                    "intake SHA-256 matches: " + source.toStdString());
            }
        }
    }
}

void testRegistrationIsIdempotent(Expectations& expectations) {
    const auto familiesBefore = kit::registerBundledFonts().registeredFamilies;
    const auto& again = kit::registerBundledFonts();
    expectations.expect(again.registeredFamilies == familiesBefore,
                        "re-registering returns the same result rather than duplicating faces");
}

void testEveryTypeRoleResolvesToItsBundledVariableFace(Expectations& expectations) {
    struct Case {
        kit::TypeRole role;
        const char* file;
        QString family;
        const char* what;
    };
    for (const auto& [role, file, family, what] :
         {Case{kit::TypeRole::Ui, kInterface, kit::interfaceFontFamily(), "the UI role"},
          Case{kit::TypeRole::UiSmall, kInterface, kit::interfaceFontFamily(), "the small role"},
          Case{kit::TypeRole::UiSmallStrong, kInterface, kit::interfaceFontFamily(),
               "the small strong role"},
          Case{kit::TypeRole::Title, kInterface, kit::interfaceFontFamily(), "the title role"},
          Case{kit::TypeRole::Value, kMonospace, kit::monospaceFontFamily(), "the value role"}}) {
        const QFont font = kit::font(role);
        // QFontInfo reports the family Qt actually resolved; the 'head' table proves the face is
        // the bundled file and not an identically named system install, and 'fvar' that it is the
        // variable face.
        expectations.expect(QFontInfo(font).family() == family,
                            std::string{what} + " resolves to " + family.toStdString() +
                                " (resolved " + QFontInfo(font).family().toStdString() + ')');
        const QRawFont raw = QRawFont::fromFont(font);
        expectations.expect(raw.fontTable("head") ==
                                QRawFont(resourceBytes(file), 12.0).fontTable("head"),
                            std::string{what} + " draws the bundled file, not a system copy");
        expectations.expect(!raw.fontTable("fvar").isEmpty(),
                            std::string{what} + " draws the variable face");
    }
}

// The ink a sample covers: a heavier instance of the same outlines covers more of it.
[[nodiscard]] qint64 inkOf(const QFont& font) {
    QImage image(480, 64, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setFont(font);
    painter.setPen(Qt::black);
    painter.drawText(image.rect(), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Bloom Handgloves 0123456789"));
    painter.end();
    qint64 ink = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            ink += qAlpha(image.pixel(x, y));
    return ink;
}

[[nodiscard]] QFont atWeight(QFont font, const int weight) {
    font.setWeight(static_cast<QFont::Weight>(weight));
    font.setVariableAxis(QFont::Tag("wght"), static_cast<float>(weight));
    return font;
}

void testRoleWeightsComeFromTheVariableAxis(Expectations& expectations) {
    const auto axis = [](const kit::TypeRole role, const QFont::Tag tag) {
        return kit::font(role).variableAxisValue(tag);
    };
    const QFont::Tag weight("wght");
    expectations.expect(std::abs(axis(kit::TypeRole::Ui, weight) - 500.0F) < 1e-3F &&
                            std::abs(axis(kit::TypeRole::Title, weight) - 600.0F) < 1e-3F &&
                            std::abs(axis(kit::TypeRole::UiSmallStrong, weight) - 600.0F) < 1e-3F &&
                            std::abs(axis(kit::TypeRole::Value, weight) - 500.0F) < 1e-3F,
                        "the roles name 500 and 600 on the wght axis");
    expectations.expect(std::abs(axis(kit::TypeRole::Ui, QFont::Tag("opsz")) - 14.0F) < 1e-3F,
                        "the interface roles pin Inter's text optical size");
    // Rasterised on this platform's engine (FreeType on Linux): the same size at 400, 500 and 600
    // must really draw three different weights, heaviest last.
    const QFont ui = kit::font(kit::TypeRole::Ui);
    const qint64 regular = inkOf(atWeight(ui, 400));
    const qint64 medium = inkOf(ui);
    const qint64 semiBold = inkOf(atWeight(ui, 600));
    expectations.expect(regular < medium && medium < semiBold,
                        "Inter Variable draws 400 < 500 < 600 (ink " + std::to_string(regular) +
                            ", " + std::to_string(medium) + ", " + std::to_string(semiBold) + ')');
    const QFont value = kit::font(kit::TypeRole::Value);
    expectations.expect(inkOf(atWeight(value, 400)) < inkOf(value),
                        "Geist Mono draws its 500 value role heavier than 400");
}

// Sizes are design pixels at every device pixel ratio and never follow the platform font size.
void testRoleSizesAreDesignPixels(Expectations& expectations) {
    const qreal dpi = QGuiApplication::primaryScreen()->logicalDotsPerInch();
    struct Case {
        kit::TypeRole role;
        qreal pixels;
    };
    QFont huge;
    huge.setPointSizeF(24.0);
    QGuiApplication::setFont(huge);
    for (const auto& [role, pixels] :
         {Case{kit::TypeRole::Ui, 12.0}, Case{kit::TypeRole::UiSmall, 10.5},
          Case{kit::TypeRole::UiSmallStrong, 10.5}, Case{kit::TypeRole::Value, 11.5},
          Case{kit::TypeRole::Title, 13.0}}) {
        const QFont font = kit::font(role);
        expectations.expect(std::abs(font.pointSizeF() * dpi / 72.0 - pixels) < 1e-6,
                            "a role is its design-pixel size whatever the platform font says (" +
                                std::to_string(pixels) + " px)");
    }
}

void testEveryRoleEndsInAPlatformFallback(Expectations& expectations) {
    // A role's family list always terminates in something the platform is guaranteed to have, so
    // a bundled face that fails to load degrades instead of vanishing.
    const QString generalFallback = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    const QString fixedFallback = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    expectations.expect(kit::fontFamiliesForRole(kit::TypeRole::Ui).last() == generalFallback,
                        "the UI role falls back to the platform sans-serif family");
    expectations.expect(kit::fontFamiliesForRole(kit::TypeRole::Title).last() == generalFallback,
                        "the title role falls back to the platform sans-serif family");
    expectations.expect(kit::fontFamiliesForRole(kit::TypeRole::Value).last() == fixedFallback,
                        "the value role falls back to the platform monospace family");
    expectations.expect(kit::fontFamiliesForRole(kit::TypeRole::Ui).size() == 2,
                        "a role asks for its bundled family and the platform fallback only");
}

void testTheInstallerSetsTheApplicationFont(QApplication& application, Expectations& expectations) {
    kit::installKinetikTheme(application);
    const QFont applied = QApplication::font();
    expectations.expect(applied.families() == kit::fontFamiliesForRole(kit::TypeRole::Ui),
                        "the application font is the UI role's resolved family list");
    expectations.expect(QFontInfo(applied).family() == kit::interfaceFontFamily(),
                        "the very first widget already renders in the bundled interface family");
}

} // namespace

// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks) -- QApplication owns its style.
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testEveryBundledFaceIsInTheResourcePackAndRegisters(expectations);
    testRegistrationIsIdempotent(expectations);
    testIntakeDigests(expectations);
    testEveryTypeRoleResolvesToItsBundledVariableFace(expectations);
    testRoleWeightsComeFromTheVariableAxis(expectations);
    testRoleSizesAreDesignPixels(expectations);
    testEveryRoleEndsInAPlatformFallback(expectations);
    testTheInstallerSetsTheApplicationFont(application, expectations);
    QApplication::setStyle(new kit::AltUnderlineProxyStyle());
    QMenuBar menuBar;
    QHeaderView tableHeader(Qt::Horizontal);
    for (QWidget* widget : {static_cast<QWidget*>(&menuBar), static_cast<QWidget*>(&tableHeader)}) {
        widget->ensurePolished();
        expectations.expect(
            widget->font().pixelSize() == kit::font(kit::TypeRole::Ui).pixelSize() &&
                QFontInfo(widget->font()).family() == kit::interfaceFontFamily(),
            "platform class fonts cannot override TypeRole after proxy installation");
    }
    return expectations.failures() == 0 ? 0 : 1;
}

// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
