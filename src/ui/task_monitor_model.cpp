#include <bloom/ui/task_monitor_model.hpp>

#include <bloom/ui/task_ui_bridge.hpp>

#include <QThread>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ranges>
#include <string>
#include <utility>

namespace bloom::ui {
namespace {

QString text(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString priorityText(const runtime::TaskPriority priority) {
    switch (priority) {
    case runtime::TaskPriority::Interactive:
        return TaskMonitorModel::tr("Interactive");
    case runtime::TaskPriority::Visible:
        return TaskMonitorModel::tr("Visible");
    case runtime::TaskPriority::Foreground:
        return TaskMonitorModel::tr("Foreground");
    case runtime::TaskPriority::Background:
        return TaskMonitorModel::tr("Background");
    }
    return TaskMonitorModel::tr("Unknown priority");
}

QString stateText(const runtime::TaskSnapshot& snapshot) {
    if (!runtime::isTerminal(snapshot.state) && snapshot.cancellationRequested) {
        return TaskMonitorModel::tr("Cancelling…");
    }
    switch (snapshot.state) {
    case runtime::TaskState::Queued:
        return TaskMonitorModel::tr("Queued");
    case runtime::TaskState::Running:
        return TaskMonitorModel::tr("Running");
    case runtime::TaskState::Succeeded:
        return TaskMonitorModel::tr("Succeeded");
    case runtime::TaskState::Cancelled:
        return TaskMonitorModel::tr("Cancelled");
    case runtime::TaskState::Failed:
        return TaskMonitorModel::tr("Failed");
    }
    return TaskMonitorModel::tr("Unknown state");
}

QString progressText(const runtime::TaskProgress& progress) {
    if (!progress.total.has_value()) {
        return TaskMonitorModel::tr("Indeterminate");
    }
    if (*progress.total == 0) {
        return TaskMonitorModel::tr("%1 / 0").arg(progress.completed);
    }
    const long double ratio =
        static_cast<long double>(progress.completed) / static_cast<long double>(*progress.total);
    const auto percent = static_cast<qulonglong>(std::llround(std::clamp(ratio, 0.0L, 1.0L) * 100));
    return TaskMonitorModel::tr("%1% · %2 / %3")
        .arg(percent)
        .arg(progress.completed)
        .arg(*progress.total);
}

std::chrono::steady_clock::duration elapsedDuration(const runtime::TaskSnapshot& snapshot) {
    const auto start = snapshot.startedAt.value_or(snapshot.queuedAt);
    const auto finish = snapshot.finishedAt.value_or(std::chrono::steady_clock::now());
    return finish > start ? finish - start : std::chrono::steady_clock::duration::zero();
}

QString durationText(const runtime::TaskSnapshot& snapshot) {
    const double seconds = std::chrono::duration<double>(elapsedDuration(snapshot)).count();
    if (seconds < 10.0) {
        return TaskMonitorModel::tr("%1 s").arg(seconds, 0, 'f', 1);
    }
    if (seconds < 60.0) {
        return TaskMonitorModel::tr("%1 s").arg(std::floor(seconds), 0, 'f', 0);
    }

    const auto wholeSeconds = static_cast<qulonglong>(seconds);
    const auto minutes = wholeSeconds / 60;
    const auto remainingSeconds = wholeSeconds % 60;
    if (minutes < 60) {
        return TaskMonitorModel::tr("%1:%2").arg(minutes).arg(remainingSeconds, 2, 10,
                                                              QLatin1Char('0'));
    }
    const auto hours = minutes / 60;
    const auto remainingMinutes = minutes % 60;
    return TaskMonitorModel::tr("%1:%2:%3")
        .arg(hours)
        .arg(remainingMinutes, 2, 10, QLatin1Char('0'))
        .arg(remainingSeconds, 2, 10, QLatin1Char('0'));
}

bool canCancel(const runtime::TaskSnapshot& snapshot) noexcept {
    return !runtime::isTerminal(snapshot.state) && !snapshot.cancellationRequested;
}

bool equalSnapshot(const runtime::TaskSnapshot& lhs, const runtime::TaskSnapshot& rhs) {
    return lhs.id == rhs.id && lhs.name == rhs.name && lhs.owner == rhs.owner &&
           lhs.priority == rhs.priority && lhs.executor == rhs.executor && lhs.state == rhs.state &&
           lhs.groupId == rhs.groupId && lhs.sourceVersion == rhs.sourceVersion &&
           lhs.progress == rhs.progress && lhs.diagnostics == rhs.diagnostics &&
           lhs.cancellationRequested == rhs.cancellationRequested && lhs.queuedAt == rhs.queuedAt &&
           lhs.startedAt == rhs.startedAt && lhs.finishedAt == rhs.finishedAt;
}

bool equalIdentities(const std::vector<runtime::TaskSnapshot>& lhs,
                     const std::vector<runtime::TaskSnapshot>& rhs) {
    return lhs.size() == rhs.size() &&
           std::ranges::equal(lhs, rhs, {}, &runtime::TaskSnapshot::id, &runtime::TaskSnapshot::id);
}

} // namespace

TaskMonitorModel::TaskMonitorModel(TaskUiBridge& bridge, QObject* parent)
    : QAbstractTableModel(parent), bridge_(bridge) {
    Q_ASSERT(QThread::currentThread() == thread());
    connect(&bridge_, &TaskUiBridge::snapshotsPolled, this, &TaskMonitorModel::refreshFromBridge);
    refreshFromBridge();
}

int TaskMonitorModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(snapshots_.size());
}

int TaskMonitorModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant TaskMonitorModel::data(const QModelIndex& index, const int role) const {
    const auto* snapshot = snapshotForIndex(index);
    if (snapshot == nullptr) {
        return {};
    }

    if (role == TaskIdRole) {
        return QVariant::fromValue<qulonglong>(snapshot->id.value());
    }
    if (role == TaskStateRole) {
        return static_cast<int>(snapshot->state);
    }
    if (role == ProgressCompletedRole) {
        return QVariant::fromValue<qulonglong>(snapshot->progress.completed);
    }
    if (role == ProgressTotalRole) {
        return snapshot->progress.total.has_value()
                   ? QVariant::fromValue<qulonglong>(*snapshot->progress.total)
                   : QVariant{};
    }
    if (role == CancellationRequestedRole) {
        return snapshot->cancellationRequested;
    }
    if (role == DiagnosticCountRole) {
        return static_cast<qulonglong>(snapshot->diagnostics.size());
    }
    if (role == CanCancelRole) {
        return canCancel(*snapshot);
    }
    if (role == Qt::TextAlignmentRole) {
        const bool rightAligned =
            index.column() == ProgressColumn || index.column() == DurationColumn;
        return static_cast<int>(rightAligned ? Qt::AlignRight | Qt::AlignVCenter
                                             : Qt::AlignLeft | Qt::AlignVCenter);
    }

    QString display;
    switch (index.column()) {
    case JobColumn:
        display = text(snapshot->name);
        break;
    case PhaseColumn:
        display = snapshot->progress.phase.empty() ? tr("Waiting") : text(snapshot->progress.phase);
        break;
    case ProgressColumn:
        display = progressText(snapshot->progress);
        break;
    case PriorityColumn:
        display = priorityText(snapshot->priority);
        break;
    case StateColumn:
        display = stateText(*snapshot);
        break;
    case DurationColumn:
        display = durationText(*snapshot);
        break;
    default:
        return {};
    }

    if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) {
        return display;
    }
    if (role == Qt::ToolTipRole) {
        return tr("Task %1 — %2").arg(snapshot->id.value()).arg(stateText(*snapshot));
    }
    if (role == Qt::AccessibleDescriptionRole) {
        return tr("Task %1, %2, %3")
            .arg(snapshot->id.value())
            .arg(stateText(*snapshot), progressText(snapshot->progress));
    }
    return {};
}

