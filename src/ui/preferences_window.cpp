#include "preferences_window.hpp"

#include "properties_sections.hpp"

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAbstractButton>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/file_chooser.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace bloom::ui {
namespace {

constexpr double kBytesPerGiB = 1024.0 * 1024.0 * 1024.0;
constexpr auto kMaximumBudgetBytes = std::min<std::uint64_t>(
    std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::size_t>::max());
constexpr double kMaximumBudgetGiB = static_cast<double>(kMaximumBudgetBytes) / kBytesPerGiB;

[[nodiscard]] std::uint64_t budgetBytesFromGiB(const double value) {
    const auto bytes = std::round(static_cast<long double>(std::max(0.0, value)) *
                                  static_cast<long double>(kBytesPerGiB));
    return bytes >= static_cast<long double>(kMaximumBudgetBytes)
               ? kMaximumBudgetBytes
               : static_cast<std::uint64_t>(bytes);
}

constexpr auto kRestartBadgeObjectName = "preferencesRestartBadge";

// The category the last Preferences window showed, for this session.
int gLastPreferencesPage = 0;

[[nodiscard]] kit::KLabel* makeRestartBadge(QWidget* parent) {
    auto* badge = new kit::KLabel(QObject::tr("Restart"), parent, kit::TypeRole::UiSmall);
    badge->setObjectName(QLatin1StringView(kRestartBadgeObjectName));
    badge->setTone(kit::Color::Muted);
    badge->setToolTip(
        QObject::tr("This value is read when Bloom starts and takes effect after restart."));
    return badge;
}

// A sidebar category row: the kit selected-row recipe with its leading Accent edge (a list view
// paints no row panel, so the edge is the delegate's), the style's hover wash and keyboard-focus
// inset otherwise. A group gap paints nothing.
class RailDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        if ((index.flags() & Qt::ItemIsEnabled) == 0)
            return;
        QStyleOptionViewItem item(option);
        if ((option.state & QStyle::State_Selected) != 0) {
            kit::paintSelectedRow(*painter, QRectF(option.rect), kit::Color::Background, true);
            item.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver);
        }
        QStyledItemDelegate::paint(painter, item, index);
    }
};

// A preference value of one width per kind, so a column of rows lines up.
void setValueWidth(QWidget& control) {
    control.setFixedWidth(kit::px(kit::Size::DropdownWidthWide));
}

// Muted helper text under the row it explains, in the value column.
void addHelperRow(QVBoxLayout* layout, QWidget* parent, kit::KLabel* helper) {
    auto* holder = new QWidget(parent);
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(
        kit::px(kit::Spacing::RowPadding) + kit::px(kit::Size::PreferencesLabelWidth) +
            kit::px(kit::Spacing::PropertyGutter),
        0, kit::px(kit::Spacing::RowPadding) + kit::px(kit::Size::PropertiesDiamondColumn),
        kit::px(kit::Spacing::XXS));
    helper->setParent(holder);
    helper->setTypeRole(kit::TypeRole::UiSmall);
    helper->setTone(kit::Color::Muted);
    helper->setWordWrap(true);
    row->addWidget(helper, 1);
    layout->addWidget(holder);
}

} // namespace

QScrollArea* PreferencesWindow::wrapPage(QWidget* content, const QString& objectName) {
    auto* scroll = new QScrollArea;
    scroll->setObjectName(objectName);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // A page is a Properties panel body: the panel Background behind Properties sections.
    auto* surface = new kit::KSurface(nullptr, kit::Color::Background);
    auto* surfaceLayout = new QVBoxLayout(surface);
    surfaceLayout->setContentsMargins(0, 0, 0, 0);
    surfaceLayout->addWidget(content);
    scroll->setWidget(surface);
    return scroll;
}

// The Properties panel's own content margins and section spacing (properties_editor.cpp).
void PreferencesWindow::setPageMargins(QVBoxLayout& layout) {
    layout.setContentsMargins(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::Gutter),
                              kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XS));
    layout.setSpacing(kit::px(kit::Spacing::Gutter));
}

QWidget* PreferencesWindow::addPreferencesRow(QVBoxLayout* layout, QWidget* parent,
                                              const QString& label,
                                              const std::initializer_list<QWidget*> controls) {
    // The Properties panel's own row: right-aligned label in the shared label column, values in
    // the value column.
    auto* row =
        new kit::KPropertyRow(properties::makeRowLabel(label, parent), nullptr, controls, parent);
    row->setLabelWidth(kit::px(kit::Size::PreferencesLabelWidth));
    for (auto* control : controls)
        if (qobject_cast<kit::KDropdown*>(control) != nullptr)
            setValueWidth(*control);
    layout->addWidget(row);
    return row;
}

