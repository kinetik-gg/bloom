#include "export_job_lines.hpp"

#include "export_render_list.hpp"

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/switch_control.hpp>

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QDir>
#include <QMenu>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>

namespace bloom::ui::exportpanel {
namespace {

[[nodiscard]] QHBoxLayout* rowLayout(QWidget* parent) {
    auto* layout = new QHBoxLayout(parent);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    return layout;
}

// A line's label: one control tall, right-aligned in the shared label column.
[[nodiscard]] kit::KLabel* lineLabel(const QString& text, const RenderList& list, QWidget* parent) {
    auto* label = new kit::KLabel(text, parent);
    label->setTone(kit::Color::Muted);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setFixedWidth(list.lineLabelWidth());
    return label;
}

// ▾: the quick picks behind a value, plate-less as After Effects draws it.
[[nodiscard]] kit::KIconButton* picksButton(const QString& name, const QString& accessible,
                                            QWidget* parent) {
    auto* button = new kit::KIconButton(parent);
    button->setObjectName(name);
    button->setProperty("disclosure", true);
    button->setIcon(kit::icon(kit::IconId::CaretDown, kit::IconRole::Chrome));
    button->setAccessibleName(accessible);
    button->setToolTip(accessible);
    return button;
}

[[nodiscard]] kit::KButton* linkButton(const QString& name, QWidget* parent) {
    auto* link = new kit::KButton(parent);
    link->setObjectName(name);
    link->setVariant(kit::KButton::Variant::Link);
    return link;
}

[[nodiscard]] kit::KIconButton* iconButton(const kit::IconId icon, const QString& name,
                                           const QString& accessible, QWidget* parent) {
    auto* button = new kit::KIconButton(parent);
    button->setObjectName(name);
    button->setIcon(kit::icon(icon, kit::IconRole::Chrome));
    button->setAccessibleName(accessible);
    return button;
}

void setReason(QWidget& widget, const bool available, const QString& tip, const QString& reason) {
    widget.setEnabled(available);
    widget.setToolTip(available ? tip : reason);
}

[[nodiscard]] QMenu* openBelow(QMenu* menu, QWidget& anchor) {
    menu->popup(anchor.mapToGlobal(QPoint(0, anchor.height())));
    return menu;
}

constexpr auto kRecentFoldersKey = "export/recent-folders";
constexpr int kRecentFolderCount = 5;

} // namespace

QStringList recentExportFolders() {
    return QSettings().value(QString::fromLatin1(kRecentFoldersKey)).toStringList();
}

void rememberExportFolder(const QString& folder) {
    auto folders = recentExportFolders();
    const auto shown = QDir::toNativeSeparators(folder);
    folders.removeAll(shown);
    folders.prepend(shown);
    while (folders.size() > kRecentFolderCount)
        folders.removeLast();
    QSettings().setValue(QString::fromLatin1(kRecentFoldersKey), folders);
}

CheckCell::CheckCell(QWidget* const parent) : QWidget(parent) {
    auto* layout = rowLayout(this);
    box_ = new kit::KCheckBox(this);
    box_->setObjectName(QStringLiteral("exportRenderCheck"));
    box_->setAccessibleName(tr("Render"));
    layout->addStretch(1);
    layout->addWidget(box_);
    layout->addStretch(1);
}

StatusCell::StatusCell(QWidget* const parent) : QWidget(parent) {
    setObjectName(QStringLiteral("exportStatusCell"));
    auto* layout = rowLayout(this);
    layout->setContentsMargins(kit::px(kit::Spacing::S), 0, kit::px(kit::Spacing::S), 0);
    layout->setSpacing(kit::px(kit::Spacing::S));
    label_ = new kit::KLabel(this);
    label_->setObjectName(QStringLiteral("exportStatusText"));
    label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    label_->setMinimumWidth(0);
    bar_ = new kit::KProgressBar(this);
    bar_->setObjectName(QStringLiteral("exportStatusBar"));
    bar_->setFixedWidth(kit::px(kit::Size::DropdownWidth));
    bar_->hide();
    layout->addWidget(label_, 1);
    layout->addWidget(bar_, 0, Qt::AlignVCenter);
}

void StatusCell::show(const StatusText& status, const host::ExportJobView& job) {
    text_ = status.text;
    label_->setElidedText(status.text);
    label_->setTone(toneColor(status.tone));
    setToolTip(status.detail.isEmpty() ? status.text : status.detail);
    const bool running = host::isActive(job.state) && job.state != host::ExportJobState::Waiting;
    bar_->setVisible(running);
    if (job.progress.framesTotal > 0 && !isWaitingForViewer(job) &&
        job.state != host::ExportJobState::Verifying && job.state != host::ExportJobState::Preparing)
        bar_->setProgress(std::min(job.progress.framesCompleted, job.progress.framesTotal),
                          job.progress.framesTotal);
    else
        bar_->setBusy();
}

bool StatusCell::barShown() const { return !bar_->isHidden(); }

ActionsCell::ActionsCell(QWidget* const parent) : QWidget(parent) {
    auto* layout = rowLayout(this);
    button_ = iconButton(kit::IconId::ContextMenu, QStringLiteral("exportJobMenuButton"),
                         tr("Job actions"), this);
    button_->setToolTip(tr("Job actions"));
    layout->addStretch(1);
    layout->addWidget(button_);
    layout->addStretch(1);
}

RenderSettingsLine::RenderSettingsLine(const RenderList& list, QWidget* const parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("exportRenderSettingsLine"));
    auto* layout = rowLayout(this);
    label_ = lineLabel(tr("Render Settings:"), list, this);
    picks_ = picksButton(QStringLiteral("exportRangePicks"), tr("Range choices"), this);
    link_ = linkButton(QStringLiteral("exportRenderSettingsLink"), this);
    link_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(label_);
    layout->addSpacing(kit::px(kit::Spacing::S));
    layout->addWidget(picks_);
    layout->addWidget(link_);
    layout->addStretch(1);
    connect(link_, &kit::KButton::clicked, this, &RenderSettingsLine::openRequested);
    connect(picks_, &QAbstractButton::clicked, this, [this] { (void)openBelow(buildPicks(), *picks_); });
}

