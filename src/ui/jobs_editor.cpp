#include <bloom/ui/jobs_editor.hpp>

#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/task_monitor_model.hpp>

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace bloom::ui {
namespace {

QString text(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString ownerKindText(const runtime::TaskOwnerKind kind) {
    switch (kind) {
    case runtime::TaskOwnerKind::Application:
        return JobsEditor::tr("Application");
    case runtime::TaskOwnerKind::Project:
        return JobsEditor::tr("Project");
    case runtime::TaskOwnerKind::Composition:
        return JobsEditor::tr("Composition");
    case runtime::TaskOwnerKind::PanelRequest:
        return JobsEditor::tr("Panel request");
    case runtime::TaskOwnerKind::Export:
        return JobsEditor::tr("Export");
    }
    return JobsEditor::tr("Unknown owner");
}

QString executorText(const runtime::TaskExecutor executor) {
    switch (executor) {
    case runtime::TaskExecutor::Cpu:
        return JobsEditor::tr("CPU");
    case runtime::TaskExecutor::BlockingIo:
        return JobsEditor::tr("Blocking I/O");
    case runtime::TaskExecutor::Gpu:
        return JobsEditor::tr("GPU service");
    }
    return JobsEditor::tr("Unknown executor");
}

QString severityText(const runtime::DiagnosticSeverity severity) {
    switch (severity) {
    case runtime::DiagnosticSeverity::Information:
        return JobsEditor::tr("Information");
    case runtime::DiagnosticSeverity::Warning:
        return JobsEditor::tr("Warning");
    case runtime::DiagnosticSeverity::Error:
        return JobsEditor::tr("Error");
    }
    return JobsEditor::tr("Unknown severity");
}

QString elapsedText(const std::chrono::steady_clock::time_point start,
                    const std::chrono::steady_clock::time_point finish) {
    const double seconds =
        std::chrono::duration<double>(finish > start ? finish - start
                                                     : std::chrono::steady_clock::duration::zero())
            .count();
    if (seconds < 60.0) {
        return JobsEditor::tr("%1 seconds").arg(seconds, 0, 'f', 1);
    }
    const auto wholeSeconds = static_cast<qulonglong>(seconds);
    return JobsEditor::tr("%1 minutes, %2 seconds").arg(wholeSeconds / 60).arg(wholeSeconds % 60);
}

QString progressDetail(const runtime::TaskProgress& progress) {
    if (!progress.total.has_value()) {
        return JobsEditor::tr("Indeterminate (%1 units reported)").arg(progress.completed);
    }
    if (*progress.total == 0) {
        return JobsEditor::tr("%1 of 0 units").arg(progress.completed);
    }
    const long double ratio =
        static_cast<long double>(progress.completed) / static_cast<long double>(*progress.total);
    const auto percentage =
        static_cast<qulonglong>(std::llround(std::clamp(ratio, 0.0L, 1.0L) * 100));
    return JobsEditor::tr("%1 of %2 units (%3%)")
        .arg(progress.completed)
        .arg(*progress.total)
        .arg(percentage);
}

void addDetail(QStringList& lines, const QString& label, const QString& value) {
    lines.push_back(JobsEditor::tr("%1: %2").arg(label, value));
}

} // namespace

JobsEditor::JobsEditor(TaskMonitorModel& model, QWidget* parent) : QWidget(parent), model_(model) {
    setObjectName("jobsEditor");
    setAccessibleName(tr("Jobs editor"));
    setAccessibleDescription(
        tr("Inspect background work, progress, diagnostics, and cancellation state"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* controls = new QWidget(this);
    controls->setObjectName("jobsControls");
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(8, 5, 8, 5);
    controlsLayout->setSpacing(6);
    auto* title = new QLabel(tr("Jobs"), controls);
    title->setObjectName("editorSectionTitle");
    cancelButton_ = new QPushButton(tr("Cancel"), controls);
    cancelButton_->setObjectName("cancelSelectedJobButton");
    cancelButton_->setAccessibleName(tr("Cancel selected job"));
    cancelButton_->setAccessibleDescription(
        tr("Requests cooperative cancellation for the selected queued or running job"));
    cancelButton_->setToolTip(tr("Cancel the selected job"));
    cancelButton_->setEnabled(false);
    controlsLayout->addWidget(title);
    controlsLayout->addStretch(1);
    controlsLayout->addWidget(cancelButton_);

    table_ = new QTableView(this);
    table_->setObjectName("jobsTable");
    table_->setAccessibleName(tr("Background jobs"));
    table_->setAccessibleDescription(
        tr("Jobs ordered newest first with phase, progress, priority, state, and duration"));
    table_->setModel(&model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setSortingEnabled(false);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(TaskMonitorModel::JobColumn,
                                                     QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(TaskMonitorModel::PhaseColumn,
                                                     QHeaderView::Stretch);
    for (int column = TaskMonitorModel::ProgressColumn; column < TaskMonitorModel::ColumnCount;
         ++column) {
        table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }

    details_ = new QPlainTextEdit(this);
    details_->setObjectName("jobDetails");
    details_->setAccessibleName(tr("Selected job details and diagnostics"));
    details_->setAccessibleDescription(
        tr("Read-only structured details and complete diagnostics for the selected job"));
    details_->setReadOnly(true);
    details_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    details_->setMaximumBlockCount(4096);
    details_->setMinimumHeight(140);
    details_->setPlainText(tr("No jobs have been submitted."));

    layout->addWidget(controls);
    layout->addWidget(table_, 2);
    layout->addWidget(details_, 1);

    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &JobsEditor::updateDetails);
    connect(cancelButton_, &QPushButton::clicked, this, &JobsEditor::cancelSelectedTask);
    connect(&model_, &QAbstractItemModel::modelAboutToBeReset, this, &JobsEditor::captureSelection);
    connect(&model_, &QAbstractItemModel::modelReset, this, &JobsEditor::restoreSelection);
    connect(&model_, &QAbstractItemModel::dataChanged, this, &JobsEditor::updateDetails);

    restoreSelection();
}

void JobsEditor::captureSelection() {
    const auto taskId = model_.taskIdForIndex(selectedIndex());
    selectedTaskId_ = taskId.value_or(runtime::TaskId{});
}

void JobsEditor::restoreSelection() {
    QModelIndex target = model_.indexForTask(selectedTaskId_);
    if (!target.isValid() && model_.rowCount() > 0) {
        target = model_.index(0, TaskMonitorModel::JobColumn);
    }
    if (target.isValid()) {
        table_->setCurrentIndex(target);
        table_->selectRow(target.row());
    } else {
        table_->clearSelection();
    }
    updateDetails();
}

void JobsEditor::updateDetails() {
    const QModelIndex current = selectedIndex();
    const auto* snapshot = model_.snapshotForIndex(current);
    if (snapshot == nullptr) {
        selectedTaskId_ = {};
        cancelButton_->setEnabled(false);
        details_->setPlainText(tr("No jobs have been submitted."));
        return;
    }

    selectedTaskId_ = snapshot->id;
    cancelButton_->setEnabled(model_.data(current, TaskMonitorModel::CanCancelRole).toBool());

    const QModelIndex stateIndex = model_.index(current.row(), TaskMonitorModel::StateColumn);
    const QModelIndex priorityIndex = model_.index(current.row(), TaskMonitorModel::PriorityColumn);
    QStringList lines;
    addDetail(lines, tr("Task ID"), QString::number(snapshot->id.value()));
    addDetail(lines, tr("Job"), text(snapshot->name));
    addDetail(lines, tr("Owner"),
              tr("%1 %2").arg(ownerKindText(snapshot->owner.kind)).arg(snapshot->owner.id.value()));
    addDetail(lines, tr("Executor"), executorText(snapshot->executor));
    addDetail(lines, tr("Priority"), model_.data(priorityIndex).toString());
    addDetail(lines, tr("State"), model_.data(stateIndex).toString());
    addDetail(lines, tr("Phase"),
              snapshot->progress.phase.empty() ? tr("Waiting") : text(snapshot->progress.phase));
    if (!snapshot->progress.subphase.empty()) {
        addDetail(lines, tr("Subphase"), text(snapshot->progress.subphase));
    }
    addDetail(lines, tr("Progress"), progressDetail(snapshot->progress));
    addDetail(lines, tr("Cancellation requested"),
              snapshot->cancellationRequested ? tr("Yes") : tr("No"));

    const auto now = std::chrono::steady_clock::now();
    const auto effectiveFinish = snapshot->finishedAt.value_or(now);
    addDetail(lines, tr("Total elapsed"), elapsedText(snapshot->queuedAt, effectiveFinish));
    if (snapshot->startedAt.has_value()) {
        addDetail(lines, tr("Run elapsed"), elapsedText(*snapshot->startedAt, effectiveFinish));
    } else {
        addDetail(lines, tr("Queue elapsed"), elapsedText(snapshot->queuedAt, effectiveFinish));
    }
    if (snapshot->sourceVersion.documentRevision.has_value()) {
        addDetail(lines, tr("Source revision"),
                  QString::number(*snapshot->sourceVersion.documentRevision));
    }
    if (snapshot->sourceVersion.requestGeneration.has_value()) {
        addDetail(lines, tr("Request generation"),
                  QString::number(*snapshot->sourceVersion.requestGeneration));
    }
    if (snapshot->groupId.has_value()) {
        addDetail(lines, tr("Task group"), QString::number(snapshot->groupId->value()));
    }

    lines.push_back({});
    lines.push_back(tr("Diagnostics (%1)").arg(snapshot->diagnostics.size()));
    if (snapshot->diagnostics.empty()) {
        lines.push_back(tr("None"));
    }
    for (const auto& diagnostic : snapshot->diagnostics) {
        lines.push_back({});
        lines.push_back(
            tr("[%1] %2").arg(severityText(diagnostic.severity), text(diagnostic.code)));
        addDetail(lines, tr("Summary"), text(diagnostic.summary));
        if (!diagnostic.detail.empty()) {
            addDetail(lines, tr("Detail"), text(diagnostic.detail));
        }
        if (!diagnostic.suggestedAction.empty()) {
            addDetail(lines, tr("Suggested action"), text(diagnostic.suggestedAction));
        }
    }
    details_->setPlainText(lines.join(QLatin1Char('\n')));
}

void JobsEditor::cancelSelectedTask() {
    const QModelIndex current = selectedIndex();
    if (model_.requestCancellation(current)) {
        cancelButton_->setEnabled(false);
        updateDetails();
    }
}

QModelIndex JobsEditor::selectedIndex() const {
    const QModelIndex current = table_->currentIndex();
    return current.isValid() ? current.siblingAtColumn(TaskMonitorModel::JobColumn) : QModelIndex{};
}

bool registerJobsEditor(EditorRegistry& registry, TaskMonitorModel& model) {
    return registry.registerEditor(
        {.id = "bloom.jobs",
         .displayName = JobsEditor::tr("Jobs"),
         .create = [&model](QWidget* parent) { return new JobsEditor(model, parent); }});
}

} // namespace bloom::ui
