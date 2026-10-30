#include <bloom/ui/export_editor.hpp>

#include "asset_drop.hpp"
#include "export_colour_choices.hpp"
#include "export_job_row.hpp"
#include "export_panel_text.hpp"

#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/surfaces.hpp>

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <variant>

namespace bloom::ui {

using exportpanel::ExportColourChoices;
using exportpanel::JobRow;

namespace {

[[nodiscard]] document::ExportJobId jobId(const qulonglong raw) {
    return document::ExportJobId::fromRaw(raw);
}

// A job the artist can start now: it is on, and Start is the only thing missing. A platform that
// cannot render yet still shows its jobs as ready to run when it can.
[[nodiscard]] bool isReady(const host::ExportJobView& job) {
    if (!job.record.enabled || job.state != host::ExportJobState::Ready)
        return false;
    const auto& start = job.commands[host::ExportCommand::Start];
    return start.available || start.code == host::ExportUnavailableCode::PlatformNotSupported;
}

} // namespace

ExportEditor::ExportEditor(CompositionSession& session, ExportQueueAdapter* const adapter,
                           QWidget* const parent)
    : QWidget(parent), session_(session), adapter_(adapter),
      colours_(std::make_unique<ExportColourChoices>()) {
    setObjectName(QStringLiteral("exportEditor"));
    setAccessibleName(tr("Export editor"));
    setAccessibleDescription(tr("The render queue: jobs, their outputs and their progress"));
    setAcceptDrops(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

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
    removeFinished_ = headerButton(tr("Remove finished"), QStringLiteral("exportRemoveFinishedButton"),
                                   kit::KButton::Variant::Secondary);
    stopAll_ = headerButton(tr("Stop All"), QStringLiteral("exportStopAllButton"),
                            kit::KButton::Variant::Danger);
    // The one Primary of the panel; hidden until a job is ready.
    startAll_ = headerButton(tr("Start All"), QStringLiteral("exportStartAllButton"),
                             kit::KButton::Variant::Primary);
    chrome_.header.addWidget(addComposition_);
    chrome_.header.addStretch();
    chrome_.header.addWidget(removeFinished_);
    chrome_.header.addWidget(stopAll_);
    chrome_.header.addWidget(startAll_);
    for (auto* button : {addComposition_, removeFinished_, stopAll_, startAll_})
        button->setProperty("chromeSuppressed", true);

    // Export runs on Linux in this version; the queue stays editable everywhere.
    const auto note = [this](const QString& name) {
        auto* label = new kit::KLabel(this);
        label->setObjectName(name);
        label->setTone(kit::Color::Muted);
        label->setWordWrap(true);
        label->setContentsMargins(kit::px(kit::Spacing::S), kit::px(kit::Spacing::XS),
                                  kit::px(kit::Spacing::S), kit::px(kit::Spacing::XS));
        label->hide();
        return label;
    };
    platformNote_ = note(QStringLiteral("exportPlatformNote"));
    startNote_ = note(QStringLiteral("exportStartNote"));
    layout->addWidget(platformNote_);
    layout->addWidget(startNote_);

    pages_ = new QStackedWidget(this);
    layout->addWidget(pages_, 1);

    scroll_ = new QScrollArea(pages_);
    scroll_->setObjectName(QStringLiteral("exportJobList"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* surface = new kit::KSurface(nullptr, kit::Color::Background);
    rows_ = new QVBoxLayout(surface);
    rows_->setContentsMargins(0, 0, 0, 0);
    rows_->setSpacing(0);
    rows_->addStretch(1);
    scroll_->setWidget(surface);
    // The line a drop will land on, between two jobs.
    dropLine_ = new kit::KSurface(surface, kit::Color::Accent);
    dropLine_->setObjectName(QStringLiteral("exportDropLine"));
    dropLine_->setFixedHeight(kit::px(kit::Spacing::XS));
    dropLine_->hide();
    pages_->addWidget(scroll_);

    emptyPage_ = new kit::KSurface(pages_, kit::Color::Background);
    emptyPage_->setObjectName(QStringLiteral("exportEmptyState"));
    auto* emptyLayout = new QVBoxLayout(emptyPage_);
    emptyLayout->setContentsMargins(kit::px(kit::Spacing::L), kit::px(kit::Spacing::L),
                                    kit::px(kit::Spacing::L), kit::px(kit::Spacing::L));
    emptyLayout->setSpacing(kit::px(kit::Spacing::M));
    emptyLayout->addStretch(1);
    emptyText_ = new kit::KLabel(emptyPage_);
    emptyText_->setObjectName(QStringLiteral("exportEmptyText"));
    emptyText_->setTypeRole(kit::TypeRole::Title);
    emptyText_->setAlignment(Qt::AlignHCenter);
    emptyText_->setWordWrap(true);
    emptyText_->setText(tr("Nothing to render yet."));
    auto* emptyLead = new kit::KLabel(emptyPage_);
    emptyLead->setObjectName(QStringLiteral("exportEmptyLead"));
    emptyLead->setTone(kit::Color::Muted);
    emptyLead->setAlignment(Qt::AlignHCenter);
    emptyLead->setWordWrap(true);
    emptyLead->setText(tr("Add a composition to turn it into a video, images or audio."));
    emptyAdd_ = new kit::KButton(tr("Add composition"), emptyPage_);
    emptyAdd_->setObjectName(QStringLiteral("exportAddActiveButton"));
    emptyAdd_->setVariant(kit::KButton::Variant::Primary);
    emptyHelp_ = new kit::KLabel(emptyPage_);
    emptyHelp_->setObjectName(QStringLiteral("exportEmptyHelp"));
    emptyHelp_->setTone(kit::Color::Muted);
    emptyHelp_->setAlignment(Qt::AlignHCenter);
    emptyHelp_->setWordWrap(true);
    emptyHelp_->setText(tr("You can also drag one in from Assets, or press Ctrl+M."));
    emptyLayout->addWidget(emptyText_);
    emptyLayout->addWidget(emptyLead);
    emptyLayout->addWidget(emptyAdd_, 0, Qt::AlignHCenter);
    emptyLayout->addWidget(emptyHelp_);
    emptyLayout->addStretch(1);
    pages_->addWidget(emptyPage_);

    if (adapter_ == nullptr) {
        emptyText_->setText(tr("Export is not available in this window."));
        emptyLead->hide();
        emptyHelp_->hide();
        emptyAdd_->hide();
        pages_->setCurrentWidget(emptyPage_);
        return;
    }

    connect(adapter_, &ExportQueueAdapter::rebound, this, [this] { shownRevision_ = ~std::uint64_t{0}; });
    connect(adapter_, &ExportQueueAdapter::queueChanged, this, &ExportEditor::sync);
    connect(adapter_, &ExportQueueAdapter::jobAdded, this,
            [this](const qulonglong job) {
                // The queue's change was announced first: the row usually exists by now.
                if (jobRow(jobId(job)) != nullptr)
                    selectJob(jobId(job));
                else
                    pendingSelection_ = jobId(job);
            });
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        // Composition names and rates read from the project.
        shownRevision_ = ~std::uint64_t{0};
        sync();
    });
    connect(&session_, &CompositionSession::colorSettingsChanged, this, [this] { coloursStale_ = true; });
    connect(startAll_, &kit::KButton::clicked, this, &ExportEditor::startAll);
    connect(stopAll_, &kit::KButton::clicked, this, [this] { (void)adapter_->stopAll(); });
    connect(removeFinished_, &kit::KButton::clicked, this, [this] { (void)adapter_->removeFinished(); });
    connect(addComposition_, &kit::KButton::clicked, this, &ExportEditor::addActiveComposition);
    connect(emptyAdd_, &kit::KButton::clicked, this, &ExportEditor::addActiveComposition);

    if (const auto pending = adapter_->takePendingSelection())
        pendingSelection_ = *pending;
    sync();
}

ExportEditor::~ExportEditor() = default;

void ExportEditor::selectJob(const document::ExportJobId job) {
    current_ = job;
    for (auto* row : jobRows_)
        row->setCurrent(row->job() == job);
    if (auto* row = jobRow(job)) {
        row->setExpanded(true);
        // The row's own height changes as it opens: bring it into view once the layout settles.
        QTimer::singleShot(0, this, [this, job] {
            if (auto* shown = jobRow(job))
                scroll_->ensureWidgetVisible(shown, 0, kit::px(kit::Spacing::M));
        });
    }
}

std::optional<document::ExportJobId> ExportEditor::selectedJob() const { return current_; }

std::vector<JobRow*> ExportEditor::jobRows() const { return jobRows_; }

JobRow* ExportEditor::jobRow(const document::ExportJobId job) const {
    for (auto* row : jobRows_)
        if (row->job() == job)
            return row;
    return nullptr;
}

QString ExportEditor::startNoteText() const { return startNote_->isHidden() ? QString{} : startNote_->text(); }

void ExportEditor::addActiveComposition() {
    if (adapter_ == nullptr)
        return;
    if (const auto composition = session_.compositionId(); composition.isValid())
        (void)adapter_->addCompositionToQueue(composition);
}

void ExportEditor::startAll() {
    const auto before = adapter_->snapshot();
    if (!before)
        return;
    int startable = 0;
    for (const auto& job : before->jobs)
        if (!job.transient && job.record.enabled && job.commands[host::ExportCommand::Start])
            ++startable;
    // Start All never asks a second question: it starts what can run and says what waits.
    (void)adapter_->startAll();
    startedByStartAll_ = startable;
    shownRevision_ = ~std::uint64_t{0};
    sync();
}

void ExportEditor::moveJob(const qulonglong job, const int direction) {
    const auto found = std::ranges::find_if(jobRows_, [job](const JobRow* row) {
        return static_cast<qulonglong>(row->job().value()) == job;
    });
    if (found == jobRows_.end())
        return;
    const auto index = static_cast<int>(found - jobRows_.begin());
    const auto target = index + direction;
    if (target < 0 || target >= static_cast<int>(jobRows_.size()))
        return;
    (void)adapter_->moveJob(jobId(job), static_cast<std::size_t>(target));
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
}

void ExportEditor::rebuild() {
    const auto snapshot = adapter_->snapshot();
    const auto catalogue = adapter_->catalogue();
    if (coloursStale_) {
        *colours_ = exportpanel::exportColourChoices(session_);
        coloursStale_ = false;
    }
    // A transient job (a script's one-off render) is runtime only: it has no place in the list.
    std::vector<const host::ExportJobView*> jobs;
    for (const auto& candidate : snapshot->jobs)
        if (!candidate.transient)
            jobs.push_back(&candidate);

    std::vector<JobRow*> rows;
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const auto& job = *jobs[i];
        JobRow* row = jobRow(job.id);
        if (row == nullptr) {
            row = new JobRow(session_, *adapter_, job.id, rows_->parentWidget());
            connect(row, &JobRow::activated, this, [this](const qulonglong id) { selectJob(jobId(id)); });
            connect(row, &JobRow::moveRequested, this,
                    [this](const qulonglong id, const int direction) { moveJob(id, direction); });
        }
        // The list order is the queue's: a row at the wrong place is re-inserted there.
        if (rows_->indexOf(row) != static_cast<int>(i)) {
            rows_->removeWidget(row);
            rows_->insertWidget(static_cast<int>(i), row);
        }
        rows.push_back(row);
    }
    for (auto* row : jobRows_)
        if (std::ranges::find(rows, row) == rows.end()) {
            rows_->removeWidget(row);
            delete row;
        }
    jobRows_ = rows;
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        rows[i]->setPosition(static_cast<int>(i), static_cast<int>(jobs.size()));
        rows[i]->update(*jobs[i], catalogue.get(), *colours_);
        rows[i]->setCurrent(current_.has_value() && rows[i]->job() == *current_);
    }
    if (current_.has_value() && jobRow(*current_) == nullptr)
        current_.reset();
    if (pendingSelection_.has_value() && jobRow(*pendingSelection_) != nullptr) {
        selectJob(*pendingSelection_);
        pendingSelection_.reset();
    }

