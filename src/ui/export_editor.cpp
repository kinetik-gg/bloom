#include <bloom/ui/export_editor.hpp>

#include "export_editor_internal.hpp"
#include "export_job_tree.hpp"
#include "export_panel_text.hpp"
#include "reveal_in_file_manager.hpp"

#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/surfaces.hpp>

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMenu>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>

namespace bloom::ui {

namespace exportpanel {

ExportRunStrip::ExportRunStrip(QWidget* const parent) : QWidget(parent) {
    setObjectName(QStringLiteral("exportRunStrip"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kit::px(kit::Spacing::S), kit::px(kit::Spacing::S),
                               kit::px(kit::Spacing::S), kit::px(kit::Spacing::S));
    layout->setSpacing(kit::px(kit::Spacing::XS));
    auto* top = new QHBoxLayout;
    top->setSpacing(kit::px(kit::Spacing::S));
    name_ = new kit::KLabel(this);
    name_->setObjectName(QStringLiteral("exportRunName"));
    name_->setTypeRole(kit::TypeRole::UiSmallStrong);
    state_ = new kit::KLabel(this);
    state_->setObjectName(QStringLiteral("exportRunState"));
    state_->setTone(kit::Color::Muted);
    top->addWidget(name_, 1);
    top->addWidget(state_, 0);
    layout->addLayout(top);
    bar_ = new kit::KProgressBar(this);
    bar_->setObjectName(QStringLiteral("exportRunProgress"));
    bar_->setAccessibleName(tr("Export progress"));
    layout->addWidget(bar_);
    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(kit::px(kit::Spacing::S));
    frames_ = new kit::KLabel(QString{}, this, kit::TypeRole::Value);
    frames_->setObjectName(QStringLiteral("exportRunFrames"));
    frames_->setTone(kit::Color::Muted);
    time_ = new kit::KLabel(QString{}, this, kit::TypeRole::Value);
    time_->setObjectName(QStringLiteral("exportRunTime"));
    time_->setTone(kit::Color::Muted);
    bottom->addWidget(frames_, 1);
    bottom->addWidget(time_, 0);
    layout->addLayout(bottom);
    hide();
}

void ExportRunStrip::setJob(const host::ExportJobView* const job) {
    if (job == nullptr) {
        hide();
        return;
    }
    show();
    name_->setElidedText(qs(job->displayName));
    const auto& progress = job->progress;
    state_->setText(runLine(*job));
    if (progress.framesTotal > 0)
        bar_->setProgress(std::min(progress.framesCompleted, progress.framesTotal),
                          progress.framesTotal);
    else
        bar_->setBusy();
    frames_->setText(frameProgressText(progress));
    auto time = elapsedText(progress);
    // No estimate exists while the Viewer holds the GPU: show elapsed time only.
    if (const auto remaining = remainingText(progress); !remaining.isEmpty())
        time += QStringLiteral(" · ") + remaining;
    time_->setText(time);
}

QString ExportRunStrip::nameText() const { return name_->text(); }
QString ExportRunStrip::stateText() const { return state_->text(); }
QString ExportRunStrip::frameText() const { return frames_->text(); }
QString ExportRunStrip::timeText() const { return time_->text(); }

} // namespace exportpanel

using exportpanel::ExportDetailPane;
using exportpanel::ExportJobTree;
using exportpanel::ExportRunStrip;

namespace {

constexpr auto kSplitterKey = "export/splitter-state";

[[nodiscard]] document::ExportJobId jobId(const qulonglong raw) {
    return document::ExportJobId::fromRaw(raw);
}
[[nodiscard]] document::ExportOutputId outputId(const qulonglong raw) {
    return document::ExportOutputId::fromRaw(raw);
}

[[nodiscard]] kit::IconId formatGlyph(const document::ExportFormat format) {
    switch (format) {
    case document::ExportFormat::Png:
    case document::ExportFormat::OpenExr:
    case document::ExportFormat::Tiff:
    case document::ExportFormat::Dpx:
        return kit::IconId::Images;
    case document::ExportFormat::Wav:
        return kit::IconId::Audio;
    default:
        return kit::IconId::Clip;
    }
}

} // namespace

ExportEditor::ExportEditor(CompositionSession& session, ExportQueueAdapter* const adapter,
                           QWidget* const parent)
    : QWidget(parent), session_(session), adapter_(adapter) {
    setObjectName(QStringLiteral("exportEditor"));
    setAccessibleName(tr("Export editor"));
    setAccessibleDescription(tr("The render queue: jobs, their outputs and their progress"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    chrome_.header.objectName = QStringLiteral("exportHeader");
    chrome_.header.owner = this;
    const auto headerButton = [this](const QString& text, const QString& name) {
        auto* button = new kit::KButton(text, this);
        button->setObjectName(name);
        return button;
    };
    startAll_ = headerButton(tr("Start All"), QStringLiteral("exportStartAllButton"));
    stopAll_ = headerButton(tr("Stop All"), QStringLiteral("exportStopAllButton"));
    removeFinished_ =
        headerButton(tr("Remove finished"), QStringLiteral("exportRemoveFinishedButton"));
    chrome_.header.addStretch();
    chrome_.header.addWidget(removeFinished_);
    chrome_.header.addWidget(stopAll_);
    chrome_.header.addWidget(startAll_);

    strip_ = new ExportRunStrip(this);
    layout->addWidget(strip_);

    // Export runs on Linux in this version; the queue stays editable everywhere.
    platformNote_ = new kit::KLabel(this);
    platformNote_->setObjectName(QStringLiteral("exportPlatformNote"));
    platformNote_->setTone(kit::Color::Muted);
    platformNote_->setContentsMargins(kit::px(kit::Spacing::S), kit::px(kit::Spacing::XS),
                                      kit::px(kit::Spacing::S), kit::px(kit::Spacing::XS));
    platformNote_->hide();
    layout->addWidget(platformNote_);

    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->setObjectName(QStringLiteral("exportSplitter"));
    splitter_->setChildrenCollapsible(false);
    splitter_->setHandleWidth(kit::px(kit::Size::SplitHandle));
    layout->addWidget(splitter_, 1);

    pages_ = new QStackedWidget(splitter_);
    tree_ = new ExportJobTree(session_, pages_);
    pages_->addWidget(tree_);
    emptyPage_ = new kit::KSurface(pages_, kit::Color::Background);
    emptyPage_->setObjectName(QStringLiteral("exportEmptyState"));
    auto* emptyLayout = new QVBoxLayout(emptyPage_);
    emptyLayout->setContentsMargins(kit::px(kit::Spacing::L), kit::px(kit::Spacing::L),
                                    kit::px(kit::Spacing::L), kit::px(kit::Spacing::L));
    emptyLayout->setSpacing(kit::px(kit::Spacing::M));
    emptyLayout->addStretch(1);
    emptyText_ = new kit::KLabel(emptyPage_);
    emptyText_->setObjectName(QStringLiteral("exportEmptyText"));
    emptyText_->setTone(kit::Color::Muted);
    emptyText_->setAlignment(Qt::AlignHCenter);
    emptyText_->setWordWrap(true);
    emptyAdd_ = new kit::KButton(tr("Add active composition"), emptyPage_);
    emptyAdd_->setObjectName(QStringLiteral("exportAddActiveButton"));
    emptyLayout->addWidget(emptyText_);
    emptyLayout->addWidget(emptyAdd_, 0, Qt::AlignHCenter);
    emptyLayout->addStretch(1);
    pages_->addWidget(emptyPage_);
    splitter_->addWidget(pages_);

    if (adapter_ != nullptr)
        detail_ = new ExportDetailPane(session_, *adapter_, nullptr);
    auto* scroll = new QScrollArea(splitter_);
    scroll->setObjectName(QStringLiteral("exportDetailScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* surface = new kit::KSurface(nullptr, kit::Color::Background);
    auto* surfaceLayout = new QVBoxLayout(surface);
    surfaceLayout->setContentsMargins(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::Gutter),
                                      kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XS));
    if (detail_ != nullptr)
        surfaceLayout->addWidget(detail_);
    surfaceLayout->addStretch(1);
    scroll->setWidget(surface);
    splitter_->addWidget(scroll);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 1);
    detailScroll_ = scroll;
    scroll->hide();

    try {
        QSettings settings;
        splitter_->restoreState(settings.value(QString::fromLatin1(kSplitterKey)).toByteArray());
    } catch (...) {
    }
    connect(splitter_, &QSplitter::splitterMoved, this, [this] {
        QSettings settings;
        settings.setValue(QString::fromLatin1(kSplitterKey), splitter_->saveState());
    });

    if (adapter_ == nullptr) {
        emptyText_->setText(tr("Export is not available in this window."));
        emptyAdd_->hide();
        startAll_->setEnabled(false);
        stopAll_->setEnabled(false);
        removeFinished_->setEnabled(false);
        startAll_->setToolTip(tr("Export is not available in this window"));
        pages_->setCurrentWidget(emptyPage_);
        return;
    }

    connect(adapter_, &ExportQueueAdapter::rebound, this,
            [this] { shownRevision_ = ~std::uint64_t{0}; });
    connect(adapter_, &ExportQueueAdapter::queueChanged, this, &ExportEditor::sync);
    connect(adapter_, &ExportQueueAdapter::jobAdded, this,
            [this](const qulonglong job) { selectJob(jobId(job)); });
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        // Composition names and rates read from the project.
        shownRevision_ = ~std::uint64_t{0};
        sync();
    });
    connect(startAll_, &kit::KButton::clicked, this, &ExportEditor::startAll);
    connect(stopAll_, &kit::KButton::clicked, this, [this] { (void)adapter_->stopAll(); });
    connect(removeFinished_, &kit::KButton::clicked, this,
            [this] { (void)adapter_->removeFinished(); });
    connect(emptyAdd_, &kit::KButton::clicked, this, &ExportEditor::addActiveComposition);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &ExportEditor::updateSelection);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this,
            &ExportEditor::showContextMenu);
    connect(tree_, &ExportJobTree::enabledToggled, this,
            [this](const qulonglong job, const qulonglong output, const bool enabled) {
                if (output == 0)
                    (void)adapter_->editJob(jobId(job), jobEdit([&](auto& edit) { edit.enabled = enabled; }));
                else
                    (void)adapter_->editOutput(jobId(job), outputId(output), outputEdit([&](auto& edit) { edit.enabled = enabled; }));
            });
    connect(tree_, &ExportJobTree::moveRequested, this,
            [this](const qulonglong job, const int index) {
                (void)adapter_->moveJob(jobId(job), static_cast<std::size_t>(index));
            });
    connect(tree_, &ExportJobTree::compositionsDropped, this,
            [this](const QList<qulonglong>& compositions) {
                for (const auto composition : compositions)
                    (void)adapter_->addCompositionToQueue(document::CompositionId::fromRaw(composition));
            });
    connect(tree_, &ExportJobTree::removeRequested, this, [this] {
        if (const auto output = selectedOutput(); output.has_value() && selectedJob().has_value())
            (void)adapter_->removeOutput(*selectedJob(), *output);
        else if (const auto job = selectedJob())
            (void)adapter_->removeJob(*job);
    });

    sync();
    if (const auto pending = adapter_->takePendingSelection())
        selectJob(*pending);
}

