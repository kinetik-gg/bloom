#include "project_color_settings_dialog.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/host/project_color_choice.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/project_host.hpp>

#include <QPointer>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <optional>
#include <string_view>

namespace bloom::ui {
namespace {

// A digest for people: its first 12 hex digits; the whole digest is in the tooltip.
[[nodiscard]] QString shortDigest(const core::Sha256Digest& digest) {
    const auto hex = digest.toLowercaseHex();
    return QString::fromUtf8(hex.data(), 12);
}

[[nodiscard]] QString fullDigest(const core::Sha256Digest& digest) {
    const auto hex = digest.toLowercaseHex();
    return QString::fromUtf8(hex.data(), static_cast<qsizetype>(hex.size()));
}

// The settings `selected` (with `workingId`) describes, or nullopt with the reason it cannot.
struct Resolved final {
    std::optional<document::ColorSettings> settings;
    QString reason;
};

[[nodiscard]] Resolved resolve(const document::ColorSettings& original, const QString& currentKey,
                               const QString& selected, const std::string& workingId) {
    // Shared with Python's Host.set_color_settings and MCP's project.set-color-settings.
    const auto config = selected == currentKey ? host::ProjectColorConfig::Current
                        : selected == QStringLiteral("aces")
                            ? host::ProjectColorConfig::AcesCg
                            : host::ProjectColorConfig::BloomNeutral;
    auto resolved = host::resolveProjectColorSettings(original, config, workingId);
    if (resolved.settings)
        return {std::move(resolved.settings), {}};
    // The host's reason is English for every client; the dialog says it in the interface language.
    if (resolved.reason.find("ACES configuration is unavailable") != std::string::npos)
        return {std::nullopt, QObject::tr("The built-in ACES configuration is unavailable in this "
                                          "build, so it cannot be selected.")};
    return {std::nullopt, QObject::tr("The working color space \u201c%1\u201d is not offered by "
                                      "that configuration.")
                              .arg(QString::fromStdString(workingId))};
}

} // namespace

void showProjectColorSettingsDialog(ProjectHost& host, QWidget& owner) {
    const auto* current = host.colorSettings();
    if (current == nullptr)
        return;
    const auto original = *current;

    auto* dialog = new kit::KDialog(QObject::tr("Project Settings — Color"),
                                    QStringLiteral("projectColorSettingsDialog"), owner);
    auto* form = dialog->addForm();
    auto* config = new kit::KDropdown(dialog);
    config->setObjectName(QStringLiteral("projectColorConfigPicker"));
    config->setAccessibleName(QObject::tr("OCIO configuration"));
    config->addItem(QObject::tr("Bloom Neutral v1"), QStringLiteral("neutral"));
    config->addItem(QObject::tr("ACES 1.3 CG (OCIO built-in)"), QStringLiteral("aces"));
    const auto* const currentBuiltin =
        std::get_if<document::BuiltInOcioConfigLocator>(&original.ocioConfig.locator);
    const auto currentUri = currentBuiltin == nullptr ? std::string_view{} : currentBuiltin->uri;
    const auto currentKey = currentUri == color::kAcesCgV1ConfigUri ? QStringLiteral("aces")
                            : currentUri == color::kBloomNeutralV1ConfigUri
                                ? QStringLiteral("neutral")
                                : QStringLiteral("current");
    if (currentKey == QStringLiteral("current"))
        config->addItem(QObject::tr("External configuration (kept as it is)"), currentKey);
    config->setCurrentIndex(config->findData(currentKey));
    (void)form->addRow(QObject::tr("OCIO Config"), config);

    auto* working = new kit::KDropdown(dialog);
    working->setObjectName(QStringLiteral("projectWorkingColorSpacePicker"));
    working->setAccessibleName(QObject::tr("Working color space"));
    (void)form->addRow(QObject::tr("Working Color Space"), working);
    auto* summary = new kit::KLabel(dialog);
    summary->setObjectName(QStringLiteral("projectColorSummary"));
    summary->setWordWrap(true);
    summary->setTone(kit::Color::Muted);
    (void)form->addRow(summary);
    auto* hint = new kit::KLabel(
        QObject::tr("An external OCIO configuration is kept as it is. Bloom can switch the "
                    "project to one of its built-in configurations, but cannot edit or choose an "
                    "external one."),
        dialog, kit::TypeRole::UiSmall);
    hint->setObjectName(QStringLiteral("projectColorHint"));
    hint->setWordWrap(true);
    hint->setTone(kit::Color::Muted);
    dialog->contentLayout()->addWidget(hint);
    dialog->contentLayout()->addStretch(1);

    const auto populate = [config, working, summary, original, currentKey] {
        const auto selected = config->currentData().toString();
        const std::array<std::string_view, 5> acesSpaces{
            "ACEScg", "ACES2065-1", "Linear Rec.709 (sRGB)", "Linear P3-D65", "Linear Rec.2020"};
        working->clearItems();
        core::Sha256Digest digest = original.ocioConfig.expectedRevision.digest;
        QString name;
        QString displays;
        if (selected == QStringLiteral("current")) {
            working->addItem(QString::fromStdString(original.processColorSpaceId),
                             QString::fromStdString(original.processColorSpaceId));
            name = QObject::tr("External configuration");
        } else if (selected == QStringLiteral("aces")) {
            for (const auto id : acesSpaces)
                working->addItem(QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size())),
                                 QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size())));
            if (selected != currentKey)
                if (const auto revision = color::ocioBuiltInContentRevision(
                        color::OcioConfigLocatorKind::BloomBuiltIn, color::kAcesCgV1ConfigUri))
                    digest = *revision;
            name = QObject::tr("ACES 1.3 CG (cg-config v1.0.0, OCIO 2.1)");
            displays = QObject::tr("sRGB - Display: ACES 1.0 - SDR Video, Un-tone-mapped, Raw");
        } else {
            working->addItem(QStringLiteral("lin_rec709_scene"),
                             QStringLiteral("lin_rec709_scene"));
            if (selected != currentKey)
                digest = color::kBloomNeutralV1ConfigDigest;
            name = QObject::tr("Bloom Neutral v1");
            displays = QObject::tr("sRGB - Display");
        }
        QStringList lines{name};
        if (!displays.isEmpty())
            lines << QObject::tr("Displays: %1").arg(displays);
        lines << QObject::tr("Revision %1…").arg(shortDigest(digest));
        summary->setText(lines.join(QLatin1Char('\n')));
        summary->setToolTip(QObject::tr("SHA-256 %1").arg(fullDigest(digest)));
        if (selected == currentKey) {
            const auto previous = QString::fromStdString(original.processColorSpaceId);
            if (working->findData(previous) < 0)
                working->addItem(previous, previous);
            working->setCurrentIndex(working->findData(previous));
        } else {
            working->setCurrentIndex(0);
        }
    };
    QObject::connect(config, &kit::KDropdown::currentIndexChanged, dialog,
                     [populate] { populate(); });
    populate();
    dialog->addButton(kit::KDialog::Role::Accept, QObject::tr("OK"));
    dialog->addButton(kit::KDialog::Role::Reject, QObject::tr("Cancel"));
    dialog->setValidator([config, working, original, currentKey]() -> std::optional<kit::KRefusal> {
        const auto resolved = resolve(original, currentKey, config->currentData().toString(),
                                      working->currentData().toString().toStdString());
        if (!resolved.settings)
            return kit::KRefusal{config, resolved.reason};
        return std::nullopt;
    });
    dialog->watch(config);
    dialog->watch(working);
    dialog->setInitialFocus(config);
    QPointer<ProjectHost> guard(&host);
    QPointer<QWidget> window(&kit::KDialog::ownerWindow(owner));
    QPointer<kit::KDropdown> configField(config);
    QPointer<kit::KDropdown> workingField(working);
    dialog->open([=](const kit::KDialog::Role role) {
        if (role != kit::KDialog::Role::Accept || !guard || !configField || !workingField)
            return;
        const auto selected = configField->currentData().toString();
        const auto workingId = workingField->currentData().toString().toStdString();
        if (selected == currentKey && workingId == original.processColorSpaceId)
            return;
        auto resolved = resolve(original, currentKey, selected, workingId);
        if (!resolved.settings)
            return;
        const auto status = guard->setColorSettings(std::move(*resolved.settings));
        if (status == host::ProjectSessionColorSettingsStatus::InvalidSettings && window)
            kit::inform(*window, kit::KPromptTone::Error, QObject::tr("Project Settings"),
                        QObject::tr("The color configuration was not changed."),
                        QObject::tr("The selected color configuration could not be resolved."),
                        QStringLiteral("projectColorSettingsFailed"));
    });
}

} // namespace bloom::ui
