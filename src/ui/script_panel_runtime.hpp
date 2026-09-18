#pragma once

#include <QObject>
#include <QString>
#include <memory>
#include <thread>

namespace bloom::ui {
class CompositionSession;
class ProjectHost;

// One console per editor registry. Python and native render work never run in a Qt callback.
class ScriptPanelRuntime final : public QObject {
    Q_OBJECT
  public:
    ScriptPanelRuntime(CompositionSession& session, ProjectHost& host);
    ~ScriptPanelRuntime() override;
    [[nodiscard]] bool submit(const QString& source);
    void cancel();
    void shutdown();
    [[nodiscard]] bool busy() const;

  signals:
    void outputReady(QString source, QString output, bool succeeded);
    void activityChanged(QString message, bool busy);

  private:
    struct State;
    void refresh();
    void replaceSession();
    void poll();
    static void work(const std::shared_ptr<State>& state);
    CompositionSession& session_;
    ProjectHost& host_;
    std::shared_ptr<State> state_;
    std::jthread worker_;
};
} // namespace bloom::ui