QVariant TaskMonitorModel::headerData(const int section, const Qt::Orientation orientation,
                                      const int role) const {
    if (orientation != Qt::Horizontal ||
        (role != Qt::DisplayRole && role != Qt::AccessibleTextRole)) {
        return {};
    }
    switch (section) {
    case JobColumn:
        return tr("Job");
    case PhaseColumn:
        return tr("Phase");
    case ProgressColumn:
        return tr("Progress");
    case PriorityColumn:
        return tr("Priority");
    case StateColumn:
        return tr("State");
    case DurationColumn:
        return tr("Duration");
    default:
        return {};
    }
}

QHash<int, QByteArray> TaskMonitorModel::roleNames() const {
    auto roles = QAbstractTableModel::roleNames();
    roles.insert(TaskIdRole, "taskId");
    roles.insert(TaskStateRole, "taskState");
    roles.insert(ProgressCompletedRole, "progressCompleted");
    roles.insert(ProgressTotalRole, "progressTotal");
    roles.insert(CancellationRequestedRole, "cancellationRequested");
    roles.insert(DiagnosticCountRole, "diagnosticCount");
    roles.insert(CanCancelRole, "canCancel");
    return roles;
}

const runtime::TaskSnapshot*
TaskMonitorModel::snapshotForIndex(const QModelIndex& index) const noexcept {
    if (!index.isValid() || index.model() != this || index.row() < 0 ||
        index.row() >= static_cast<int>(snapshots_.size())) {
        return nullptr;
    }
    return &snapshots_[static_cast<std::size_t>(index.row())];
}

