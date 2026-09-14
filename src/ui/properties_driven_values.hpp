#pragma once
#include <QObject>
#include <QString>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <functional>
#include <map>
class QTimer;
namespace bloom::ui {
class CompositionSession;
// One cancellable background value-only evaluation, with one newest pending request.
class PropertiesDrivenValues final : public QObject {
  public:
    using Values = std::map<document::ParameterId, QString>;
    explicit PropertiesDrivenValues(CompositionSession& session, QObject* parent);
    ~PropertiesDrivenValues() override;
    void request(std::vector<document::ParameterId> parameters);
    std::function<void(const Values&)> ready;

  private:
    void poll();
    void start();
    CompositionSession& session_;
    std::unique_ptr<runtime::TaskScheduler> scheduler_;
    runtime::TaskHandle<std::shared_ptr<Values>> task_;
    QTimer* timer_;
    std::vector<document::ParameterId> parameters_;
    std::uint64_t generation_ = 0;
    std::uint64_t activeGeneration_ = 0;
    bool active_ = false;
};
} // namespace bloom::ui
