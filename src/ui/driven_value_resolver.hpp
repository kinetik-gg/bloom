#pragma once
#include <QObject>
#include <QString>
#include <atomic>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <functional>
#include <map>
class QTimer;
namespace bloom::ui {
class CompositionSession;
// One cancellable background value-only evaluation, with one newest pending request.
// Owned by CompositionSession (task DRIVE-1), so ONE evaluation answers every surface that shows
// a driven parameter -- the Properties row and the timeline row for one parameter cannot show
// different strings, because there is one string.
class DrivenValueResolver final : public QObject {
  public:
    using Values = std::map<document::ParameterId, QString>;
    explicit DrivenValueResolver(CompositionSession& session, QObject* parent);
    ~DrivenValueResolver() override;
    void request(std::vector<document::ParameterId> parameters);
    std::function<void(const Values&)> ready;

  private:
    void poll();
    void start();
    CompositionSession& session_;
    std::shared_ptr<runtime::TaskScheduler> scheduler_;
    std::shared_ptr<std::atomic_bool> retire_;
    runtime::TaskHandle<std::shared_ptr<Values>> task_;
    QTimer* timer_;
    std::vector<document::ParameterId> parameters_;
    std::uint64_t generation_ = 0;
    std::uint64_t activeGeneration_ = 0;
    bool active_ = false;
};
} // namespace bloom::ui