kit::KValueField* PreferencesWindow::addAutomaticRow(QVBoxLayout* layout, QWidget* parent,
                                                     const QString& objectName,
                                                     const QString& label, const double maximum,
                                                     const int decimals, const QString& unit,
                                                     const double customDefault) {
    // A value whose zero means Automatic: a mode (Automatic / Custom) and, for Custom, the value
    // with its unit. The field stays the one source of the value.
    auto* mode = new kit::KDropdown(parent);
    mode->setObjectName(objectName + QStringLiteral("Mode"));
    mode->setAccessibleName(label);
    mode->addItem(tr("Automatic"), QStringLiteral("automatic"));
    mode->addItem(tr("Custom"), QStringLiteral("custom"));
    properties::ValueCellSpec spec;
    spec.objectName = objectName;
    spec.accessibleName = label;
    spec.minimum = 0.0;
    spec.maximum = maximum;
    spec.decimals = decimals;
    spec.singleStep = 1.0;
    spec.unit = unit;
    auto* field = properties::makeValueCell(spec, parent);
    field->setCompact(false);
    field->setSpecialValueText(tr("Automatic"));
    setValueWidth(*field);
    (void)addPreferencesRow(layout, parent, label, {mode, field});
    auto custom = std::make_shared<double>(customDefault);
    const auto sync = [mode, field, custom] {
        const bool isCustom = field->value() > 0.0;
        if (isCustom)
            *custom = field->value();
        const QSignalBlocker blocker(mode);
        mode->setCurrentIndex(isCustom ? 1 : 0);
        field->setVisible(isCustom);
    };
    connect(field, &kit::KValueField::valueChanged, this, sync);
    connect(mode, &kit::KDropdown::activated, this, [mode, field, custom](int) {
        field->setValue(mode->currentIndex() == 1 ? *custom : 0.0);
    });
    syncCallbacks_.push_back(sync);
    return field;
}