void RenderSettingsLine::show(const QString& value, const bool editable, const QString& reason,
                              const bool currentFrame, const QString& currentFrameReason) {
    link_->setText(value);
    link_->updateGeometry();
    setReason(*link_, editable, tr("Render Settings"), reason);
    setReason(*picks_, editable, tr("Range choices"), reason);
    currentFrame_ = currentFrame;
    currentFrameReason_ = currentFrameReason;
}

QMenu* RenderSettingsLine::buildPicks() {
    auto* menu = kit::makeMenu(this);
    menu->setObjectName(QStringLiteral("exportRangeMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setToolTipsVisible(true);
    const auto add = [&](const QString& text, const document::ExportRangeKind kind, const char* name) {
        auto* action = menu->addAction(text);
        action->setObjectName(QString::fromLatin1(name));
        connect(action, &QAction::triggered, this, [this, kind] { Q_EMIT rangePicked(static_cast<int>(kind)); });
        return action;
    };
    add(tr("Work area"), document::ExportRangeKind::WorkArea, "exportRangeWorkArea");
    add(tr("Whole composition"), document::ExportRangeKind::Composition, "exportRangeComposition");
    auto* frame = add(tr("Current frame"), document::ExportRangeKind::Frame, "exportRangeCurrentFrame");
    frame->setEnabled(currentFrame_);
    frame->setToolTip(currentFrame_ ? QString{} : currentFrameReason_);
    return menu;
}

OutputModuleLine::OutputModuleLine(const RenderList& list, QWidget* const parent)
    : QWidget(parent), list_(list) {
    setObjectName(QStringLiteral("exportOutputModuleLine"));
    rows_ = new QVBoxLayout(this);
    rows_->setContentsMargins(0, 0, 0, 0);
    rows_->setSpacing(0);
    auto* firstRow = new QWidget(this);
    first_ = rowLayout(firstRow);
    auto* secondRow = new QWidget(this);
    second_ = rowLayout(secondRow);
    rows_->addWidget(firstRow);
    rows_->addWidget(secondRow);
    secondRow->hide();

    label_ = lineLabel(tr("Output Module:"), list, firstRow);
    presets_ = picksButton(QStringLiteral("exportPresetPicks"), tr("Formats"), firstRow);
    module_ = linkButton(QStringLiteral("exportOutputModuleLink"), firstRow);
    add_ = iconButton(kit::IconId::Add, QStringLiteral("exportAddOutputButton"), tr("Add output"),
                      firstRow);
    remove_ = iconButton(kit::IconId::Subtract, QStringLiteral("exportRemoveOutputButton"),
                         tr("Remove output"), firstRow);

    toGroup_ = new QWidget(firstRow);
    toGroup_->setObjectName(QStringLiteral("exportOutputTo"));
    auto* group = rowLayout(toGroup_);
    toLabel_ = new kit::KLabel(tr("Output To:"), toGroup_);
    toLabel_->setTone(kit::Color::Muted);
    toLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    destinationMenu_ = picksButton(QStringLiteral("exportDestinationPicks"), tr("Save to choices"), toGroup_);
    destination_ = linkButton(QStringLiteral("exportOutputToLink"), toGroup_);
    destination_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    group->addWidget(toLabel_);
    group->addSpacing(kit::px(kit::Spacing::S));
    group->addWidget(destinationMenu_);
    group->addWidget(destination_);

    first_->addWidget(label_);
    first_->addSpacing(kit::px(kit::Spacing::S));
    first_->addWidget(presets_);
    first_->addWidget(module_);
    first_->addSpacing(kit::px(kit::Spacing::S));
    first_->addWidget(add_);
    first_->addSpacing(kit::px(kit::Spacing::XS));
    first_->addWidget(remove_);
    first_->addSpacing(kit::px(kit::Spacing::L));
    first_->addWidget(toGroup_);
    first_->addStretch(1);

    connect(module_, &kit::KButton::clicked, this, &OutputModuleLine::openRequested);
    connect(destination_, &kit::KButton::clicked, this, &OutputModuleLine::chooseRequested);
    connect(add_, &QAbstractButton::clicked, this, &OutputModuleLine::addRequested);
    connect(remove_, &QAbstractButton::clicked, this, &OutputModuleLine::removeRequested);
    connect(presets_, &QAbstractButton::clicked, this, [this] { (void)openBelow(buildPresets(), *presets_); });
    connect(destinationMenu_, &QAbstractButton::clicked, this,
            [this] { (void)openBelow(buildDestinations(), *destinationMenu_); });
}

void OutputModuleLine::show(const OutputLineState& state) {
    state_ = state;
    module_->setText(state.module);
    destination_->setText(state.destination);
    destination_->updateGeometry();
    setReason(*module_, state.editable, state.moduleTip, state.editReason);
    setReason(*presets_, state.editable, tr("Formats"), state.editReason);
    setReason(*destination_, state.editable, state.destinationTip, state.editReason);
    setReason(*destinationMenu_, state.editable, tr("Save to choices"), state.editReason);
    setReason(*add_, state.canAdd, tr("Add output"), state.addReason);
    setReason(*remove_, state.canRemove, tr("Remove output"), state.removeReason);
    applyModuleWidth();
}

void OutputModuleLine::applyModuleWidth() {
    module_->setFixedWidth(std::max(list_.moduleWidth(), module_->minimumSizeHint().width()));
}

int OutputModuleLine::oneRowWidth() const {
    const int lead = list_.lineLabelWidth() + kit::px(kit::Spacing::S) + kit::px(kit::Size::Control) +
                     std::max(list_.moduleWidth(), module_->minimumSizeHint().width()) +
                     kit::px(kit::Spacing::S) + kit::px(kit::Size::Control) + kit::px(kit::Spacing::XS) +
                     kit::px(kit::Size::Control) + kit::px(kit::Spacing::L);
    // Output To keeps room for at least a short path before it folds away.
    return lead + toLabel_->sizeHint().width() + kit::px(kit::Spacing::S) + kit::px(kit::Size::Control) +
           std::min(destination_->sizeHint().width(), kit::px(kit::Size::DropdownWidthExpanded));
}

void OutputModuleLine::setFolded(const bool folded) {
    if (folded == folded_)
        return;
    folded_ = folded;
    auto* secondRow = second_->parentWidget();
    if (folded) {
        first_->removeWidget(toGroup_);
        toGroup_->setParent(secondRow);
        second_->addWidget(toGroup_);
        second_->addStretch(1);
        // On its own row Output To is a line label, right-aligned like Output Module.
        toLabel_->setFixedWidth(list_.lineLabelWidth());
        secondRow->show();
    } else {
        second_->removeWidget(toGroup_);
        while (auto* item = second_->takeAt(0))
            delete item;
        toGroup_->setParent(first_->parentWidget());
        first_->insertWidget(first_->count() - 1, toGroup_);
        toLabel_->setMinimumWidth(0);
        toLabel_->setMaximumWidth(QWIDGETSIZE_MAX);
        secondRow->hide();
    }
    toGroup_->show();
}

QMenu* OutputModuleLine::buildPresets() {
    auto* menu = kit::makeMenu(this);
    menu->setObjectName(QStringLiteral("exportPresetMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    for (const auto& [id, name] : state_.presets) {
        if (id.isEmpty()) {
            menu->addSection(name);
            continue;
        }
        auto* action = menu->addAction(name);
        action->setData(id);
        connect(action, &QAction::triggered, this, [this, id] { Q_EMIT presetPicked(id); });
    }
    return menu;
}

QMenu* OutputModuleLine::buildDestinations() {
    auto* menu = kit::makeMenu(this);
    menu->setObjectName(QStringLiteral("exportDestinationMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto* fallback = menu->addAction(tr("Default name"));
    fallback->setObjectName(QStringLiteral("exportDefaultNameAction"));
    connect(fallback, &QAction::triggered, this, &OutputModuleLine::defaultNameRequested);
    if (!state_.recentFolders.isEmpty())
        menu->addSection(tr("Recent folders"));
    for (const auto& folder : state_.recentFolders) {
        auto* action = menu->addAction(folder);
        action->setData(folder);
        connect(action, &QAction::triggered, this, [this, folder] { Q_EMIT folderPicked(folder); });
    }
    return menu;
}

SummaryLine::SummaryLine(const RenderList& list, QWidget* const parent) : QWidget(parent) {
    setObjectName(QStringLiteral("exportSummaryLine"));
    auto* layout = rowLayout(this);
    // The words start under the line's values.
    layout->setContentsMargins(std::max(0, list.valueLeft() - list.depthLeft(2)), 0,
                               kit::px(kit::Spacing::S), 0);
    label_ = new kit::KLabel(this);
    label_->setObjectName(QStringLiteral("exportSummaryText"));
    label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    label_->setMinimumWidth(0);
    layout->addWidget(label_, 1);
}

void SummaryLine::show(const QString& text, const kit::Color tone) {
    text_ = text;
    label_->setElidedText(text);
    label_->setTone(tone);
    setToolTip(text);
}

} // namespace bloom::ui::exportpanel
