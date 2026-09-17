#pragma once

#include <QStringList>
#include <QWidget>
#include <bloom/ui/editor_area.hpp>
#include <memory>

namespace bloom::ui {
namespace kit {
class KLineEdit;
class KLabel;
class KDropdown;
} // namespace kit
class ScriptPanelRuntime;

class ScriptPanel final : public QWidget, public EditorChromeProvider {
    Q_OBJECT
  public:
    explicit ScriptPanel(std::shared_ptr<ScriptPanelRuntime> runtime, QWidget* parent = nullptr);
    [[nodiscard]] EditorChromeSpec& editorChrome() override { return chrome_; }

  signals:
    void executionFinished(bool succeeded);

  private:
    void run();
    void append(const QString& source, const QString& output, bool succeeded);
    std::shared_ptr<ScriptPanelRuntime> runtime_;
    EditorChromeSpec chrome_;
    kit::KLineEdit* input_ = nullptr;
    kit::KLabel* output_ = nullptr;
    kit::KLabel* status_ = nullptr;
    kit::KDropdown* history_ = nullptr;
    QString transcript_;
    QStringList sources_;
};
} // namespace bloom::ui
