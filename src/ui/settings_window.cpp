#include "settings_window.hpp"

#include "properties_sections.hpp"

#include <bloom/ui/acceleration_status.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAbstractButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <utility>

namespace bloom::ui {
namespace {

constexpr double kBytesPerGiB = 1024.0 * 1024.0 * 1024.0;

constexpr auto kRestartBadgeObjectName = "settingsRestartBadge";

[[nodiscard]] QLabel* makeValueLabel(const QString& text, QWidget* parent) {
    auto* label = properties::makeReadOnlyValueLabel(kit::TypeRole::Value, parent);
    label->setText(text);
    return label;
}

[[nodiscard]] kit::KLabel* makeRestartBadge(QWidget* parent) {
    auto* badge = new kit::KLabel(QObject::tr("Restart"), parent, kit::TypeRole::UiSmall);
    badge->setObjectName(QLatin1StringView(kRestartBadgeObjectName));
    QPalette palette = badge->palette();
    palette.setColor(QPalette::WindowText, kit::color(kit::Color::Muted));
    badge->setPalette(palette);
    badge->setToolTip(
        QObject::tr("This value is read when Bloom starts and takes effect after restart."));
    return badge;
}

[[nodiscard]] QScrollArea* wrapInScroll(QWidget* content, const QString& objectName) {
    auto* scroll = new QScrollArea;
    scroll->setObjectName(objectName);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

} // namespace

SettingsWindow::SettingsWindow(const ApplicationPreferences& current,
                               const AccelerationStatusProvider* const accelerationStatus,
                               QWidget* parent)
    : QDialog(parent), draft_(current), applied_(current), accelerationStatus_(accelerationStatus) {
    setObjectName(QStringLiteral("settingsWindow"));
    setWindowTitle(tr("Settings"));
    resize(880, 620);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                             kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    root->setSpacing(kit::px(kit::Spacing::M));

    auto* body = new QHBoxLayout;
    body->setSpacing(kit::px(kit::Spacing::L));

    categoryRail_ = new kit::KRadioGroup(this);
    categoryRail_->setObjectName(QStringLiteral("settingsCategoryRail"));
    categoryRail_->setPresentation(kit::KRadioGroup::Presentation::Discrete);
    categoryRail_->setFixedWidth(180);
    body->addWidget(categoryRail_, 0);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("settingsPages"));
    body->addWidget(pages_, 1);
    root->addLayout(body, 1);

    const auto pages = buildPages();
    for (const auto& page : pages) {
        categoryRail_->addOption(page.title);
        pages_->addWidget(page.widget);
    }
    connect(categoryRail_, &kit::KRadioGroup::currentIndexChanged, pages_,
            [this](const int index) { pages_->setCurrentIndex(index); });
    if (!pages.empty()) {
        categoryRail_->setCurrentIndex(0);
        pages_->setCurrentIndex(0);
    }

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                 QDialogButtonBox::Apply | QDialogButtonBox::RestoreDefaults,
                             this);
    buttons->setObjectName(QStringLiteral("settingsButtonBox"));
    applyButton_ = buttons->button(QDialogButtonBox::Apply);
    applyButton_->setObjectName(QStringLiteral("settingsApplyButton"));
    if (auto* restore = buttons->button(QDialogButtonBox::RestoreDefaults); restore != nullptr) {
        restore->setObjectName(QStringLiteral("settingsRestoreDefaultsButton"));
        connect(restore, &QPushButton::clicked, this,
                [this] { setPreferences(defaultApplicationPreferences()); });
    }
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        commitPreferences();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(applyButton_, &QPushButton::clicked, this, [this] { commitPreferences(); });
    root->addWidget(buttons);

    refreshControls();
    updateApplyEnabled();
}

ApplicationPreferences SettingsWindow::preferences() const { return draft_; }

void SettingsWindow::setPreferences(const ApplicationPreferences& preferences) {
    draft_ = preferences;
    refreshControls();
    updateApplyEnabled();
}

void SettingsWindow::refreshControls() {
    for (const auto& refresh : refreshCallbacks_) {
        refresh();
    }
}

void SettingsWindow::commitPreferences() {
    applied_ = draft_;
    emit preferencesApplied(applied_);
    updateApplyEnabled();
}

void SettingsWindow::updateApplyEnabled() {
    if (applyButton_ != nullptr) {
        applyButton_->setEnabled(draft_ != applied_);
    }
}