PreferencesWindow::PreferencesWindow(QWidget& owner, const ApplicationPreferences& current,
                                     const AccelerationStatusProvider* const accelerationStatus)
    : kit::KDialog(tr("Preferences"), QStringLiteral("preferencesWindow"), owner, Qt::WindowModal,
                   Width::Wide),
      draft_(current), applied_(current), accelerationStatus_(accelerationStatus) {

    auto* body = new QHBoxLayout;
    body->setSpacing(kit::px(kit::Spacing::L));

    // The sidebar, Blender style: one category per row at the list pitch, grouped by small gaps,
    // the kit's selected-row recipe (wash and Accent edge) on the current one and a hover wash;
    // Up/Down, Home/End and type-ahead move through it. The page actions sit at its foot.
    auto* sidebar = new QWidget(this);
    sidebar->setFixedWidth(kit::px(kit::Size::PreferencesRailWidth));
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(kit::px(kit::Spacing::S));
    categoryRail_ = new QListWidget(sidebar);
    categoryRail_->setObjectName(QStringLiteral("preferencesCategoryRail"));
    categoryRail_->setAccessibleName(tr("Preferences pages"));
    categoryRail_->setFrameShape(QFrame::NoFrame);
    categoryRail_->setMouseTracking(true);
    categoryRail_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    categoryRail_->viewport()->setAutoFillBackground(false);
    categoryRail_->setItemDelegate(new RailDelegate(categoryRail_));
    sidebarLayout->addWidget(categoryRail_, 1);
    auto* actions = new kit::KMenuButton(sidebar);
    actions->setObjectName(QStringLiteral("preferencesActionsButton"));
    actions->setIcon(kit::icon(kit::IconId::Menu, kit::IconRole::Chrome));
    actions->setToolTip(tr("Restore Defaults"));
    actions->setAccessibleName(tr("Restore Defaults"));
    actions->setPopupMode(QToolButton::InstantPopup);
    auto* actionsMenu = kit::makeMenu(actions);
    auto* restore = actionsMenu->addAction(tr("Restore Defaults"));
    restore->setObjectName(QStringLiteral("preferencesRestoreDefaultsAction"));
    connect(restore, &QAction::triggered, this, &PreferencesWindow::confirmRestoreDefaults);
    actions->setMenu(actionsMenu);
    sidebarLayout->addWidget(actions, 0, Qt::AlignLeft);
    body->addWidget(sidebar, 0);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("preferencesPages"));
    body->addWidget(pages_, 1);
    contentLayout()->addLayout(body, 1);

    const auto pages = buildPages();
    for (const auto& page : pages)
        pages_->addWidget(page.widget);
    // Grouped as Blender groups its categories: what everything uses, the editors, the machine.
    // A page whose id no group names (a later page such as Sounds, until it is listed) still
    // appears, in a group of its own after these.
    std::vector<std::vector<QString>> groups{
        {QStringLiteral("general"), QStringLiteral("sounds")},
        {QStringLiteral("viewer"), QStringLiteral("timeline"), QStringLiteral("nodes")},
        {QStringLiteral("memory"), QStringLiteral("performance")}};
    std::vector<QString> unlisted;
    for (const auto& page : pages)
        if (std::ranges::none_of(groups, [&page](const auto& group) {
                return std::ranges::find(group, page.id) != group.end();
            }))
            unlisted.push_back(page.id);
    if (!unlisted.empty())
        groups.push_back(std::move(unlisted));
    bool first = true;
    for (const auto& group : groups) {
        std::vector<std::size_t> members;
        for (const auto& id : group)
            for (std::size_t index = 0; index < pages.size(); ++index)
                if (pages[index].id == id)
                    members.push_back(index);
        if (members.empty())
            continue;
        if (!first) {
            auto* gap = new QListWidgetItem(categoryRail_);
            gap->setFlags(Qt::NoItemFlags);
            gap->setSizeHint(QSize(0, kit::px(kit::Spacing::S)));
        }
        first = false;
        for (const auto index : members) {
            auto* item = new QListWidgetItem(pages[index].title, categoryRail_);
            item->setData(Qt::UserRole, static_cast<int>(index));
            item->setSizeHint(QSize(0, kit::px(kit::Size::ListRow)));
        }
    }
    connect(categoryRail_, &QListWidget::currentItemChanged, pages_, [this](QListWidgetItem* item) {
        if (item == nullptr || !item->data(Qt::UserRole).isValid())
            return;
        pages_->setCurrentIndex(item->data(Qt::UserRole).toInt());
        gLastPreferencesPage = pages_->currentIndex();
    });
    // The category last shown, for the rest of the session.
    for (int row = 0; row < categoryRail_->count(); ++row)
        if (auto* item = categoryRail_->item(row);
            item->data(Qt::UserRole).isValid() &&
            item->data(Qt::UserRole).toInt() == gLastPreferencesPage)
            categoryRail_->setCurrentRow(row);

    buttonBox()->setObjectName(QStringLiteral("preferencesButtonBox"));
    addButton(Role::Accept, tr("OK"))->setObjectName(QStringLiteral("preferencesOkButton"));
    addButton(Role::Reject, tr("Cancel"))->setObjectName(QStringLiteral("preferencesCancelButton"));
    applyButton_ = addButton(Role::Apply, tr("Apply"));
    applyButton_->setObjectName(QStringLiteral("preferencesApplyButton"));
    connect(this, &QDialog::accepted, this, [this] { commitPreferences(); });
    connect(this, &kit::KDialog::applyRequested, this, [this] { commitPreferences(); });
    setValidator([this]() -> std::optional<kit::KRefusal> {
        if (!cacheDirectoryIssue_.isEmpty())
            return kit::KRefusal{cacheDirectoryField_, cacheDirectoryIssue_};
        return std::nullopt;
    });

    refreshControls();
    updateApplyEnabled();
}

void PreferencesWindow::fitPagesToContent() {
    // The window fits its content: it opens with the pages a dialog text column wide and as tall
    // as the tallest page, up to most of the screen; only a page taller than that scrolls. It can
    // be made narrower, down to the widest page's minimum. Measured once polished, so the kit
    // fonts and metrics are the ones measured.
    const int pageWidth = kit::px(kit::Size::DialogTextWidth);
    int tallest = 0;
    int narrowest = 0;
    for (int index = 0; index < pages_->count(); ++index)
        if (auto* scroll = qobject_cast<QScrollArea*>(pages_->widget(index));
            scroll != nullptr && scroll->widget() != nullptr) {
            auto* content = scroll->widget();
            content->ensurePolished();
            for (auto* child : content->findChildren<QWidget*>())
                child->ensurePolished();
            if (auto* layout = content->layout())
                layout->activate();
            tallest =
                std::max(tallest, content->hasHeightForWidth() ? content->heightForWidth(pageWidth)
                                                               : content->sizeHint().height());
            narrowest = std::max(narrowest, content->minimumSizeHint().width());
        }
    const auto* screen = window()->screen();
    const int ceiling = screen != nullptr ? screen->availableGeometry().height() * 3 / 4 : tallest;
    pages_->setMinimumSize(std::min(narrowest, pageWidth), std::min(tallest, ceiling));
    if (!fitted_) {
        fitted_ = true;
        layout()->activate();
        resize(std::max(width(),
                        layout()->totalMinimumSize().width() - pages_->minimumWidth() + pageWidth),
               height());
    }
}

