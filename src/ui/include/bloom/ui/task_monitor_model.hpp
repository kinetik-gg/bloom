#pragma once

#include <bloom/runtime/task_types.hpp>

#include <QAbstractTableModel>
#include <QByteArray>
#include <QHash>
#include <QModelIndex>
#include <QVariant>

#include <optional>
#include <vector>

namespace bloom::ui {

class TaskUiBridge;

class TaskMonitorModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    enum Column {
        JobColumn,
        PhaseColumn,
        ProgressColumn,
        PriorityColumn,
        StateColumn,
        DurationColumn,
        ColumnCount,
    };

    enum Role {
        TaskIdRole = Qt::UserRole + 1,
        TaskStateRole,
        ProgressCompletedRole,
        ProgressTotalRole,
        CancellationRequestedRole,
        DiagnosticCountRole,
        CanCancelRole,
    };

    explicit TaskMonitorModel(TaskUiBridge& bridge, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] const runtime::TaskSnapshot*
    snapshotForIndex(const QModelIndex& index) const noexcept;
    [[nodiscard]] std::optional<runtime::TaskId>
    taskIdForIndex(const QModelIndex& index) const noexcept;
    [[nodiscard]] QModelIndex indexForTask(runtime::TaskId taskId, int column = JobColumn) const;
    [[nodiscard]] bool requestCancellation(const QModelIndex& index) noexcept;

  private slots:
    void refreshFromBridge();

  private:
    TaskUiBridge& bridge_;
    std::vector<runtime::TaskSnapshot> snapshots_;
};

} // namespace bloom::ui
