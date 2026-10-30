#include <bloom/ui/export_editor.hpp>

#include "asset_drop.hpp"
#include "export_colour_choices.hpp"
#include "export_detail_support.hpp"
#include "export_job_lines.hpp"
#include "export_panel_text.hpp"
#include "export_render_list.hpp"

#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <bloom/ui/kit/switch_control.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMimeData>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <variant>

namespace bloom::ui {

using exportpanel::CheckCell;
using exportpanel::ExportColourChoices;
using exportpanel::OutputLineState;
using exportpanel::OutputModuleLine;
using exportpanel::qs;
using exportpanel::RenderList;
using exportpanel::RenderSettingsLine;
using exportpanel::RowKind;
using exportpanel::StatusCell;
using exportpanel::SummaryLine;

namespace {

constexpr auto kStripKey = "export/current-render/expanded";

[[nodiscard]] document::ExportJobId jobId(const qulonglong raw) {
    return document::ExportJobId::fromRaw(raw);
}

[[nodiscard]] QString lineKey(const RowKind kind, const std::uint64_t id) {
    return (kind == RowKind::RenderSettings ? QStringLiteral("r%1") : QStringLiteral("o%1")).arg(id);
}

// A job Start All would start now, or would start once the artist says yes to a replace. A
// platform that cannot render yet still shows its jobs as ready to run when it can.
[[nodiscard]] bool isReady(const host::ExportJobView& job) {
    if (!job.record.enabled || job.state != host::ExportJobState::Ready)
        return false;
    const auto& start = job.commands[host::ExportCommand::Start];
    return start.available || start.code == host::ExportUnavailableCode::PlatformNotSupported ||
           start.code == host::ExportUnavailableCode::NeedsReplaceConfirmation;
}

[[nodiscard]] const host::ExportJobView* findView(const host::ExportQueueSnapshot* snapshot,
                                                  const document::ExportJobId job) {
    if (snapshot == nullptr)
        return nullptr;
    for (const auto& view : snapshot->jobs)
        if (view.id == job)
            return &view;
    return nullptr;
}

// Where an output saves, as the artist reads it: the folder as it was chosen and the file name as
// it resolves now ("Exports/SC020.mov"), never a name token.
[[nodiscard]] QString destinationShown(const host::ExportOutputView& output) {
    const auto chosen = exportpanel::detail::displayPath(output.record.destination);
    if (chosen.isEmpty())
        return QCoreApplication::translate("ExportEditor", "Not yet specified");
    if (output.destination.firstPath.empty())
        return chosen;
    const auto file = QString::fromStdString(output.destination.firstPath.filename().string());
    const auto folder = QFileInfo(chosen).path();
    return folder == QStringLiteral(".") ? file : QDir::toNativeSeparators(folder + QLatin1Char('/') + file);
}

// One Muted line of what an output comes to: the files, their size, what blocks it.
[[nodiscard]] QString outputSummary(const host::ExportOutputView& output) {
    QStringList parts;
    if (output.destination.fileCount > 1 && !output.destination.lastPath.empty())
        parts.push_back(QCoreApplication::translate("ExportEditor", "%1 to %2 (%3 files)")
                            .arg(QString::fromStdString(output.destination.firstPath.generic_string()),
                                 QString::fromStdString(output.destination.lastPath.filename().string()))
                            .arg(output.destination.fileCount));
    else if (!output.destination.firstPath.empty())
        parts.push_back(QString::fromStdString(output.destination.firstPath.generic_string()));
    if (output.estimatedBytes.has_value())
        parts.push_back(QCoreApplication::translate("ExportEditor", "about %1")
                            .arg(exportpanel::bytesText(*output.estimatedBytes)));
    if (output.freeBytesAtDestination.has_value())
        parts.push_back(QCoreApplication::translate("ExportEditor", "%1 free")
                            .arg(exportpanel::bytesText(*output.freeBytesAtDestination)));
    // Every format is offered alike; a format the engine cannot run fails at Start with its own
    // plain summary, so availability is not repeated here.
    for (const auto& issue : exportpanel::orderedIssues(output.preflight))
        if (issue.code != host::ExportPreflightCode::DestinationCheckPending &&
            issue.code != host::ExportPreflightCode::PresetUnavailable &&
            !parts.contains(qs(issue.reason)))
            parts.push_back(qs(issue.reason));
    return parts.join(QStringLiteral(" · "));
}

} // namespace

ExportEditor::ExportEditor(CompositionSession& session, ExportQueueAdapter* const adapter,
                           QWidget* const parent)
    : QWidget(parent), session_(session), adapter_(adapter),
      colours_(std::make_unique<ExportColourChoices>()) {
    setObjectName(QStringLiteral("exportEditor"));
    setAccessibleName(tr("Export editor"));
    setAccessibleDescription(tr("The render queue: jobs, their settings and outputs, and their progress"));
    setAcceptDrops(true);

    auto* layout = new QVBoxLayout(this);
    const int gutter = kit::px(kit::Spacing::S);
    layout->setContentsMargins(gutter, gutter, gutter, gutter);
    layout->setSpacing(kit::px(kit::Spacing::Gutter));

    chrome_.header.objectName = QStringLiteral("exportHeader");
    chrome_.header.owner = this;
    const auto headerButton = [this](const QString& text, const QString& name,
                                     const kit::KButton::Variant variant) {
        auto* button = new kit::KButton(text, this);
        button->setObjectName(name);
        button->setVariant(variant);
        return button;
    };
    addComposition_ = headerButton(tr("Add composition"), QStringLiteral("exportAddCompositionButton"),
                                   kit::KButton::Variant::Secondary);
    stopAll_ = headerButton(tr("Stop All"), QStringLiteral("exportStopAllButton"),
                            kit::KButton::Variant::Danger);
    startAll_ = headerButton(tr("Start All"), QStringLiteral("exportStartAllButton"),
                             kit::KButton::Variant::Primary);
    chrome_.header.addWidget(addComposition_);
    chrome_.header.addStretch();
    chrome_.header.addWidget(stopAll_);
    chrome_.header.addWidget(startAll_);
    for (auto* button : {addComposition_, stopAll_, startAll_})
        button->setProperty("chromeSuppressed", true);

    buildStrip();
    layout->addWidget(strip_);

    pages_ = new QStackedWidget(this);
    layout->addWidget(pages_, 1);
    list_ = new RenderList(pages_);
    pages_->addWidget(list_);
    // The line a drop will land on, between two jobs.
    dropLine_ = new kit::KSurface(list_->viewport(), kit::Color::Accent);
    dropLine_->setObjectName(QStringLiteral("exportDropLine"));
    dropLine_->setFixedHeight(kit::px(kit::Spacing::XXS));
    dropLine_->hide();
    buildEmptyPage();

    if (adapter_ == nullptr) {
        emptyText_->setText(tr("Export is not available in this window."));
        emptyHelp_->hide();
        emptyAdd_->hide();
        strip_->hide();
        pages_->setCurrentWidget(emptyPage_);
        return;
    }

    connect(adapter_, &ExportQueueAdapter::rebound, this, [this] { shownRevision_ = ~std::uint64_t{0}; });
    connect(adapter_, &ExportQueueAdapter::queueChanged, this, &ExportEditor::sync);
    connect(adapter_, &ExportQueueAdapter::jobAdded, this, [this](const qulonglong job) {
        // The queue's change was announced first: the row usually exists by now.
        if (jobItem(jobId(job)) != nullptr)
            selectJob(jobId(job));
        else
            pendingSelection_ = jobId(job);
    });
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        // Composition names, rates and the active composition read from the project.
        shownRevision_ = ~std::uint64_t{0};
        sync();
    });
    connect(&session_, &CompositionSession::colorSettingsChanged, this, [this] { coloursStale_ = true; });
    connect(startAll_, &kit::KButton::clicked, this, &ExportEditor::startAll);
    connect(stopAll_, &kit::KButton::clicked, this, [this] { (void)adapter_->stopAll(); });
    connect(addComposition_, &kit::KButton::clicked, this, &ExportEditor::addActiveComposition);
    connect(emptyAdd_, &kit::KButton::clicked, this, &ExportEditor::addActiveComposition);
    connect(list_, &RenderList::menuRequested, this,
            [this](const qulonglong job, const QPoint at) { showJobMenu(jobId(job), at); });
    connect(list_, &RenderList::widthChanged, this, &ExportEditor::refitLines);
    const auto track = [this](QTreeWidgetItem* item, const bool open) {
        if (syncing_)
            return;
        const auto kind = exportpanel::rowKind(item);
        if (kind == RowKind::Job) {
            const auto id = static_cast<qulonglong>(exportpanel::rowJob(item).value());
            if (open)
                openJobs_.insert(id);
            else
                openJobs_.remove(id);
        } else if (kind == RowKind::RenderSettings || kind == RowKind::Output) {
            const auto key = lineKey(kind, kind == RowKind::Output ? exportpanel::rowOutput(item).value()
                                                                   : exportpanel::rowJob(item).value());
            if (open)
                openLines_.insert(key);
            else
                openLines_.remove(key);
        }
    };
    connect(list_, &QTreeWidget::itemExpanded, this, [track](QTreeWidgetItem* item) { track(item, true); });
    connect(list_, &QTreeWidget::itemCollapsed, this, [track](QTreeWidgetItem* item) { track(item, false); });

    if (const auto pending = adapter_->takePendingSelection())
        pendingSelection_ = *pending;
    sync();
}

