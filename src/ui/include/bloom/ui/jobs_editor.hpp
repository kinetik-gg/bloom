#pragma once

#include <bloom/runtime/task_types.hpp>

#include <QWidget>

class QModelIndex;
class QPlainTextEdit;
namespace bloom::ui::kit {
class KButton;
}
class QTableView;

namespace bloom::ui {

class EditorRegistry;
class TaskMonitorModel;

class JobsEditor final : public QWidget {
    Q_OBJECT

  public:
    explicit JobsEditor(TaskMonitorModel& model, QWidget* parent = nullptr);

  private slots:
    void captureSelection();
    void restoreSelection();
    void updateDetails();
    void cancelSelectedTask();

  private:
    [[nodiscard]] QModelIndex selectedIndex() const;

    TaskMonitorModel& model_;
    QTableView* table_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    kit::KButton* cancelButton_ = nullptr;
    runtime::TaskId selectedTaskId_;
};

[[nodiscard]] bool registerJobsEditor(EditorRegistry& registry, TaskMonitorModel& model);

} // namespace bloom::ui