ExportEditor::~ExportEditor() = default;

void ExportEditor::selectJob(const document::ExportJobId job) {
    if (auto* item = tree_->jobItem(static_cast<qulonglong>(job.value()))) {
        tree_->setCurrentItem(item);
        item->setSelected(true);
    }
}

std::optional<document::ExportJobId> ExportEditor::selectedJob() const {
    const auto* item = tree_->currentItem();
    if (item == nullptr || !item->isSelected())
        return std::nullopt;
    return jobId(item->data(exportpanel::NameColumn, exportpanel::kJobRole).toULongLong());
}

std::optional<document::ExportOutputId> ExportEditor::selectedOutput() const {
    const auto* item = tree_->currentItem();
    if (item == nullptr || !item->isSelected())
        return std::nullopt;
    const auto output = item->data(exportpanel::NameColumn, exportpanel::kOutputRole).toULongLong();
    return output != 0 ? std::optional(outputId(output)) : std::nullopt;
}

void ExportEditor::addActiveComposition() {
    if (adapter_ == nullptr)
        return;
    if (const auto composition = session_.compositionId(); composition.isValid())
        (void)adapter_->addCompositionToQueue(composition);
}

void ExportEditor::startAll() {
    const auto snapshot = adapter_->snapshot();
    if (!snapshot)
        return;
    // Replacing an existing file is never silent: ask once for every output that would.
    std::vector<std::pair<document::ExportJobId, document::ExportOutputId>> needing;
    QStringList names;
    for (const auto& job : snapshot->jobs) {
        if (!job.record.enabled || job.state != host::ExportJobState::Ready)
            continue;
        for (const auto& output : job.outputs)
            if (output.record.enabled &&
                exportpanel::hasIssue(output, host::ExportPreflightCode::DestinationExists) &&
                !output.replaceConfirmed) {
                needing.emplace_back(job.id, output.id);
                names.push_back(exportpanel::qs(job.displayName));
            }
    }
    if (needing.empty()) {
        (void)adapter_->startAll();
        return;
    }
    kit::KConfirmSpec spec;
    spec.title = tr("Start All");
    spec.objectName = QStringLiteral("exportReplaceConfirm");
    spec.tone = kit::KPromptTone::Warning;
    spec.heading = tr("Replace existing files?");
    spec.detail = tr("These jobs would replace files that already exist: %1.")
                      .arg(names.join(QStringLiteral(", ")));
    spec.destructiveText = tr("Replace and Start");
    spec.rejectText = tr("Cancel");
    spec.defaultRole = kit::KDialog::Role::Reject;
    kit::confirm(*this, spec, [this, needing](const kit::KDialog::Role role) {
        if (role != kit::KDialog::Role::Destructive)
            return;
        for (const auto& [job, output] : needing)
            (void)adapter_->confirmReplace(job, output);
        (void)adapter_->startAll();
    });
}