ExportEditor::~ExportEditor() = default;

void ExportEditor::buildStrip() {
    strip_ = new kit::KSurface(this, kit::Color::Surface);
    strip_->setObjectName(QStringLiteral("exportCurrentRender"));
    auto* rows = new QVBoxLayout(strip_);
    rows->setContentsMargins(0, 0, kit::px(kit::Spacing::S), 0);
    rows->setSpacing(0);
    auto* line = new QWidget(strip_);
    auto* lineLayout = new QHBoxLayout(line);
    lineLayout->setContentsMargins(0, 0, 0, 0);
    lineLayout->setSpacing(kit::px(kit::Spacing::XS));
    stripToggle_ = new kit::KIconButton(line);
    stripToggle_->setObjectName(QStringLiteral("exportCurrentRenderToggle"));
    stripToggle_->setProperty("disclosure", true);
    auto* title = new kit::KLabel(tr("Current Render"), line, kit::TypeRole::UiSmallStrong);
    title->setObjectName(QStringLiteral("exportCurrentRenderTitle"));
    stripText_ = new kit::KLabel(line);
    stripText_->setObjectName(QStringLiteral("exportCurrentRenderText"));
    stripText_->setTone(kit::Color::Muted);
    stripText_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    stripText_->setMinimumWidth(0);
    lineLayout->addWidget(stripToggle_);
    lineLayout->addWidget(title);
    lineLayout->addSpacing(kit::px(kit::Spacing::S));
    lineLayout->addWidget(stripText_, 1);
    rows->addWidget(line);
    auto* barRow = new QWidget(strip_);
    auto* barLayout = new QHBoxLayout(barRow);
    // The bar starts under the title and stops at a readable length, never across a wide panel.
    barLayout->setContentsMargins(kit::px(kit::Size::Control) + kit::px(kit::Spacing::XS), 0, 0,
                                  kit::px(kit::Spacing::S));
    stripBar_ = new kit::KProgressBar(barRow);
    stripBar_->setObjectName(QStringLiteral("exportCurrentRenderBar"));
    stripBar_->setMaximumWidth(kit::px(kit::Size::DialogWideWidth));
    barLayout->addWidget(stripBar_, 1);
    barLayout->addStretch(0);
    rows->addWidget(barRow);
    connect(stripToggle_, &QAbstractButton::clicked, this,
            [this] { setCurrentRenderExpanded(!stripExpanded_); });
    setCurrentRenderExpanded(QSettings().value(QString::fromLatin1(kStripKey), false).toBool());
}