std::optional<runtime::TaskId>
TaskMonitorModel::taskIdForIndex(const QModelIndex& index) const noexcept {
    const auto* snapshot = snapshotForIndex(index);
    return snapshot == nullptr ? std::nullopt : std::optional{snapshot->id};
}

QModelIndex TaskMonitorModel::indexForTask(const runtime::TaskId taskId, const int column) const {
    if (!taskId.isValid() || column < 0 || column >= ColumnCount) {
        return {};
    }
    const auto found = std::ranges::find(snapshots_, taskId, &runtime::TaskSnapshot::id);
    if (found == snapshots_.end()) {
        return {};
    }
    return index(static_cast<int>(std::distance(snapshots_.begin(), found)), column);
}

bool TaskMonitorModel::requestCancellation(const QModelIndex& indexValue) noexcept {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* snapshot = snapshotForIndex(indexValue);
    if (snapshot == nullptr || !canCancel(*snapshot)) {
        return false;
    }
    const runtime::TaskId taskId = snapshot->id;
    if (!bridge_.requestCancellation(taskId)) {
        return false;
    }

    const auto row = indexValue.row();
    snapshots_[static_cast<std::size_t>(row)].cancellationRequested = true;
    emit dataChanged(index(row, JobColumn), index(row, DurationColumn));
    return true;
}

void TaskMonitorModel::refreshFromBridge() {
    Q_ASSERT(QThread::currentThread() == thread());
    auto next = bridge_.snapshots();
    std::ranges::sort(next, [](const auto& lhs, const auto& rhs) { return rhs.id < lhs.id; });

    if (!equalIdentities(snapshots_, next)) {
        beginResetModel();
        snapshots_ = std::move(next);
        endResetModel();
        return;
    }

    for (std::size_t position = 0; position < next.size(); ++position) {
        const bool changed = !equalSnapshot(snapshots_[position], next[position]);
        const bool durationChanges = !runtime::isTerminal(next[position].state);
        snapshots_[position] = std::move(next[position]);
        const auto row = static_cast<int>(position);
        if (changed) {
            emit dataChanged(index(row, JobColumn), index(row, DurationColumn));
        } else if (durationChanges) {
            emit dataChanged(index(row, DurationColumn), index(row, DurationColumn),
                             {Qt::DisplayRole, Qt::AccessibleTextRole});
        }
    }
}

} // namespace bloom::ui