void ExportEditor::sync() {
    if (adapter_ == nullptr || syncing_)
        return;
    const auto snapshot = adapter_->snapshot();
    if (!snapshot || snapshot->revision == shownRevision_)
        return;
    syncing_ = true;
    shownRevision_ = snapshot->revision;
    rebuild();
    updateHeader();
    syncing_ = false;
    updateSelection();
}

void ExportEditor::rebuild() {
    const auto snapshot = adapter_->snapshot();
    const auto catalogue = adapter_->catalogue();
    // A transient job (a script's one-off render) is runtime only: it has no place in the queue list.
    std::vector<const host::ExportJobView*> jobs;
    for (const auto& candidate : snapshot->jobs)
        if (!candidate.transient)
            jobs.push_back(&candidate);
    const auto& project = session_.snapshot().project();
    // Remember the selection and the expanded jobs by id: rows are keyed by stable ids.
    const auto selectedJobId = selectedJob();
    const auto selectedOutputId = selectedOutput();

    QString signature;
    for (const auto* shown : jobs) {
        const auto& job = *shown;
        signature += QString::number(job.id.value()) + QLatin1Char(':');
        for (const auto& output : job.outputs)
            signature += QString::number(output.id.value()) + QLatin1Char(',');
        signature += QLatin1Char(';');
    }
    const bool sameShape = signature == rowSignature_;
    rowSignature_ = signature;
    const QSignalBlocker blocker(tree_);
    if (!sameShape)
        tree_->clear();

    const auto fill = [&](QTreeWidgetItem& item, const host::ExportJobView& job) {
        using namespace exportpanel;
        const auto status = jobStatus(job);
        const bool editable = job.commands[host::ExportCommand::EditJob].available;
        const auto* composition = project.findComposition(job.record.composition);
        item.setData(NameColumn, kJobRole, static_cast<qulonglong>(job.id.value()));
        item.setData(NameColumn, kOutputRole, qulonglong{0});
        item.setData(NameColumn, kGlyphRole, static_cast<int>(kit::IconId::Composition));
        item.setText(NameColumn, qs(job.displayName));
        item.setToolTip(NameColumn, qs(job.displayName));
        item.setText(CompositionColumn,
                     composition ? qs(composition->name()) : tr("Missing composition"));
        item.setText(RangeColumn, rangeText(job));
        item.setToolTip(RangeColumn,
                        composition ? rangeTimecodeText(job, composition->format().frameRate())
                                    : QString{});
        item.setText(StatusColumn, status.text);
        item.setData(StatusColumn, kToneRole, static_cast<int>(status.tone));
        item.setToolTip(StatusColumn, status.detail);
        item.setData(EnabledColumn, kEditableRole, editable);
        item.setCheckState(EnabledColumn, job.record.enabled ? Qt::Checked : Qt::Unchecked);
        item.setToolTip(EnabledColumn,
                        editable ? tr("Included by Start All")
                                 : exportpanel::qs(job.commands[host::ExportCommand::EditJob].reason));
        if (job.record.labelColor.has_value()) {
            const auto& rgb = *job.record.labelColor;
            item.setData(LabelColumn, kLabelRole, QColor(rgb[0], rgb[1], rgb[2]));
            item.setToolTip(LabelColumn, tr("Label colour"));
        } else {
            item.setData(LabelColumn, kLabelRole, QVariant{});
        }
        item.setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
    };
    const auto fillOutput = [&](QTreeWidgetItem& item, const host::ExportJobView& job,
                                const host::ExportOutputView& output) {
        using namespace exportpanel;
        const auto status = outputStatus(output);
        const bool editable = output.commands[host::ExportCommand::EditOutput].available;
        item.setData(NameColumn, kJobRole, static_cast<qulonglong>(job.id.value()));
        item.setData(NameColumn, kOutputRole, static_cast<qulonglong>(output.id.value()));
        item.setData(NameColumn, kGlyphRole, static_cast<int>(formatGlyph(output.record.settings.format)));
        item.setText(NameColumn, presetText(output, catalogue.get()));
        item.setData(CompositionColumn, kDestinationRole, destinationText(output));
        item.setToolTip(NameColumn, destinationToolTip(output));
        item.setToolTip(CompositionColumn, destinationToolTip(output));
        item.setToolTip(RangeColumn, destinationToolTip(output));
        item.setText(StatusColumn, status.text);
        item.setData(StatusColumn, kToneRole, static_cast<int>(status.tone));
        item.setToolTip(StatusColumn, status.detail);
        item.setData(EnabledColumn, kEditableRole, editable);
        item.setCheckState(EnabledColumn, output.record.enabled ? Qt::Checked : Qt::Unchecked);
        item.setToolTip(EnabledColumn,
                        editable ? tr("Render this output")
                                 : exportpanel::qs(output.commands[host::ExportCommand::EditOutput].reason));
        item.setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    };

    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const auto& job = *jobs[i];
        QTreeWidgetItem* item = sameShape ? tree_->topLevelItem(static_cast<int>(i)) : new QTreeWidgetItem;
        if (!sameShape)
            tree_->addTopLevelItem(item);
        fill(*item, job);
        for (std::size_t o = 0; o < job.outputs.size(); ++o) {
            QTreeWidgetItem* child = sameShape ? item->child(static_cast<int>(o)) : new QTreeWidgetItem(item);
            fillOutput(*child, job, job.outputs[o]);
        }
        if (!sameShape)
            item->setExpanded(true);
    }
    tree_->viewport()->update();

    if (!sameShape && selectedJobId.has_value()) {
        auto* item = selectedOutputId.has_value()
                         ? tree_->outputItem(selectedJobId->value(), selectedOutputId->value())
                         : tree_->jobItem(selectedJobId->value());
        if (item == nullptr)
            item = tree_->jobItem(selectedJobId->value());
        if (item != nullptr) {
            tree_->setCurrentItem(item);
            item->setSelected(true);
        }
    }
    pages_->setCurrentWidget(jobs.empty() ? static_cast<QWidget*>(emptyPage_)
                                                    : static_cast<QWidget*>(tree_));
    const auto reason = adapter_->addUnavailableReason();
    emptyText_->setText(tr("The export queue is empty. Add a composition to render it."));
    emptyAdd_->setEnabled(session_.compositionId().isValid() && reason.isEmpty());
    emptyAdd_->setToolTip(!session_.compositionId().isValid() ? tr("Open a composition first")
                                                             : reason);
}