    pages_->setCurrentWidget(jobs.empty() ? static_cast<QWidget*>(emptyPage_)
                                          : static_cast<QWidget*>(scroll_));
    const auto reason = adapter_->addUnavailableReason();
    emptyAdd_->setEnabled(session_.compositionId().isValid() && reason.isEmpty());
    emptyAdd_->setToolTip(!session_.compositionId().isValid() ? tr("Open a composition first") : reason);
    addComposition_->setEnabled(emptyAdd_->isEnabled());
    addComposition_->setToolTip(emptyAdd_->isEnabled() ? tr("Add the open composition to the list")
                                                       : emptyAdd_->toolTip());
}

void ExportEditor::updateHeader() {
    const auto snapshot = adapter_->snapshot();
    const auto set = [](kit::KButton& button, const host::ExportCommandAvailability& availability,
                        const QString& tip) {
        button.setEnabled(availability.available);
        button.setToolTip(availability.available ? tip : exportpanel::qs(availability.reason));
    };
    const auto& commands = snapshot->commands;
    int shown = 0;
    int ready = 0;
    int waitingForOk = 0;
    for (const auto& job : snapshot->jobs) {
        if (job.transient)
            continue;
        ++shown;
        ready += isReady(job) ? 1 : 0;
        for (const auto& output : job.outputs)
            if (job.state == host::ExportJobState::Ready && exportpanel::awaitsReplaceConfirmation(output)) {
                ++waitingForOk;
                break;
            }
    }
    set(*startAll_, commands[host::ExportCommand::StartAll], tr("Start every job that is ready"));
    set(*stopAll_, commands[host::ExportCommand::StopAll], tr("Stop every job that is running"));
    set(*removeFinished_, commands[host::ExportCommand::RemoveFinished],
        tr("Remove the jobs that finished"));
    // Start All is the one Primary and only exists while a job can be started.
    // The header row places its controls itself: a control leaves it through "chromeSuppressed".
    const auto show = [](kit::KButton& button, const bool visible) {
        if (button.property("chromeSuppressed").toBool() == visible)
            button.setProperty("chromeSuppressed", !visible);
    };
    show(*startAll_, ready > 0);
    show(*stopAll_, snapshot->activeJob.has_value() || commands[host::ExportCommand::StopAll].available);
    show(*removeFinished_, commands[host::ExportCommand::RemoveFinished].available);
    show(*addComposition_, shown > 0);

    const auto& start = commands[host::ExportCommand::StartAll];
    const bool unsupported = start.code == host::ExportUnavailableCode::PlatformNotSupported;
    platformNote_->setVisible(unsupported);
    platformNote_->setText(unsupported ? exportpanel::qs(start.reason) : QString{});

    // "2 started, 1 waiting for your OK": said after Start All, until nobody waits.
    if (waitingForOk > 0 && startedByStartAll_ >= 0) {
        startNote_->setText(tr("%1 started, %2 waiting for your OK").arg(startedByStartAll_).arg(waitingForOk));
        startNote_->show();
    } else {
        startNote_->hide();
        if (waitingForOk == 0)
            startedByStartAll_ = -1;
    }
}

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