void ExportEditor::buildEmptyPage() {
    emptyPage_ = new kit::KSurface(pages_, kit::Color::Background);
    emptyPage_->setObjectName(QStringLiteral("exportEmptyState"));
    auto* emptyLayout = new QVBoxLayout(emptyPage_);
    emptyLayout->setContentsMargins(kit::px(kit::Spacing::L), kit::px(kit::Spacing::L),
                                    kit::px(kit::Spacing::L), kit::px(kit::Spacing::L));
    emptyLayout->setSpacing(kit::px(kit::Spacing::M));
    emptyLayout->addStretch(1);
    emptyText_ = new kit::KLabel(tr("Nothing to render yet."), emptyPage_, kit::TypeRole::Title);
    emptyText_->setObjectName(QStringLiteral("exportEmptyText"));
    emptyText_->setAlignment(Qt::AlignHCenter);
    emptyText_->setWordWrap(true);
    emptyAdd_ = new kit::KButton(tr("Add composition"), emptyPage_);
    emptyAdd_->setObjectName(QStringLiteral("exportAddActiveButton"));
    emptyAdd_->setVariant(kit::KButton::Variant::Primary);
    emptyHelp_ = new kit::KLabel(tr("Or drag compositions here from Assets."), emptyPage_);
    emptyHelp_->setObjectName(QStringLiteral("exportEmptyHelp"));
    emptyHelp_->setTone(kit::Color::Muted);
    emptyHelp_->setAlignment(Qt::AlignHCenter);
    emptyHelp_->setWordWrap(true);
    emptyLayout->addWidget(emptyText_);
    emptyLayout->addWidget(emptyAdd_, 0, Qt::AlignHCenter);
    emptyLayout->addWidget(emptyHelp_);
    emptyLayout->addStretch(1);
    pages_->addWidget(emptyPage_);
}