void ExportEditor::updateHeader() {
    const auto snapshot = adapter_->snapshot();
    const auto set = [](kit::KButton& button, const host::ExportCommandAvailability& availability,
                        const QString& tip) {
        button.setEnabled(availability.available);
        button.setToolTip(availability.available ? tip : exportpanel::qs(availability.reason));
    };
    const auto& commands = snapshot->commands;
    set(*startAll_, commands[host::ExportCommand::StartAll], tr("Start every enabled job"));
    set(*stopAll_, commands[host::ExportCommand::StopAll], tr("Stop every active job"));
    set(*removeFinished_, commands[host::ExportCommand::RemoveFinished],
        tr("Remove the jobs that finished"));
    // The one affirmative action while something is ready to run and nothing runs yet.
    startAll_->setVariant(commands[host::ExportCommand::StartAll].available &&
                                  !snapshot->activeJob.has_value()
                              ? kit::KButton::Variant::Primary
                              : kit::KButton::Variant::Secondary);
    const auto& start = commands[host::ExportCommand::StartAll];
    const bool unsupported = start.code == host::ExportUnavailableCode::PlatformNotSupported;
    platformNote_->setVisible(unsupported);
    platformNote_->setText(unsupported ? exportpanel::qs(start.reason) : QString{});

    const host::ExportJobView* active = nullptr;
    if (snapshot->activeJob.has_value())
        for (const auto& job : snapshot->jobs)
            if (job.id == *snapshot->activeJob)
                active = &job;
    strip_->setJob(active);
}

