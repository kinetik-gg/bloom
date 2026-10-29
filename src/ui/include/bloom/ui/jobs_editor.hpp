#pragma once

#include <bloom/runtime/task_types.hpp>
#include <bloom/ui/editor_area.hpp>

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

class JobsEditor final : public QWidget, public EditorChromeProvider {
    Q_OBJECT

  public:
    explicit JobsEditor(TaskMonitorModel& model, QWidget* parent = nullptr);
    // Cancel is a header control, laid out and sized by the panel like every other header's.
    [[nodiscard]] EditorChromeSpec& editorChrome() override { return chrome_; }

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
    EditorChromeSpec chrome_;
    runtime::TaskId selectedTaskId_;
};

[[nodiscard]] bool registerJobsEditor(EditorRegistry& registry, TaskMonitorModel& model);

} // namespace bloom::ui