void ExportEditor::setCurrentRenderExpanded(const bool expanded) {
    stripExpanded_ = expanded;
    stripBar_->parentWidget()->setVisible(expanded);
    stripToggle_->setIcon(kit::icon(expanded ? kit::IconId::CaretDown : kit::IconId::CaretRight,
                                    kit::IconRole::Chrome));
    stripToggle_->setAccessibleName(expanded ? tr("Hide the progress bar") : tr("Show the progress bar"));
    QSettings().setValue(QString::fromLatin1(kStripKey), expanded);
}

QString ExportEditor::currentRenderText() const { return stripText_->property("fullText").toString(); }

void ExportEditor::selectJob(const document::ExportJobId job) {
    auto* item = jobItem(job);
    if (item == nullptr)
        return;
    list_->setCurrentItem(item);
    item->setExpanded(true);
    // The rows below the job appear as it opens: bring it into view once the layout settles.
    QTimer::singleShot(0, this, [this, job] {
        if (auto* shown = jobItem(job))
            list_->scrollToItem(shown);
    });
}

std::optional<document::ExportJobId> ExportEditor::selectedJob() const {
    auto* item = list_->currentItem();
    while (item != nullptr && item->parent() != nullptr)
        item = item->parent();
    if (item == nullptr || !item->isSelected())
        return std::nullopt;
    return exportpanel::rowJob(item);
}

void ExportEditor::setJobExpanded(const document::ExportJobId job, const bool expanded) {
    if (auto* item = jobItem(job))
        item->setExpanded(expanded);
}

std::vector<document::ExportJobId> ExportEditor::jobOrder() const {
    std::vector<document::ExportJobId> order;
    for (int index = 0; index < list_->topLevelItemCount(); ++index)
        order.push_back(exportpanel::rowJob(list_->topLevelItem(index)));
    return order;
}

QTreeWidgetItem* ExportEditor::jobItem(const document::ExportJobId job) const { return list_->jobItem(job); }

CheckCell* ExportEditor::renderCheck(const document::ExportJobId job) const {
    auto* item = jobItem(job);
    return item != nullptr ? qobject_cast<CheckCell*>(list_->itemWidget(item, exportpanel::RenderColumn))
                           : nullptr;
}

StatusCell* ExportEditor::statusCell(const document::ExportJobId job) const {
    auto* item = jobItem(job);
    return item != nullptr ? qobject_cast<StatusCell*>(list_->itemWidget(item, exportpanel::StatusColumn))
                           : nullptr;
}

RenderSettingsLine* ExportEditor::renderSettingsLine(const document::ExportJobId job) const {
    auto* item = jobItem(job);
    return item != nullptr && item->childCount() > 0
               ? qobject_cast<RenderSettingsLine*>(list_->itemWidget(item->child(0), 0))
               : nullptr;
}

OutputModuleLine* ExportEditor::outputLine(const document::ExportJobId job,
                                           const document::ExportOutputId output) const {
    auto* item = jobItem(job);
    for (int index = 0; item != nullptr && index < item->childCount(); ++index)
        if (exportpanel::rowKind(item->child(index)) == RowKind::Output &&
            exportpanel::rowOutput(item->child(index)) == output)
            return qobject_cast<OutputModuleLine*>(list_->itemWidget(item->child(index), 0));
    return nullptr;
}

SummaryLine* ExportEditor::summaryLine(QTreeWidgetItem* const line) const {
    return line != nullptr && line->childCount() > 0
               ? qobject_cast<SummaryLine*>(list_->itemWidget(line->child(0), 0))
               : nullptr;
}

void ExportEditor::addActiveComposition() {
    if (adapter_ == nullptr)
        return;
    if (const auto composition = session_.compositionId(); composition.isValid())
        (void)adapter_->addCompositionToQueue(composition);
}