// --- Row helpers -------------------------------------------------------------------------------

kit::KSwitch* SettingsWindow::addSwitchRow(QVBoxLayout* layout, QWidget* parent,
                                           const QString& objectName, const QString& label,
                                           const bool restartRequired) {
    auto* control = new kit::KSwitch(parent);
    control->setObjectName(objectName);
    control->setAccessibleName(label);
    auto* rowLabel = properties::makeRowLabel(label, parent);
    if (restartRequired) {
        properties::addRow(layout, parent, rowLabel, nullptr, {control, makeRestartBadge(parent)});
    } else {
        properties::addRow(layout, parent, rowLabel, nullptr, control);
    }
    return control;
}

kit::KDropdown* SettingsWindow::addDropdownRow(QVBoxLayout* layout, QWidget* parent,
                                               const QString& objectName, const QString& label) {
    auto* control = new kit::KDropdown(parent);
    control->setObjectName(objectName);
    control->setAccessibleName(label);
    properties::addRow(layout, parent, properties::makeRowLabel(label, parent), nullptr, control);
    return control;
}

kit::KValueField* SettingsWindow::addValueFieldRow(QVBoxLayout* layout, QWidget* parent,
                                                   const QString& objectName, const QString& label,
                                                   const double minimum, const double maximum,
                                                   const int decimals, const QString& unit,
                                                   const bool restartRequired) {
    properties::ValueCellSpec spec;
    spec.objectName = objectName;
    spec.accessibleName = label;
    spec.minimum = minimum;
    spec.maximum = maximum;
    spec.decimals = decimals;
    spec.singleStep = 1.0;
    spec.unit = unit;
    auto* field = properties::makeValueCell(spec, parent);
    auto* rowLabel = properties::makeRowLabel(label, parent);
    if (restartRequired) {
        properties::addRow(layout, parent, rowLabel, nullptr, {field, makeRestartBadge(parent)});
    } else {
        properties::addRow(layout, parent, rowLabel, nullptr, field);
    }
    return field;
}

// --- Pages -------------------------------------------------------------------------------------

std::vector<SettingsWindow::Page> SettingsWindow::buildPages() {
    std::vector<Page> pages;
    pages.push_back({QStringLiteral("general"), tr("General"), buildGeneralPage()});
    pages.push_back({QStringLiteral("memory"), tr("Memory & Caches"), buildMemoryPage()});
    pages.push_back({QStringLiteral("timeline"), tr("Timeline"), buildTimelinePage()});
    pages.push_back({QStringLiteral("nodes"), tr("Node Graph"), buildNodeGraphPage()});
    pages.push_back({QStringLiteral("viewer"), tr("Viewer"), buildViewerPage()});
    pages.push_back({QStringLiteral("performance"), tr("Performance"), buildPerformancePage()});
    return pages;
}

QWidget* SettingsWindow::buildGeneralPage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::S));

    auto* section = properties::makeSection(
        layout, content, QStringLiteral("settingsGeneralSection"), tr("Playback"),
        QStringLiteral("settings/sections/general/collapsed"));
    auto* rows = section->bodyLayout();

    auto* audio = addSwitchRow(rows, content, QStringLiteral("settingsAudioEnabledSwitch"),
                               tr("Audio playback"), false);
    audio->setChecked(draft_.audioEnabled);
    connect(audio, &QAbstractButton::toggled, this, [this](const bool enabled) {
        draft_.audioEnabled = enabled;
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, audio] {
        const QSignalBlocker blocker(audio);
        audio->setChecked(draft_.audioEnabled);
    });

    auto* loop = addSwitchRow(rows, content, QStringLiteral("settingsLoopPlaybackSwitch"),
                              tr("Loop playback"), false);
    loop->setChecked(draft_.loopPlayback);
    connect(loop, &QAbstractButton::toggled, this, [this](const bool enabled) {
        draft_.loopPlayback = enabled;
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, loop] {
        const QSignalBlocker blocker(loop);
        loop->setChecked(draft_.loopPlayback);
    });

    layout->addStretch(1);
    return wrapInScroll(content, QStringLiteral("settingsGeneralPage"));
}