void PreferencesWindow::confirmRestoreDefaults() {
    kit::KConfirmSpec spec;
    spec.title = tr("Restore Defaults");
    spec.objectName = QStringLiteral("preferencesRestoreDefaultsConfirmation");
    spec.tone = kit::KPromptTone::Warning;
    spec.heading = tr("Restore every preference to its default?");
    spec.detail = tr("Nothing is saved until you choose OK or Apply.");
    spec.destructiveText = tr("Restore Defaults");
    spec.rejectText = tr("Cancel");
    spec.defaultRole = Role::Reject;
    QPointer<PreferencesWindow> self(this);
    kit::confirm(*this, spec, [self](const Role role) {
        if (self && role == Role::Destructive)
            self->setPreferences(defaultApplicationPreferences());
    });
}

void PreferencesWindow::checkCacheDirectory(const QString& directory) {
    cacheDirectoryIssue_.clear();
    const auto path = directory.trimmed();
    if (!path.isEmpty()) {
        const QFileInfo info(path);
        if (info.exists() && !info.isDir()) {
            cacheDirectoryIssue_ = tr("This is a file, not a folder.");
        } else if (info.exists() && !info.isWritable()) {
            cacheDirectoryIssue_ = tr("Bloom cannot write to this folder.");
        } else if (!info.exists()) {
            // The nearest folder that exists must let Bloom create the rest.
            QFileInfo parent(info.absolutePath());
            while (!parent.exists() && parent.absoluteFilePath() != parent.absolutePath())
                parent = QFileInfo(parent.absolutePath());
            if (!parent.isDir() || !parent.isWritable())
                cacheDirectoryIssue_ = tr("Bloom cannot create this folder here.");
        }
    }
    revalidate();
}

ApplicationPreferences PreferencesWindow::preferences() const { return draft_; }

void PreferencesWindow::setPreferences(const ApplicationPreferences& preferences) {
    draft_ = preferences;
    refreshControls();
    checkCacheDirectory(QString::fromStdString(draft_.mediaDiskCacheDirectory));
    updateApplyEnabled();
}

void PreferencesWindow::resetSection(
    kit::KSection& section,
    std::function<void(ApplicationPreferences&, const ApplicationPreferences&)> assign) {
    // A section's Reset, as in Properties: that section's values return to their defaults in the
    // window; nothing is saved until OK or Apply.
    connect(&section, &kit::KSection::resetRequested, this, [this, assign = std::move(assign)] {
        auto next = draft_;
        assign(next, defaultApplicationPreferences());
        setPreferences(next);
    });
}

void PreferencesWindow::refreshControls() {
    for (const auto& refresh : refreshCallbacks_) {
        refresh();
    }
    for (const auto& sync : syncCallbacks_) {
        sync();
    }
}

void PreferencesWindow::commitPreferences() {
    applied_ = draft_;
    emit preferencesApplied(applied_);
    updateApplyEnabled();
}

void PreferencesWindow::updateApplyEnabled() {
    if (applyButton_ != nullptr) {
        applyButton_->setEnabled(draft_ != applied_);
    }
}

// --- Row helpers -------------------------------------------------------------------------------

kit::KSwitch* PreferencesWindow::addSwitchRow(QVBoxLayout* layout, QWidget* parent,
                                              const QString& objectName, const QString& label,
                                              const bool restartRequired) {
    // A switch sits in the value column with its own clickable words, as a check box does.
    auto* control = new kit::KSwitch(parent);
    control->setObjectName(objectName);
    auto* words = new kit::KCheckLabel(label, *control, parent);
    words->setObjectName(objectName + QStringLiteral("Label"));
    // The row's value text size, a little apart from the switch.
    words->setTypeRole(kit::TypeRole::UiSmall);
    words->setContentsMargins(kit::px(kit::Spacing::XS), 0, 0, 0);
    (void)restartRequired;
    (void)addPreferencesRow(layout, parent, QString{}, {control, words});
    return control;
}