void ExportEditor::sync() {
    if (adapter_ == nullptr || syncing_)
        return;
    const auto snapshot = adapter_->snapshot();
    if (!snapshot || snapshot->revision == shownRevision_)
        return;
    syncing_ = true;
    shownRevision_ = snapshot->revision;
    if (coloursStale_) {
        *colours_ = exportpanel::exportColourChoices(session_);
        coloursStale_ = false;
    }
    // A transient job (a script's one-off render) is runtime only: it has no place in the list.
    std::vector<std::pair<std::uint64_t, std::vector<std::uint64_t>>> structure;
    for (const auto& job : snapshot->jobs) {
        if (job.transient)
            continue;
        std::vector<std::uint64_t> outputs;
        for (const auto& output : job.outputs)
            outputs.push_back(output.id.value());
        structure.emplace_back(job.id.value(), std::move(outputs));
    }
    if (structure != shownStructure_) {
        shownStructure_ = structure;
        rebuildItems();
    }
    updateItems();
    updateHeader();
    updateStrip();
    syncing_ = false;
    if (pendingSelection_.has_value() && jobItem(*pendingSelection_) != nullptr) {
        selectJob(*pendingSelection_);
        pendingSelection_.reset();
    }
}

void ExportEditor::rebuildItems() {
    const auto current = selectedJob();
    const int scroll = list_->verticalScrollBar()->value();
    list_->clear();
    for (const auto& [job, outputs] : shownStructure_) {
        auto* item = new QTreeWidgetItem(list_);
        item->setData(0, exportpanel::kRowKindRole, static_cast<int>(RowKind::Job));
        item->setData(0, exportpanel::kJobRole, static_cast<qulonglong>(job));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        const auto line = [job](QTreeWidgetItem* parent, const RowKind kind, const std::uint64_t output) {
            auto* child = new QTreeWidgetItem(parent);
            child->setData(0, exportpanel::kRowKindRole, static_cast<int>(kind));
            child->setData(0, exportpanel::kJobRole, static_cast<qulonglong>(job));
            child->setData(0, exportpanel::kOutputRole, static_cast<qulonglong>(output));
            child->setFlags(Qt::ItemIsEnabled);
            child->setFirstColumnSpanned(true);
            return child;
        };
        auto* settings = line(item, RowKind::RenderSettings, 0);
        list_->setItemWidget(settings, 0, new RenderSettingsLine(*list_, list_));
        list_->setItemWidget(line(settings, RowKind::Summary, 0), 0, new SummaryLine(*list_, list_));
        for (const auto output : outputs) {
            auto* module = line(item, RowKind::Output, output);
            list_->setItemWidget(module, 0, new OutputModuleLine(*list_, list_));
            list_->setItemWidget(line(module, RowKind::Summary, output), 0, new SummaryLine(*list_, list_));
            module->setExpanded(openLines_.contains(lineKey(RowKind::Output, output)));
        }
        settings->setExpanded(openLines_.contains(lineKey(RowKind::RenderSettings, job)));
        list_->setItemWidget(item, exportpanel::RenderColumn, new CheckCell(list_));
        list_->setItemWidget(item, exportpanel::StatusColumn, new StatusCell(list_));
        list_->setItemWidget(item, exportpanel::ActionsColumn, new exportpanel::ActionsCell(list_));
        item->setExpanded(openJobs_.contains(static_cast<qulonglong>(job)));
        connectJob(item);
    }
    if (current.has_value())
        if (auto* item = jobItem(*current))
            list_->setCurrentItem(item);
    list_->verticalScrollBar()->setValue(scroll);
}