QWidget* SettingsWindow::buildMemoryPage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::S));

    auto* notice = new kit::KLabel(
        tr("Memory budgets and the disk cache are read when Bloom starts. Changes here take effect "
           "after restart."),
        content);
    notice->setObjectName(QStringLiteral("settingsRestartNotice"));
    notice->setWordWrap(true);
    layout->addWidget(notice);

    auto* memory =
        properties::makeSection(layout, content, QStringLiteral("settingsMemorySection"),
                                tr("Memory"), QStringLiteral("settings/sections/memory/collapsed"));
    auto* memoryRows = memory->bodyLayout();

    auto* operation =
        addValueFieldRow(memoryRows, content, QStringLiteral("settingsOperationCacheBytesField"),
                         tr("Operation cache budget"), 0.0, 256.0, 2, QStringLiteral("GiB"), true);
    operation->setValue(static_cast<double>(draft_.operationCacheBytes) / kBytesPerGiB);
    connect(operation, &kit::KValueField::valueChanged, this, [this](const double value) {
        draft_.operationCacheBytes =
            static_cast<std::uint64_t>(std::llround(std::max(0.0, value) * kBytesPerGiB));
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, operation] {
        const QSignalBlocker blocker(operation);
        operation->setValue(static_cast<double>(draft_.operationCacheBytes) / kBytesPerGiB);
    });

    auto* ram =
        addValueFieldRow(memoryRows, content, QStringLiteral("settingsRamPreviewBytesField"),
                         tr("RAM preview budget"), 0.0, 256.0, 2, QStringLiteral("GiB"), true);
    ram->setValue(static_cast<double>(draft_.ramPreviewBytes) / kBytesPerGiB);
    connect(ram, &kit::KValueField::valueChanged, this, [this](const double value) {
        draft_.ramPreviewBytes =
            static_cast<std::uint64_t>(std::llround(std::max(0.0, value) * kBytesPerGiB));
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, ram] {
        const QSignalBlocker blocker(ram);
        ram->setValue(static_cast<double>(draft_.ramPreviewBytes) / kBytesPerGiB);
    });

    auto* hint =
        new kit::KLabel(tr("A budget of 0 GiB uses the machine-derived default."), content);
    hint->setObjectName(QStringLiteral("settingsMemoryHint"));
    hint->setWordWrap(true);
    memoryRows->addWidget(hint);

    auto* disk = properties::makeSection(
        layout, content, QStringLiteral("settingsMediaCacheSection"), tr("Media disk cache"),
        QStringLiteral("settings/sections/media-cache/collapsed"));
    auto* diskRows = disk->bodyLayout();

    auto* enabled =
        addSwitchRow(diskRows, content, QStringLiteral("settingsMediaDiskCacheEnabledSwitch"),
                     tr("Enable disk cache"), true);
    enabled->setChecked(draft_.mediaDiskCacheEnabled);
    connect(enabled, &QAbstractButton::toggled, this, [this](const bool value) {
        draft_.mediaDiskCacheEnabled = value;
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, enabled] {
        const QSignalBlocker blocker(enabled);
        enabled->setChecked(draft_.mediaDiskCacheEnabled);
    });

    auto* directory = new kit::KLineEdit(content);
    directory->setObjectName(QStringLiteral("settingsMediaDiskCacheDirectoryEdit"));
    directory->setAccessibleName(tr("Cache directory"));
    directory->setText(QString::fromStdString(draft_.mediaDiskCacheDirectory));
    auto* browse = new kit::KButton(tr("Browse…"), content);
    browse->setObjectName(QStringLiteral("settingsMediaDiskCacheBrowseButton"));
    connect(browse, &QPushButton::clicked, this, [this, directory] {
        const auto picked = QFileDialog::getExistingDirectory(this, tr("Choose a cache directory"),
                                                              directory->text());
        if (picked.isEmpty()) {
            return;
        }
        directory->setText(picked);
        draft_.mediaDiskCacheDirectory = picked.toStdString();
        updateApplyEnabled();
    });
    connect(directory, &QLineEdit::textChanged, this, [this](const QString& text) {
        draft_.mediaDiskCacheDirectory = text.toStdString();
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, directory] {
        const QSignalBlocker blocker(directory);
        directory->setText(QString::fromStdString(draft_.mediaDiskCacheDirectory));
    });
    properties::addRow(diskRows, content, properties::makeRowLabel(tr("Cache directory"), content),
                       nullptr, {directory, browse});

    auto* budget =
        addValueFieldRow(diskRows, content, QStringLiteral("settingsMediaDiskCacheBudgetField"),
                         tr("Disk cache budget"), 0.0, 1024.0, 2, QStringLiteral("GiB"), true);
    budget->setValue(static_cast<double>(draft_.mediaDiskCacheBudgetBytes) / kBytesPerGiB);
    connect(budget, &kit::KValueField::valueChanged, this, [this](const double value) {
        draft_.mediaDiskCacheBudgetBytes =
            static_cast<std::uint64_t>(std::llround(std::max(0.0, value) * kBytesPerGiB));
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, budget] {
        const QSignalBlocker blocker(budget);
        budget->setValue(static_cast<double>(draft_.mediaDiskCacheBudgetBytes) / kBytesPerGiB);
    });

    layout->addStretch(1);
    return wrapInScroll(content, QStringLiteral("settingsMemoryPage"));
}