std::size_t ExportEditor::dropIndexAt(const QPoint position) const {
    const auto inList = scroll_->widget()->mapFrom(this, position);
    std::size_t index = 0;
    for (const auto* row : jobRows_)
        if (row->geometry().center().y() < inList.y())
            ++index;
    return index;
}

bool ExportEditor::dropIndicatorShown() const { return !dropLine_->isHidden() || emptyDropShown_; }

int ExportEditor::dropIndicatorY() const { return dropLine_->geometry().y(); }

void ExportEditor::showDropIndicator(const QPoint position) {
    if (jobRows_.empty()) {
        // Nothing to put a line between: the whole page lights up.
        emptyPage_->setFill(kit::Color::SurfaceRaised);
        emptyDropShown_ = true;
        return;
    }
    const auto index = dropIndexAt(position);
    const int y = index < jobRows_.size() ? jobRows_[index]->geometry().top()
                                          : jobRows_.back()->geometry().bottom();
    dropLine_->setGeometry(QRect(0, y, scroll_->widget()->width(), dropLine_->height()));
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
    if (adapter_ != nullptr && !draggedCompositions(*event->mimeData()).empty()) {
        event->acceptProposedAction();
        showDropIndicator(event->position().toPoint());
    } else {
        event->ignore();
    }
}

void ExportEditor::dragMoveEvent(QDragMoveEvent* const event) {
    if (adapter_ != nullptr && !draggedCompositions(*event->mimeData()).empty()) {
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
    const auto compositions = draggedCompositions(*event->mimeData());
    if (compositions.empty()) {
        event->ignore();
        return;
    }
    // One job per composition, at the line the artist saw, as one undo entry.
    (void)adapter_->addCompositionsToQueue(
        compositions, jobRows_.empty() ? std::nullopt : std::optional(dropIndexAt(event->position().toPoint())));
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
