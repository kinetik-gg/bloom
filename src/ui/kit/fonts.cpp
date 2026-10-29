#include <bloom/ui/kit/fonts.hpp>

#include <QByteArray>
#include <QFile>
#include <QFontDatabase>
#include <QLatin1StringView>

#include <array>
#include <optional>

namespace bloom::ui::kit {

Q_LOGGING_CATEGORY(kitFontsLog, "bloom.ui.kit.fonts")

namespace {

struct BundledFace {
    QLatin1StringView resourcePath;
    QLatin1StringView expectedFamily;
    bool isMonospace;
};

// One variable face per family: every type role is a named weight on the face's own 'wght' axis
// (kit::font()), so FreeType, CoreText and DirectWrite all draw the same outlines from the same
// file. No per-weight family exists to name.
constexpr auto kBundledFaces = std::to_array<BundledFace>({
    {QLatin1StringView(":/bloom/kit/inter/InterVariable.ttf"), QLatin1StringView("Inter Variable"),
     false},
    {QLatin1StringView(":/bloom/kit/geist-mono/GeistMono-Variable.ttf"),
     QLatin1StringView("Geist Mono"), true},
});

std::optional<BundledFontStatus>& memo() {
    static std::optional<BundledFontStatus> value;
    return value;
}

[[nodiscard]] BundledFontStatus loadBundledFaces() {
    BundledFontStatus status;
    // The system font database is populated first, deliberately. A face registered under a family,
    // foundry and style the system already has replaces the system face in Qt's database, but only
    // if the system faces are there already: registered into a database Qt has not populated yet,
    // the bundled face is overwritten by the system's own copy when Qt populates it later. A
    // workstation with its own "Geist Mono" (a different revision) would then draw that instead.
    (void)QFontDatabase::families();
    for (const auto& [resourcePath, expectedFamily, isMonospace] : kBundledFaces) {
        QFile file(resourcePath);
        if (!file.open(QIODevice::ReadOnly)) {
            status.diagnostics.append(
                QStringLiteral("bundled font %1 could not be read from the resource pack")
                    .arg(resourcePath));
            continue;
        }
        const QByteArray bytes = file.readAll();
        file.close();

        const int handle = QFontDatabase::addApplicationFontFromData(bytes);
        if (handle < 0) {
            status.diagnostics.append(
                QStringLiteral("bundled font %1 is not a font Qt can load").arg(resourcePath));
            continue;
        }
        const QStringList families = QFontDatabase::applicationFontFamilies(handle);
        if (families.isEmpty()) {
            status.diagnostics.append(
                QStringLiteral("bundled font %1 registered without naming a family")
                    .arg(resourcePath));
            continue;
        }
        const QString expected = QString(expectedFamily);
        if (!families.contains(expected)) {
            // The face loaded but is not the face this build expects -- a swapped or re-released
            // asset. Reported rather than trusted: the role does not ask for it and degrades to
            // the platform family.
            status.diagnostics.append(
                QStringLiteral("bundled font %1 registered as %2, not the expected %3")
                    .arg(resourcePath, families.join(QStringLiteral(", ")), expected));
            continue;
        }
        status.registeredFamilies.append(families);
        if (isMonospace) {
            status.monospaceRegistered = true;
        } else {
            status.interfaceRegistered = true;
        }
    }

    // Qt can report both the legacy and typographic family for a static face.
    status.registeredFamilies.removeDuplicates();

    for (const QString& diagnostic : status.diagnostics) {
        // Reported, then survived: a bundled font that will not load must never stop Bloom from
        // opening. The affected role falls back to the platform family below.
        qCWarning(kitFontsLog).noquote() << diagnostic;
    }
    if (!status.interfaceRegistered) {
        qCWarning(kitFontsLog).noquote()
            << QStringLiteral("no bundled interface face registered; falling back to the platform "
                              "sans-serif family");
    }
    if (!status.monospaceRegistered) {
        qCWarning(kitFontsLog).noquote()
            << QStringLiteral("no bundled monospaced face registered; falling back to the platform "
                              "monospace family");
    }
    return status;
}

[[nodiscard]] QString platformFallback(const QFontDatabase::SystemFont role) {
    return QFontDatabase::systemFont(role).family();
}

} // namespace

const BundledFontStatus& registerBundledFonts() {
    auto& value = memo();
    if (!value.has_value()) {
        value = loadBundledFaces();
    }
    return value.value();
}

const BundledFontStatus& bundledFontStatus() { return registerBundledFonts(); }

QStringList fontFamiliesForRole(const TypeRole role) {
    const BundledFontStatus& status = bundledFontStatus();
    const bool monospace = role == TypeRole::Value;
    QStringList families;
    // The bundled family first, then the platform's own family: the only time a platform font may
    // appear in chrome is when the bundled face failed to load (reported in registerBundledFonts).
    if (monospace ? status.monospaceRegistered : status.interfaceRegistered)
        families.append(monospace ? monospaceFontFamily() : interfaceFontFamily());
    families.append(
        platformFallback(monospace ? QFontDatabase::FixedFont : QFontDatabase::GeneralFont));
    families.removeDuplicates();
    return families;
}

bool isBundledFontFamily(const QString& family) {
    const BundledFontStatus& status = bundledFontStatus();
    return (status.interfaceRegistered && family == interfaceFontFamily()) ||
           (status.monospaceRegistered && family == monospaceFontFamily());
}

} // namespace bloom::ui::kit