void ExportEditor::updateSelection() {
    if (syncing_ || detail_ == nullptr)
        return;
    const auto job = selectedJob();
    detail_->setSelection(job, selectedOutput());
    const bool firstShow = job.has_value() && !detailScroll_->isVisible();
    detailScroll_->setVisible(job.has_value());
    if (firstShow && !splitterSized_) {
        splitterSized_ = true;
        // The list above, the detail below, until the artist drags the handle.
        QSettings settings;
        if (!settings.contains(QString::fromLatin1(kSplitterKey)))
            QTimer::singleShot(0, this, [this] {
                // The list takes what its rows need (at most half); the detail gets the rest.
                int rows = 0;
                for (QTreeWidgetItemIterator it(tree_); *it; ++it)
                    ++rows;
                const int total = splitter_->height();
                const int wanted = (rows + 1) * kit::px(kit::Size::AssetRow) +
                                   kit::px(kit::Size::AssetRow);
                const int list = std::clamp(wanted, total / 4, total / 2);
                splitter_->setSizes({list, total - list});
            });
    }
}

void ExportEditor::showContextMenu(const QPoint position) {
    auto* item = tree_->itemAt(position);
    if (item == nullptr)
        return;
    if (!item->isSelected()) {
        tree_->setCurrentItem(item);
        item->setSelected(true);
    }
    const auto snapshot = adapter_->snapshot();
    const auto job = selectedJob();
    if (!snapshot || !job.has_value())
        return;
    const host::ExportJobView* view = nullptr;
    for (const auto& candidate : snapshot->jobs)
        if (candidate.id == *job)
            view = &candidate;
    if (view == nullptr)
        return;
    const auto output = selectedOutput();
    const host::ExportOutputView* outputView = nullptr;
    for (const auto& candidate : view->outputs)
        if (output.has_value() && candidate.id == *output)
            outputView = &candidate;
    if (outputView == nullptr && !view->outputs.empty())
        outputView = &view->outputs.front();

    std::unique_ptr<QMenu> menu(kit::makeMenu(this));
    menu->setObjectName(QStringLiteral("exportContextMenu"));
    menu->setToolTipsVisible(true);
    const auto add = [&](const QString& text, const char* name,
                         const host::ExportCommandAvailability& availability, auto&& slot) {
        auto* action = menu->addAction(text);
        action->setObjectName(QString::fromLatin1(name));
        action->setEnabled(availability.available);
        if (!availability.available)
            action->setToolTip(exportpanel::qs(availability.reason));
        connect(action, &QAction::triggered, this, std::forward<decltype(slot)>(slot));
        return action;
    };
    const auto id = *job;
    add(tr("Start"), "exportStartAction", view->commands[host::ExportCommand::Start],
        [this, id] { (void)adapter_->start(id); });
    add(tr("Pause"), "exportPauseAction", view->commands[host::ExportCommand::Pause],
        [this, id] { (void)adapter_->pause(id); });
    add(tr("Resume"), "exportResumeAction", view->commands[host::ExportCommand::Resume],
        [this, id] { (void)adapter_->resume(id); });
    add(tr("Stop"), "exportStopAction", view->commands[host::ExportCommand::Stop],
        [this, id] { (void)adapter_->stop(id); });
    add(tr("Retry"), "exportRetryAction", view->commands[host::ExportCommand::Retry],
        [this, id] { (void)adapter_->retry(id); });
    menu->addSeparator();
    add(tr("Duplicate"), "exportDuplicateAction", view->commands[host::ExportCommand::DuplicateJob],
        [this, id] { (void)adapter_->duplicateJob(id); });
    add(tr("Remove"), "exportRemoveAction", view->commands[host::ExportCommand::RemoveJob],
        [this, id] { (void)adapter_->removeJob(id); });
    menu->addSeparator();
    const host::ExportCommandAvailability reveal =
        outputView != nullptr ? outputView->commands[host::ExportCommand::RevealOutput]
                              : host::ExportCommandAvailability{};
    add(tr("Reveal Output"), "exportRevealAction", reveal, [this, outputView] {
        if (outputView == nullptr)
            return;
        const auto& artifacts = outputView->verification;
        revealInFileManager(artifacts && !artifacts->artifacts.firstPath.empty()
                                ? artifacts->artifacts.firstPath
                                : outputView->destination.firstPath,
                            this);
    });
    menu->addSeparator();
    const auto copy = [this, view](const host::ExportScriptFormat format) {
        QGuiApplication::clipboard()->setText(exportpanel::qs(adapter_->jobScript(view->record, format)));
    };
    menu->addAction(tr("Copy as bloom-cli Command"), this,
                    [copy] { copy(host::ExportScriptFormat::CliJson); })
        ->setObjectName(QStringLiteral("exportCopyCliAction"));
    menu->addAction(tr("Copy as Python"), this, [copy] { copy(host::ExportScriptFormat::Python); })
        ->setObjectName(QStringLiteral("exportCopyPythonAction"));
    menu->exec(tree_->viewport()->mapToGlobal(position));
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