QWidget* SettingsWindow::buildTimelinePage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::S));

    auto* section = properties::makeSection(
        layout, content, QStringLiteral("settingsTimelineSection"), tr("Timeline"),
        QStringLiteral("settings/sections/timeline/collapsed"));
    auto* rows = section->bodyLayout();

    auto* format = addDropdownRow(
        rows, content, QStringLiteral("settingsTimelineTimeFormatDropdown"), tr("Time format"));
    format->addItem(tr("Frames"), QStringLiteral("frames"));
    format->addItem(tr("Timecode"), QStringLiteral("timecode"));
    format->setCurrentIndex(
        format->findData(QLatin1String(timelineTimeFormatValue(draft_.timelineTimeFormat))));
    connect(format, &kit::KDropdown::currentIndexChanged, this, [this, format](int) {
        draft_.timelineTimeFormat =
            format->currentData().toString() == QLatin1StringView("timecode")
                ? TimelineTimeFormat::Timecode
                : TimelineTimeFormat::Frames;
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, format] {
        const QSignalBlocker blocker(format);
        format->setCurrentIndex(
            format->findData(QLatin1String(timelineTimeFormatValue(draft_.timelineTimeFormat))));
    });

    struct SwitchSpec final {
        const char* objectName;
        const char* label;
        bool ApplicationPreferences::* field;
    };
    const SwitchSpec switches[] = {
        {"settingsTimelineSnappingSwitch", QT_TR_NOOP("Snap to edges"),
         &ApplicationPreferences::timelineSnapping},
        {"settingsTimelineKeyframesSwitch", QT_TR_NOOP("Show keyframes"),
         &ApplicationPreferences::timelineKeyframesVisible},
        {"settingsTimelineGraphEditorSwitch", QT_TR_NOOP("Graph editor"),
         &ApplicationPreferences::timelineGraphEditor},
    };
    for (const auto& spec : switches) {
        auto* control =
            addSwitchRow(rows, content, QLatin1String(spec.objectName), tr(spec.label), false);
        control->setChecked(draft_.*(spec.field));
        connect(control, &QAbstractButton::toggled, this,
                [this, field = spec.field](const bool value) {
                    draft_.*field = value;
                    updateApplyEnabled();
                });
        refreshCallbacks_.push_back([this, control, field = spec.field] {
            const QSignalBlocker blocker(control);
            control->setChecked(draft_.*field);
        });
    }

    auto* width =
        addValueFieldRow(rows, content, QStringLiteral("settingsTimelineLayerColumnWidthField"),
                         tr("Layer column width"), 80.0, 600.0, 0, QStringLiteral("px"), false);
    width->setValue(static_cast<double>(draft_.timelineLayerColumnWidth));
    connect(width, &kit::KValueField::valueChanged, this, [this](const double value) {
        draft_.timelineLayerColumnWidth = static_cast<int>(std::llround(value));
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, width] {
        const QSignalBlocker blocker(width);
        width->setValue(static_cast<double>(draft_.timelineLayerColumnWidth));
    });

    layout->addStretch(1);
    return wrapInScroll(content, QStringLiteral("settingsTimelinePage"));
}

