#pragma once
#include <QObject>
#include <QString>
#include <atomic>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>
class QTimer;
namespace bloom::ui {
class CompositionSession;
// One cancellable background value-only evaluation, with one newest pending request.
// Owned by CompositionSession (task DRIVE-1), so ONE evaluation answers every surface that shows
// a driven parameter -- the Properties row and the timeline row for one parameter cannot show
// different strings, because there is one string.
class DrivenValueResolver final : public QObject {
  public:
    using ValueOutputKey = std::pair<document::NodeId, std::string>;
    struct Values final {
        std::map<document::ParameterId, QString> parameters;
        std::map<ValueOutputKey, QString> outputs;
    };
    // #358 work counters, shared with the worker task.
    struct Work final {
        std::atomic<std::uint64_t> probeCompiles{0};
        std::atomic<std::uint64_t> evaluations{0};
    };
    explicit DrivenValueResolver(CompositionSession& session, QObject* parent);
    [[nodiscard]] const Work& work() const noexcept { return *work_; }
    ~DrivenValueResolver() override;
    void request(std::vector<document::ParameterId> parameters);
    std::function<void(const Values&)> ready;

  private:
    void poll();
    void start();
    CompositionSession& session_;
    std::shared_ptr<runtime::TaskScheduler> scheduler_;
    std::shared_ptr<std::atomic_bool> retire_;
    // #358: the driver probe compiles once per (document state, composition, parameter set);
    // every later time only evaluates the cached plan's value graph.
    struct ProbeCache;
    std::shared_ptr<ProbeCache> probeCache_;
    std::shared_ptr<Work> work_ = std::make_shared<Work>();
    runtime::TaskHandle<std::shared_ptr<Values>> task_;
    QTimer* timer_;
    std::vector<document::ParameterId> parameters_;
    std::uint64_t generation_ = 0;
    std::uint64_t activeGeneration_ = 0;
    bool active_ = false;
};
} // namespace bloom::ui