void ExportEditor::updateItems() {
    const auto snapshot = adapter_->snapshot();
    const auto catalogue = adapter_->catalogue();
    const auto& project = session_.snapshot().project();
    std::vector<std::pair<QString, QString>> presets;
    if (catalogue != nullptr) {
        struct Group {
            host::ExportPresetCategory category;
            QString title;
        };
        // Video first, then image sequences, audio and the artist's own (ADR 0033, presets).
        const Group groups[] = {{host::ExportPresetCategory::Video, tr("Video")},
                                {host::ExportPresetCategory::VfxSequences, tr("Image sequences")},
                                {host::ExportPresetCategory::Audio, tr("Audio")},
                                {host::ExportPresetCategory::User, tr("My presets")}};
        for (const auto& group : groups) {
            bool header = false;
            for (const auto& preset : catalogue->presets) {
                if (preset.category != group.category)
                    continue;
                if (!header)
                    presets.emplace_back(QString{}, group.title);
                header = true;
                presets.emplace_back(qs(preset.id), qs(preset.displayName));
            }
        }
    }
    const auto folders = exportpanel::recentExportFolders();
    int moduleWidth = 0;

    for (int index = 0; index < list_->topLevelItemCount(); ++index) {
        auto* item = list_->topLevelItem(index);
        const auto* job = findView(snapshot.get(), exportpanel::rowJob(item));
        if (job == nullptr)
            continue;
        const auto* composition = project.findComposition(job->record.composition);
        const auto status = exportpanel::jobStatus(*job);
        const auto& edit = job->commands[host::ExportCommand::EditJob];
        item->setText(exportpanel::NumberColumn, QString::number(index + 1));
        item->setText(exportpanel::CompositionColumn, qs(job->displayName));
        item->setToolTip(exportpanel::CompositionColumn,
                         composition != nullptr ? qs(composition->name()) : tr("Composition removed"));
        item->setText(exportpanel::StatusColumn, status.text);
        const auto started = adapter_->startedAt(job->id);
        item->setText(exportpanel::StartedColumn,
                      job->run.has_value() && started.has_value() ? exportpanel::startedText(*started) : QString{});
        item->setText(exportpanel::RenderTimeColumn, exportpanel::renderTimeText(*job));
        auto* cell = statusCell(job->id);
        cell->show(status, *job);
        item->setData(exportpanel::StatusColumn, Qt::UserRole, cell->barShown());
        if (auto* check = renderCheck(job->id)) {
            const QSignalBlocker block(check->box());
            check->box()->setChecked(job->record.enabled);
            check->box()->setEnabled(edit.available);
            check->box()->setToolTip(edit.available ? tr("Include in Start All") : qs(edit.reason));
        }

        // Render Settings, and what it comes to.
        const bool here = composition != nullptr && job->record.composition == session_.compositionId();
        auto* settings = item->child(0);
        if (auto* line = qobject_cast<RenderSettingsLine*>(list_->itemWidget(settings, 0)))
            line->show(exportpanel::rangeText(*job), edit.available, qs(edit.reason), here,
                       tr("Open this composition to use its current frame."));
        QStringList span;
        if (composition != nullptr)
            if (const auto text = exportpanel::spanText(job->resolvedRange, composition->format().frameRate());
                !text.isEmpty())
                span.push_back(text);
        for (const auto& issue : exportpanel::orderedIssues(job->preflight))
            span.push_back(qs(issue.reason));
        if (auto* summary = summaryLine(settings))
            summary->show(span.isEmpty() ? tr("The range does not fit the composition.")
                                         : span.join(QStringLiteral(" · ")));

        // One Output Module line per output, in order.
        for (int child = 1; child < item->childCount(); ++child) {
            auto* lineItem = item->child(child);
            const auto id = exportpanel::rowOutput(lineItem);
            const auto found = std::ranges::find_if(job->outputs, [id](const auto& o) { return o.id == id; });
            auto* line = qobject_cast<OutputModuleLine*>(list_->itemWidget(lineItem, 0));
            if (found == job->outputs.end() || line == nullptr)
                continue;
            const auto& output = *found;
            OutputLineState state;
            state.module = exportpanel::presetText(output, catalogue.get());
            state.moduleTip = tr("Output Module");
            state.destination = destinationShown(output);
            state.destinationTip = exportpanel::destinationToolTip(output);
            const auto& editOutput = output.commands[host::ExportCommand::EditOutput];
            state.editable = editOutput.available;
            state.editReason = qs(editOutput.reason);
            const auto& add = job->commands[host::ExportCommand::AddOutput];
            state.canAdd = add.available;
            state.addReason = qs(add.reason);
            const auto& remove = output.commands[host::ExportCommand::RemoveOutput];
            state.canRemove = remove.available && job->outputs.size() > 1;
            state.removeReason = state.canRemove ? QString{}
                                 : job->outputs.size() <= 1 ? tr("A job keeps at least one output.")
                                                            : qs(remove.reason);
            state.presets = presets;
            state.recentFolders = folders;
            line->show(state);
            moduleWidth = std::max(moduleWidth, line->moduleLink()->sizeHint().width());
            if (auto* summary = summaryLine(lineItem))
                summary->show(outputSummary(output));
        }
    }
    list_->setModuleWidth(std::min(moduleWidth, kit::px(kit::Size::DropdownWidthExpanded)));
    list_->fitColumns();
    refitLines();

    pages_->setCurrentWidget(list_->topLevelItemCount() == 0 ? static_cast<QWidget*>(emptyPage_)
                                                             : static_cast<QWidget*>(list_));
    const auto reason = adapter_->addUnavailableReason();
    emptyAdd_->setEnabled(session_.compositionId().isValid() && reason.isEmpty());
    emptyAdd_->setToolTip(!session_.compositionId().isValid() ? tr("Open a composition first") : reason);
    addComposition_->setEnabled(emptyAdd_->isEnabled());
    addComposition_->setToolTip(emptyAdd_->isEnabled() ? tr("Add the open composition to the list")
                                                       : emptyAdd_->toolTip());
}