kit::KDropdown* PreferencesWindow::addDropdownRow(QVBoxLayout* layout, QWidget* parent,
                                                  const QString& objectName, const QString& label) {
    auto* control = new kit::KDropdown(parent);
    control->setObjectName(objectName);
    control->setAccessibleName(label);
    addPreferencesRow(layout, parent, label, {control});
    return control;
}

kit::KValueField* PreferencesWindow::addValueFieldRow(QVBoxLayout* layout, QWidget* parent,
                                                      const QString& objectName,
                                                      const QString& label, const double minimum,
                                                      const double maximum, const int decimals,
                                                      const QString& unit,
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
    field->setCompact(false);
    setValueWidth(*field);
    (void)restartRequired;
    addPreferencesRow(layout, parent, label, {field});
    return field;
}

// --- Pages -------------------------------------------------------------------------------------

std::vector<PreferencesWindow::Page> PreferencesWindow::buildPages() {
    std::vector<Page> pages;
    pages.push_back({QStringLiteral("general"), tr("General"), buildGeneralPage()});
    pages.push_back({QStringLiteral("memory"), tr("Memory & Caches"), buildMemoryPage()});
    pages.push_back({QStringLiteral("timeline"), tr("Timeline"), buildTimelinePage()});
    pages.push_back({QStringLiteral("nodes"), tr("Node Graph"), buildNodeGraphPage()});
    pages.push_back({QStringLiteral("viewer"), tr("Viewer"), buildViewerPage()});
    pages.push_back({QStringLiteral("sounds"), tr("Sounds"), buildSoundsPage()});
    pages.push_back({QStringLiteral("performance"), tr("Performance"), buildPerformancePage()});
    return pages;
}