QWidget* SettingsWindow::buildNodeGraphPage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::S));

    auto* section = properties::makeSection(
        layout, content, QStringLiteral("settingsNodeGraphSection"), tr("Node Graph"),
        QStringLiteral("settings/sections/node-graph/collapsed"));
    auto* rows = section->bodyLayout();

    auto* linkStyle = addDropdownRow(rows, content, QStringLiteral("settingsNodeLinkStyleDropdown"),
                                     tr("Link style"));
    linkStyle->addItem(tr("Spline"), QStringLiteral("spline"));
    linkStyle->addItem(tr("Straight"), QStringLiteral("straight"));
    linkStyle->addItem(tr("Angled"), QStringLiteral("angled"));
    linkStyle->setCurrentIndex(
        linkStyle->findData(QLatin1String(nodeLinkStyleValue(draft_.nodeLinkStyle))));
    connect(linkStyle, &kit::KDropdown::currentIndexChanged, this, [this, linkStyle](int) {
        const auto data = linkStyle->currentData().toString();
        if (data == QLatin1StringView("straight"))
            draft_.nodeLinkStyle = NodeLinkStyle::Straight;
        else if (data == QLatin1StringView("angled"))
            draft_.nodeLinkStyle = NodeLinkStyle::Angled;
        else
            draft_.nodeLinkStyle = NodeLinkStyle::Spline;
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, linkStyle] {
        const QSignalBlocker blocker(linkStyle);
        linkStyle->setCurrentIndex(
            linkStyle->findData(QLatin1String(nodeLinkStyleValue(draft_.nodeLinkStyle))));
    });

    auto* snap = addSwitchRow(rows, content, QStringLiteral("settingsNodeSnapSwitch"),
                              tr("Snap to grid"), false);
    snap->setChecked(draft_.nodeSnap);
    connect(snap, &QAbstractButton::toggled, this, [this](const bool value) {
        draft_.nodeSnap = value;
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, snap] {
        const QSignalBlocker blocker(snap);
        snap->setChecked(draft_.nodeSnap);
    });

    auto* grid = addValueFieldRow(rows, content, QStringLiteral("settingsNodeGridSizeField"),
                                  tr("Grid size"), 2.0, 200.0, 0, QStringLiteral("px"), false);
    grid->setValue(draft_.nodeGridSize);
    connect(grid, &kit::KValueField::valueChanged, this, [this](const double value) {
        draft_.nodeGridSize = std::max(1.0, value);
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, grid] {
        const QSignalBlocker blocker(grid);
        grid->setValue(draft_.nodeGridSize);
    });

    layout->addStretch(1);
    return wrapInScroll(content, QStringLiteral("settingsNodeGraphPage"));
}