void ExportEditor::refitLines() {
    // Output To folds onto its own row when one row would not fit the list.
    const int room = list_->viewport()->width() - list_->depthLeft(1);
    for (int index = 0; index < list_->topLevelItemCount(); ++index) {
        auto* item = list_->topLevelItem(index);
        for (int child = 0; child < item->childCount(); ++child) {
            auto* lineItem = item->child(child);
            auto* line = qobject_cast<OutputModuleLine*>(list_->itemWidget(lineItem, 0));
            if (line == nullptr)
                continue;
            line->applyModuleWidth();
            const bool fold = line->oneRowWidth() > room;
            if (fold != line->folded() || !lineItem->data(0, Qt::SizeHintRole).isValid()) {
                line->setFolded(fold);
                lineItem->setSizeHint(0, QSize(0, kit::px(kit::Size::AssetRow) * (fold ? 2 : 1)));
            }
        }
    }
}

void ExportEditor::updateHeader() {
    const auto snapshot = adapter_->snapshot();
    const auto& commands = snapshot->commands;
    int shown = 0;
    int ready = 0;
    bool rendering = snapshot->activeJob.has_value();
    for (const auto& job : snapshot->jobs) {
        if (job.transient)
            continue;
        ++shown;
        ready += isReady(job) ? 1 : 0;
        rendering = rendering || host::isActive(job.state);
    }
    // The header row places its controls itself: a control leaves it through "chromeSuppressed".
    const auto show = [](kit::KButton& button, const bool visible) {
        if (button.property("chromeSuppressed").toBool() == visible)
            button.setProperty("chromeSuppressed", !visible);
    };
    // Start All is the one Primary while a job is ready; otherwise it waits, with the reason.
    const auto& start = commands[host::ExportCommand::StartAll];
    startAll_->setVariant(ready > 0 ? kit::KButton::Variant::Primary : kit::KButton::Variant::Secondary);
    startAll_->setEnabled(ready > 0);
    startAll_->setToolTip(ready > 0 ? tr("Render every checked job that is queued")
                          : !start.available && !start.reason.empty() ? qs(start.reason)
                                                                      : tr("No checked job is queued."));
    const auto& stop = commands[host::ExportCommand::StopAll];
    stopAll_->setEnabled(stop.available);
    stopAll_->setToolTip(stop.available ? tr("Stop every job that is rendering") : qs(stop.reason));
    show(*startAll_, shown > 0);
    show(*stopAll_, rendering);
    show(*addComposition_, shown > 0);
}

void ExportEditor::updateStrip() {
    const auto snapshot = adapter_->snapshot();
    const auto* active = snapshot->activeJob.has_value() ? findView(snapshot.get(), *snapshot->activeJob)
                                                         : nullptr;
    auto text = exportpanel::currentRenderText(active);
    const bool running = active != nullptr && host::isActive(active->state);
    const auto& start = snapshot->commands[host::ExportCommand::StartAll];
    // Where export cannot run in this version, the strip says so instead of a silent Idle.
    if (!running && start.code == host::ExportUnavailableCode::PlatformNotSupported)
        text += QStringLiteral(" · ") + qs(start.reason);
    stripText_->setElidedText(text);
    stripText_->setProperty("fullText", text);
    stripText_->setToolTip(text);
    if (running && active->progress.framesTotal > 0 && !exportpanel::isWaitingForViewer(*active))
        stripBar_->setProgress(std::min(active->progress.framesCompleted, active->progress.framesTotal),
                               active->progress.framesTotal);
    else if (running)
        stripBar_->setBusy();
    else
        stripBar_->setProgress(0, 1);
}

// ---- Drag and drop: jobs moved inside the list, compositions dropped from Assets.

std::vector<document::CompositionId> ExportEditor::draggedCompositions(const QMimeData& mime) const {
    std::vector<document::CompositionId> compositions;
    // An Assets drag lists every selected row in selection order; footage and folders are not jobs.
    for (const auto& item : layerItemsFromMime(mime, session_))
        if (const auto* composition = std::get_if<document::CompositionId>(&item))
            if (std::ranges::find(compositions, *composition) == compositions.end())
                compositions.push_back(*composition);
    if (compositions.empty())
        if (const auto single = compositionFromMime(mime, session_); single.isValid())
            compositions.push_back(single);
    return compositions;
}

std::optional<document::ExportJobId> ExportEditor::draggedJob(const QMimeData& mime) const {
    const auto data = mime.data(QString::fromLatin1(exportpanel::kExportJobsMimeType));
    if (data.isEmpty())
        return std::nullopt;
    bool ok = false;
    const auto raw = QString::fromLatin1(data).split(QLatin1Char('\n')).front().toULongLong(&ok);
    if (!ok || jobItem(jobId(raw)) == nullptr)
        return std::nullopt;
    return jobId(raw);
}

