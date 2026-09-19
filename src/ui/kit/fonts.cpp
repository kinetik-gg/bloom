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

// Pinned static faces; role families name their weight explicitly on every Qt platform.
constexpr auto kBundledFaces = std::to_array<BundledFace>({
    {QLatin1StringView(":/bloom/kit/inter/Inter-Regular.ttf"), QLatin1StringView("Inter"), false},
    {QLatin1StringView(":/bloom/kit/inter/Inter-Medium.ttf"), QLatin1StringView("Inter Medium"),
     false},
    {QLatin1StringView(":/bloom/kit/inter/Inter-SemiBold.ttf"), QLatin1StringView("Inter SemiBold"),
     false},
    {QLatin1StringView(":/bloom/kit/geist-mono/GeistMono-Regular.ttf"),
     QLatin1StringView("Geist Mono"), true},
    {QLatin1StringView(":/bloom/kit/geist-mono/GeistMono-Medium.ttf"),
     QLatin1StringView("Geist Mono Medium"), true},
});

std::optional<BundledFontStatus>& memo() {
    static std::optional<BundledFontStatus> value;
    return value;
}

// Some platforms (CoreText on macOS) collapse a weight-specific static face into its base family
// and expose the weight as a style, so "Inter Medium" registers as "Inter" with style "Medium".
// The face is still the expected asset; only the family spelling differs. Stripping the weight
// suffix lets role resolution accept either spelling without treating normalisation as a swapped
// asset.
[[nodiscard]] QString baseFamilyFor(const QString& family) {
    for (const auto& suffix : {QStringLiteral(" Medium"), QStringLiteral(" SemiBold")}) {
        if (family.endsWith(suffix)) {
            return family.left(family.size() - suffix.size());
        }
    }
    return family;
}

[[nodiscard]] QString resolvedRoleFamily(const BundledFontStatus& status, const QString& expected) {
    if (status.registeredFamilies.contains(expected)) {
        return expected;
    }
    const QString base = baseFamilyFor(expected);
    if (base != expected && status.registeredFamilies.contains(base)) {
        return base;
    }
    return {};
}

[[nodiscard]] BundledFontStatus loadBundledFaces() {
    BundledFontStatus status;
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
    QString base = baseFamilyFor(expected);
        const bool acceptedAsExpected = families.contains(expected);
        const bool acceptedAsNormalised = base != expected && families.contains(base);
        if (!acceptedAsExpected && !acceptedAsNormalised) {
            // The face loaded but is not the face this build expects -- a swapped or re-released
            // asset. Reported rather than trusted: fontFamiliesForRole() will simply not offer a
            // family it cannot see, and the role degrades to its next choice.
            status.diagnostics.append(
                QStringLiteral("bundled font %1 registered as %2, not the expected %3")
                    .arg(resourcePath, families.join(QStringLiteral(", ")), expected));
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

// Appends `family` when the bundled set actually registered it, so a role never asks for a face
// that is not there and never ends up with Qt's own last-resort default by accident.
void appendIfRegistered(QStringList& families, const BundledFontStatus& status,
                        const QString& family) {
    if (status.registeredFamilies.contains(family)) {
        families.append(family);
    }
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
    QStringList families;

    switch (role) {
    case TypeRole::Ui:
    case TypeRole::UiSmall: {
        const QString medium = resolvedRoleFamily(status, QStringLiteral("Inter Medium"));
        if (!medium.isEmpty()) {
            families.append(medium);
        }
        appendIfRegistered(families, status, interfaceFontFamily());
        families.append(platformFallback(QFontDatabase::GeneralFont));
        break;
    }
    case TypeRole::Title: {
        const QString semiBold = resolvedRoleFamily(status, QStringLiteral("Inter SemiBold"));
        if (!semiBold.isEmpty()) {
            families.append(semiBold);
        }
        appendIfRegistered(families, status, interfaceFontFamily());
        families.append(platformFallback(QFontDatabase::GeneralFont));
        break;
    }
    case TypeRole::Value: {
        const QString medium = resolvedRoleFamily(status, QStringLiteral("Geist Mono Medium"));
        if (!medium.isEmpty()) {
            families.append(medium);
        }
        appendIfRegistered(families, status, monospaceFontFamily());
        families.append(platformFallback(QFontDatabase::FixedFont));
        break;
    }
    }
    families.removeDuplicates();
    return families;
}

} // namespace bloom::ui::kit