QWidget* SettingsWindow::buildViewerPage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::S));

    auto* section =
        properties::makeSection(layout, content, QStringLiteral("settingsViewerSection"),
                                tr("Viewer"), QStringLiteral("settings/sections/viewer/collapsed"));
    auto* rows = section->bodyLayout();

    auto* resolution = addDropdownRow(
        rows, content, QStringLiteral("settingsViewerResolutionDropdown"), tr("Resolution"));
    resolution->addItem(tr("Auto"), QStringLiteral("Auto"));
    resolution->addItem(tr("Full"), QStringLiteral("Full"));
    resolution->addItem(tr("Half"), QStringLiteral("Half"));
    resolution->addItem(tr("Quarter"), QStringLiteral("Quarter"));
    resolution->setCurrentIndex(
        resolution->findData(QLatin1String(viewerResolutionValue(draft_.viewerResolution))));
    connect(resolution, &kit::KDropdown::currentIndexChanged, this, [this, resolution](int) {
        const auto data = resolution->currentData().toString();
        if (data == QLatin1StringView("Full"))
            draft_.viewerResolution = ViewerResolutionPreference::Full;
        else if (data == QLatin1StringView("Half"))
            draft_.viewerResolution = ViewerResolutionPreference::Half;
        else if (data == QLatin1StringView("Quarter"))
            draft_.viewerResolution = ViewerResolutionPreference::Quarter;
        else
            draft_.viewerResolution = ViewerResolutionPreference::Auto;
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, resolution] {
        const QSignalBlocker blocker(resolution);
        resolution->setCurrentIndex(
            resolution->findData(QLatin1String(viewerResolutionValue(draft_.viewerResolution))));
    });

    auto* background = addDropdownRow(
        rows, content, QStringLiteral("settingsViewerBackgroundDropdown"), tr("Background"));
    background->addItem(tr("Solid"), QStringLiteral("Solid"));
    background->addItem(tr("Checkerboard"), QStringLiteral("Checkerboard"));
    background->addItem(tr("Black"), QStringLiteral("Black"));
    background->addItem(tr("White"), QStringLiteral("White"));
    background->setCurrentIndex(
        background->findData(QLatin1String(viewerBackgroundValue(draft_.viewerBackground))));
    connect(background, &kit::KDropdown::currentIndexChanged, this, [this, background](int) {
        const auto data = background->currentData().toString();
        if (data == QLatin1StringView("Checkerboard"))
            draft_.viewerBackground = ViewerBackgroundPreference::Checkerboard;
        else if (data == QLatin1StringView("Black"))
            draft_.viewerBackground = ViewerBackgroundPreference::Black;
        else if (data == QLatin1StringView("White"))
            draft_.viewerBackground = ViewerBackgroundPreference::White;
        else
            draft_.viewerBackground = ViewerBackgroundPreference::Solid;
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, background] {
        const QSignalBlocker blocker(background);
        background->setCurrentIndex(
            background->findData(QLatin1String(viewerBackgroundValue(draft_.viewerBackground))));
    });

    struct SwitchSpec final {
        const char* objectName;
        const char* label;
        bool ApplicationPreferences::* field;
    };
    const SwitchSpec overlays[] = {
        {"settingsViewerSafeAreasSwitch", QT_TR_NOOP("Safe areas"),
         &ApplicationPreferences::viewerSafeAreas},
        {"settingsViewerCentreCrossSwitch", QT_TR_NOOP("Centre cross"),
         &ApplicationPreferences::viewerCentreCross},
        {"settingsViewerThirdsSwitch", QT_TR_NOOP("Thirds"), &ApplicationPreferences::viewerThirds},
        {"settingsViewerRulersSwitch", QT_TR_NOOP("Rulers"), &ApplicationPreferences::viewerRulers},
        {"settingsViewerPixelGridSwitch", QT_TR_NOOP("Pixel grid"),
         &ApplicationPreferences::viewerPixelGrid},
    };
    for (const auto& spec : overlays) {
        auto* control =
            addSwitchRow(rows, content, QLatin1String(spec.objectName), tr(spec.label), false);
        control->setChecked(draft_.*(spec.field));
        connect(control, &QAbstractButton::toggled, this,
                [this, field = spec.field](const bool value) {
                    draft_.*field = value;
                    updateApplyEnabled();
                });
        refreshCallbacks_.push_back([this, control, field = spec.field] {
            const QSignalBlocker blocker(control);
            control->setChecked(draft_.*field);
        });
    }

    layout->addStretch(1);
    return wrapInScroll(content, QStringLiteral("settingsViewerPage"));
}

QWidget* SettingsWindow::buildPerformancePage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::S));

    const auto status = accelerationStatus_ != nullptr ? accelerationStatus_->accelerationStatus()
                                                       : cpuOnlyAccelerationStatus();

    auto* section = properties::makeSection(
        layout, content, QStringLiteral("settingsPerformanceSection"), tr("Acceleration"),
        QStringLiteral("settings/sections/performance/collapsed"));
    auto* rows = section->bodyLayout();

    const auto addStatusRow = [&](const QString& objectName, const QString& label,
                                  const QString& value) {
        auto* valueLabel = makeValueLabel(value, content);
        valueLabel->setObjectName(objectName);
        properties::addRow(rows, content, properties::makeRowLabel(label, content), nullptr,
                           valueLabel);
    };
    addStatusRow(QStringLiteral("settingsPerformanceBackendValue"), tr("Backend"), status.backend);
    addStatusRow(QStringLiteral("settingsPerformanceStateValue"), tr("Device state"),
                 status.deviceState);
    if (!status.deviceName.isEmpty()) {
        addStatusRow(QStringLiteral("settingsPerformanceDeviceValue"), tr("Device"),
                     status.deviceName);
    }
    if (!status.driver.isEmpty()) {
        addStatusRow(QStringLiteral("settingsPerformanceDriverValue"), tr("Driver"), status.driver);
    }

    auto* summary = new kit::KLabel(status.summary, content);
    summary->setObjectName(QStringLiteral("settingsPerformanceSummary"));
    summary->setWordWrap(true);
    rows->addWidget(summary);

    auto* note = new kit::KLabel(
        tr("GPU execution is an acceleration over the same evaluation; the CPU reference remains "
           "the correctness oracle."),
        content);
    note->setObjectName(QStringLiteral("settingsPerformanceNote"));
    note->setWordWrap(true);
    layout->addWidget(note);

    layout->addStretch(1);
    return wrapInScroll(content, QStringLiteral("settingsPerformancePage"));
}

} // namespace bloom::ui