bool ExportEditor::acceptsDrag(const QMimeData& mime) const {
    return adapter_ != nullptr && (draggedJob(mime).has_value() || !draggedCompositions(mime).empty());
}

std::size_t ExportEditor::dropIndexAt(const QPoint position) const {
    const auto y = list_->viewport()->mapFrom(this, position).y();
    std::size_t index = 0;
    for (int row = 0; row < list_->topLevelItemCount(); ++row) {
        auto* item = list_->topLevelItem(row);
        // A job and its open lines are one block: the drop goes above or below the whole block.
        auto* last = item;
        while (last->isExpanded() && last->childCount() > 0)
            last = last->child(last->childCount() - 1);
        const auto top = list_->visualItemRect(item).top();
        const auto bottom = list_->visualItemRect(last).bottom();
        if ((top + bottom) / 2 < y)
            ++index;
    }
    return index;
}

bool ExportEditor::dropIndicatorShown() const { return !dropLine_->isHidden() || emptyDropShown_; }

int ExportEditor::dropIndicatorY() const { return dropLine_->geometry().y(); }

void ExportEditor::showDropIndicator(const QPoint position) {
    if (list_->topLevelItemCount() == 0) {
        // Nothing to put a line between: the whole page lights up.
        emptyPage_->setFill(kit::Color::SurfaceRaised);
        emptyDropShown_ = true;
        return;
    }
    const auto index = static_cast<int>(dropIndexAt(position));
    int y = 0;
    if (index < list_->topLevelItemCount()) {
        y = list_->visualItemRect(list_->topLevelItem(index)).top();
    } else {
        auto* last = list_->topLevelItem(list_->topLevelItemCount() - 1);
        while (last->isExpanded() && last->childCount() > 0)
            last = last->child(last->childCount() - 1);
        y = list_->visualItemRect(last).bottom();
    }
    dropLine_->setGeometry(QRect(0, std::max(0, y - dropLine_->height() / 2), list_->viewport()->width(),
                                 dropLine_->height()));
    dropLine_->raise();
    dropLine_->show();
}

void ExportEditor::hideDropIndicator() {
    dropLine_->hide();
    if (emptyDropShown_) {
        emptyPage_->setFill(kit::Color::Background);
        emptyDropShown_ = false;
    }
}

void ExportEditor::dragEnterEvent(QDragEnterEvent* const event) {
    // Not droppable, not accepted: the pointer shows "not allowed".
    if (acceptsDrag(*event->mimeData())) {
        event->acceptProposedAction();
        showDropIndicator(event->position().toPoint());
    } else {
        event->ignore();
    }
}

void ExportEditor::dragMoveEvent(QDragMoveEvent* const event) {
    if (acceptsDrag(*event->mimeData())) {
        event->acceptProposedAction();
        showDropIndicator(event->position().toPoint());
    } else {
        event->ignore();
        hideDropIndicator();
    }
}

void ExportEditor::dragLeaveEvent(QDragLeaveEvent* const event) {
    hideDropIndicator();
    QWidget::dragLeaveEvent(event);
}

void ExportEditor::dropEvent(QDropEvent* const event) {
    hideDropIndicator();
    if (adapter_ == nullptr) {
        event->ignore();
        return;
    }
    const auto index = dropIndexAt(event->position().toPoint());
    if (const auto job = draggedJob(*event->mimeData())) {
        // The job lands where the line was: among the other jobs once it has left its place.
        const auto order = jobOrder();
        const auto from = static_cast<std::size_t>(std::ranges::find(order, *job) - order.begin());
        const auto target = index > from ? index - 1 : index;
        if (target != from)
            report(adapter_->moveJob(*job, target));
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    const auto compositions = draggedCompositions(*event->mimeData());
    if (compositions.empty()) {
        event->ignore();
        return;
    }
    // One job per composition, at the line the artist saw, as one undo entry.
    (void)adapter_->addCompositionsToQueue(
        compositions, list_->topLevelItemCount() == 0 ? std::nullopt : std::optional(index));
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

bool registerExportEditor(EditorRegistry& registry, CompositionSession& session,
                          ExportQueueAdapter* const adapter) {
    return registry.registerEditor(
        {.id = "bloom.export",
         .displayName = ExportEditor::tr("Export"),
         .create = [&session, adapter](QWidget* parent) {
             return new ExportEditor(session, adapter, parent);
         }});
}

} // namespace bloom::ui