QWidget* PreferencesWindow::buildGeneralPage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    setPageMargins(*layout);

    auto* section = properties::makeSection(
        layout, content, QStringLiteral("preferencesGeneralSection"), tr("Playback"),
        QStringLiteral("preferences/sections/general/collapsed"));
    section->setBodyPadding(kit::px(kit::Spacing::XS));
    resetSection(*section, [](ApplicationPreferences& draft, const ApplicationPreferences& base) {
        draft.audioEnabled = base.audioEnabled;
        draft.loopPlayback = base.loopPlayback;
    });
    auto* rows = section->bodyLayout();

    auto* audio = addSwitchRow(rows, content, QStringLiteral("preferencesAudioEnabledSwitch"),
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

    auto* loop = addSwitchRow(rows, content, QStringLiteral("preferencesLoopPlaybackSwitch"),
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

    // The MCP server's port and auto-start live in the MCP Server panel only; a link, not a copy.
    auto* mcpLink = new kit::KButton(tr("MCP server settings are in the MCP Server panel"), content);
    mcpLink->setObjectName(QStringLiteral("preferencesMcpLink"));
    mcpLink->setVariant(kit::KButton::Variant::Secondary);
    connect(mcpLink, &QAbstractButton::clicked, this, &PreferencesWindow::mcpSettingsRequested);
    layout->addWidget(mcpLink, 0, Qt::AlignLeft);

    layout->addStretch(1);
    return wrapPage(content, QStringLiteral("preferencesGeneralPage"));
}

QWidget* PreferencesWindow::buildMemoryPage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    setPageMargins(*layout);

    auto* memory = properties::makeSection(layout, content,
                                           QStringLiteral("preferencesMemorySection"), tr("Memory"),
                                           QStringLiteral("preferences/sections/memory/collapsed"));
    memory->setBodyPadding(kit::px(kit::Spacing::XS));
    resetSection(*memory, [](ApplicationPreferences& draft, const ApplicationPreferences& base) {
        draft.ramHeadroomBytes = base.ramHeadroomBytes;
        draft.vramHeadroomBytes = base.vramHeadroomBytes;
    });
    auto* memoryRows = memory->bodyLayout();

    const auto addHeadroomRow = [&](const QString& objectName, const QString& label,
                                    std::uint64_t ApplicationPreferences::*member) {
        auto* field = addValueFieldRow(memoryRows, content, objectName, label, 0.0,
                                       static_cast<double>(runtime::kMaximumMemoryHeadroomBytes) / kBytesPerGiB,
                                       2, QStringLiteral("GiB"), false);
        field->setValue(static_cast<double>(draft_.*member) / kBytesPerGiB);
        connect(field, &kit::KValueField::valueChanged, this, [this, member](const double value) {
            draft_.*member =
                std::min(budgetBytesFromGiB(value), runtime::kMaximumMemoryHeadroomBytes);
            updateApplyEnabled();
        });
        refreshCallbacks_.push_back([this, field, member] {
            const QSignalBlocker blocker(field);
            field->setValue(static_cast<double>(draft_.*member) / kBytesPerGiB);
        });
    };
    addHeadroomRow(QStringLiteral("preferencesRamHeadroomField"), tr("RAM headroom"),
                   &ApplicationPreferences::ramHeadroomBytes);
    addHeadroomRow(QStringLiteral("preferencesVramHeadroomField"), tr("VRAM headroom"),
                   &ApplicationPreferences::vramHeadroomBytes);

    auto* hint = new kit::KLabel(
        tr("Headroom is memory Bloom leaves free for the rest of your computer. Bloom uses "
           "everything else for rendering and caching."),
        content);
    hint->setObjectName(QStringLiteral("preferencesMemoryHint"));
    addHelperRow(memoryRows, content, hint);

    auto* disk = properties::makeSection(
        layout, content, QStringLiteral("preferencesMediaCacheSection"), tr("Media disk cache"),
        QStringLiteral("preferences/sections/media-cache/collapsed"));
    disk->setBodyPadding(kit::px(kit::Spacing::XS));
    resetSection(*disk, [](ApplicationPreferences& draft, const ApplicationPreferences& base) {
        draft.mediaDiskCacheEnabled = base.mediaDiskCacheEnabled;
        draft.mediaDiskCacheDirectory = base.mediaDiskCacheDirectory;
        draft.mediaDiskCacheBudgetBytes = base.mediaDiskCacheBudgetBytes;
    });
    auto* diskRows = disk->bodyLayout();
    // Every disk cache value is read at startup: the section says so once, in its header.
    disk->addHeaderAction(makeRestartBadge(content));

    auto* enabled =
        addSwitchRow(diskRows, content, QStringLiteral("preferencesMediaDiskCacheEnabledSwitch"),
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

    auto* directory = new kit::KPathField(content);
    directory->setObjectName(QStringLiteral("preferencesMediaDiskCacheDirectoryEdit"));
    directory->setAccessibleName(tr("Cache directory"));
    directory->setPlaceholderText(tr("Default location"));
    directory->setText(QString::fromStdString(draft_.mediaDiskCacheDirectory));
    auto* browse = new kit::KButton(tr("Browse…"), content);
    browse->setObjectName(QStringLiteral("preferencesMediaDiskCacheBrowseButton"));
    browse->setFixedWidth(browse->sizeHint().width()); // the path field takes the rest
    connect(browse, &QAbstractButton::clicked, this, [this, directory] {
        const auto picked = kit::chooseFile(*this, {.mode = kit::KFileRequest::Mode::Directory,
                                                    .title = tr("Choose a Cache Folder"),
                                                    .kind = QStringLiteral("cache-directory"),
                                                    .nameFilters = {},
                                                    .defaultSuffix = {},
                                                    .startPath = directory->text()});
        if (!picked) {
            return;
        }
        directory->setText(*picked);
        draft_.mediaDiskCacheDirectory = picked->toStdString();
        checkCacheDirectory(*picked);
        updateApplyEnabled();
    });
    connect(directory, &QLineEdit::editingFinished, this,
            [this, directory] { checkCacheDirectory(directory->text()); });
    connect(directory, &QLineEdit::textChanged, this, [this](const QString& text) {
        draft_.mediaDiskCacheDirectory = text.toStdString();
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, directory] {
        const QSignalBlocker blocker(directory);
        directory->setText(QString::fromStdString(draft_.mediaDiskCacheDirectory));
    });
    // Clearing the field returns to the default location.
    auto* clear = new kit::KIconButton(content);
    clear->setObjectName(QStringLiteral("preferencesMediaDiskCacheDirectoryClear"));
    clear->setIcon(kit::icon(kit::IconId::Reset, kit::IconRole::Chrome));
    clear->setToolTip(tr("Default location"));
    clear->setAccessibleName(tr("Default location"));
    connect(clear, &QAbstractButton::clicked, this, [this, directory] {
        directory->clear();
        checkCacheDirectory(QString{});
    });
    auto* directoryRow =
        addPreferencesRow(diskRows, content, tr("Cache directory"), {directory, browse, clear});
    cacheDirectoryField_ = directory;
    // The directory's inline reason, under its row in the value column.
    auto* directoryReason = new kit::KLabel(QString{}, content, kit::TypeRole::UiSmall);
    directoryReason->setObjectName(QStringLiteral("preferencesMediaDiskCacheDirectoryReason"));
    addHelperRow(diskRows, content, directoryReason);
    directoryReason->setTone(kit::Color::Error);
    directoryReason->hide();
    registerReason(directory, directoryReason);
    (void)directoryRow;

    auto* budget =
        addAutomaticRow(diskRows, content, QStringLiteral("preferencesMediaDiskCacheBudgetField"),
                        tr("Disk cache budget"), kMaximumBudgetGiB, 2, QStringLiteral("GiB"), 1.0);
    budget->setValue(static_cast<double>(draft_.mediaDiskCacheBudgetBytes) / kBytesPerGiB);
    connect(budget, &kit::KValueField::valueChanged, this, [this](const double value) {
        draft_.mediaDiskCacheBudgetBytes = budgetBytesFromGiB(value);
        updateApplyEnabled();
    });
    refreshCallbacks_.push_back([this, budget] {
        const QSignalBlocker blocker(budget);
        budget->setValue(static_cast<double>(draft_.mediaDiskCacheBudgetBytes) / kBytesPerGiB);
    });
    auto* budgetHint = new kit::KLabel(tr("Automatic chooses a budget for this machine."), content);
    budgetHint->setObjectName(QStringLiteral("preferencesMediaDiskCacheBudgetHint"));
    addHelperRow(diskRows, content, budgetHint);

    layout->addStretch(1);
    return wrapPage(content, QStringLiteral("preferencesMemoryPage"));
}

QWidget* PreferencesWindow::buildTimelinePage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    setPageMargins(*layout);

    auto* section = properties::makeSection(
        layout, content, QStringLiteral("preferencesTimelineSection"), tr("Timeline"),
        QStringLiteral("preferences/sections/timeline/collapsed"));
    section->setBodyPadding(kit::px(kit::Spacing::XS));
    resetSection(*section, [](ApplicationPreferences& draft, const ApplicationPreferences& base) {
        draft.timelineTimeFormat = base.timelineTimeFormat;
        draft.timelineSnapping = base.timelineSnapping;
        draft.timelineKeyframesVisible = base.timelineKeyframesVisible;
        draft.timelineGraphEditor = base.timelineGraphEditor;
        draft.timelineLayerColumnWidth = base.timelineLayerColumnWidth;
    });
    auto* rows = section->bodyLayout();

    auto* format = addDropdownRow(
        rows, content, QStringLiteral("preferencesTimelineTimeFormatDropdown"), tr("Time format"));
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
        {"preferencesTimelineSnappingSwitch", QT_TR_NOOP("Snap to edges"),
         &ApplicationPreferences::timelineSnapping},
        {"preferencesTimelineKeyframesSwitch", QT_TR_NOOP("Show keyframes"),
         &ApplicationPreferences::timelineKeyframesVisible},
        {"preferencesTimelineGraphEditorSwitch", QT_TR_NOOP("Graph editor"),
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

    // The Timeline treats Show keyframes and Graph editor as mutually exclusive (its own menus
    // enforce it). Mirror that here so the window cannot present a pair the editor would silently
    // resolve differently.
    auto* keyframesSwitch =
        content->findChild<kit::KSwitch*>(QStringLiteral("preferencesTimelineKeyframesSwitch"));
    auto* graphSwitch =
        content->findChild<kit::KSwitch*>(QStringLiteral("preferencesTimelineGraphEditorSwitch"));
    if (keyframesSwitch != nullptr && graphSwitch != nullptr) {
        connect(keyframesSwitch, &QAbstractButton::toggled, this,
                [this, graphSwitch](const bool on) {
                    if (on && draft_.timelineGraphEditor) {
                        graphSwitch->setChecked(false);
                    }
                });
        connect(graphSwitch, &QAbstractButton::toggled, this,
                [this, keyframesSwitch](const bool on) {
                    if (on && draft_.timelineKeyframesVisible) {
                        keyframesSwitch->setChecked(false);
                    }
                });
    }

    auto* width =
        addAutomaticRow(rows, content, QStringLiteral("preferencesTimelineLayerColumnWidthField"),
                        tr("Layer column width"), std::numeric_limits<int>::max(), 0,
                        QStringLiteral("px"), kit::px(kit::Size::TimelineNameDefault));
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
    return wrapPage(content, QStringLiteral("preferencesTimelinePage"));
}

QWidget* PreferencesWindow::buildNodeGraphPage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    setPageMargins(*layout);

    auto* section = properties::makeSection(
        layout, content, QStringLiteral("preferencesNodeGraphSection"), tr("Node Graph"),
        QStringLiteral("preferences/sections/node-graph/collapsed"));
    section->setBodyPadding(kit::px(kit::Spacing::XS));
    resetSection(*section, [](ApplicationPreferences& draft, const ApplicationPreferences& base) {
        draft.nodeLinkStyle = base.nodeLinkStyle;
        draft.nodeSnap = base.nodeSnap;
        draft.nodeGridSize = base.nodeGridSize;
    });
    auto* rows = section->bodyLayout();

    auto* linkStyle = addDropdownRow(
        rows, content, QStringLiteral("preferencesNodeLinkStyleDropdown"), tr("Link style"));
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

    auto* snap = addSwitchRow(rows, content, QStringLiteral("preferencesNodeSnapSwitch"),
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

    auto* grid = addValueFieldRow(rows, content, QStringLiteral("preferencesNodeGridSizeField"),
                                  tr("Grid size"), 1.0, std::numeric_limits<double>::max(), 2,
                                  QStringLiteral("px"), false);
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
    return wrapPage(content, QStringLiteral("preferencesNodeGraphPage"));
}

QWidget* PreferencesWindow::buildViewerPage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    setPageMargins(*layout);

    auto* section = properties::makeSection(
        layout, content, QStringLiteral("preferencesViewerSection"), tr("Viewer"),
        QStringLiteral("preferences/sections/viewer/collapsed"));
    section->setBodyPadding(kit::px(kit::Spacing::XS));
    resetSection(*section, [](ApplicationPreferences& draft, const ApplicationPreferences& base) {
        draft.viewerResolution = base.viewerResolution;
        draft.viewerBackground = base.viewerBackground;
        draft.viewerTransparencyGrid = base.viewerTransparencyGrid;
        draft.viewerSafeAreas = base.viewerSafeAreas;
        draft.viewerCentreCross = base.viewerCentreCross;
        draft.viewerThirds = base.viewerThirds;
        draft.viewerRulers = base.viewerRulers;
        draft.viewerPixelGrid = base.viewerPixelGrid;
    });
    auto* rows = section->bodyLayout();

    auto* resolution = addDropdownRow(
        rows, content, QStringLiteral("preferencesViewerResolutionDropdown"), tr("Resolution"));
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
        rows, content, QStringLiteral("preferencesViewerBackgroundDropdown"), tr("Background"));
    background->setToolTip(tr("The canvas around the composition frame"));
    background->addItem(tr("Solid"), QStringLiteral("Solid"));
    background->addItem(tr("Grey"), QStringLiteral("Grey"));
    background->addItem(tr("Black"), QStringLiteral("Black"));
    background->addItem(tr("White"), QStringLiteral("White"));
    background->setCurrentIndex(
        background->findData(QLatin1String(viewerBackgroundValue(draft_.viewerBackground))));
    connect(background, &kit::KDropdown::currentIndexChanged, this, [this, background](int) {
        const auto data = background->currentData().toString();
        if (data == QLatin1StringView("Grey"))
            draft_.viewerBackground = ViewerBackgroundPreference::Grey;
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
        {"preferencesViewerTransparencyGridSwitch", QT_TR_NOOP("Transparency grid"),
         &ApplicationPreferences::viewerTransparencyGrid},
        {"preferencesViewerSafeAreasSwitch", QT_TR_NOOP("Safe areas"),
         &ApplicationPreferences::viewerSafeAreas},
        {"preferencesViewerCentreCrossSwitch", QT_TR_NOOP("Centre cross"),
         &ApplicationPreferences::viewerCentreCross},
        {"preferencesViewerThirdsSwitch", QT_TR_NOOP("Thirds"),
         &ApplicationPreferences::viewerThirds},
        {"preferencesViewerRulersSwitch", QT_TR_NOOP("Rulers"),
         &ApplicationPreferences::viewerRulers},
        {"preferencesViewerPixelGridSwitch", QT_TR_NOOP("Pixel grid"),
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
    return wrapPage(content, QStringLiteral("preferencesViewerPage"));
}

} // namespace bloom::ui
