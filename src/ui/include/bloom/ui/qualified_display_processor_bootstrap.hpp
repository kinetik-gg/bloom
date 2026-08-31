#pragma once

#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>

// Owns the Qt-side scheduling/polling glue for the one-time blocking-stage handoff (issue #97,
// task C3, design decision 3). Qt types stay confined to src/ui (AGENTS.md); the pure build step
// itself lives in bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor()
// (qualified_display_processor_provider.hpp), which this class only submits and polls.
namespace bloom::ui {

class TaskUiBridge;

class QualifiedDisplayProcessorBootstrap final : public QObject {
    Q_OBJECT

  public:
    // Submits the build immediately, at construction -- design decision 3: "at pipeline/session
    // construction, resolve Bloom Neutral through the C2 registry and build the processor". Both
    // `scheduler` and `taskUiBridge` must outlive this object; `provider` is published into exactly
    // once, from this object's poll handler (TaskUiBridge::snapshotsPolled), never from the
    // submitted worker itself (the worker runs on a blocking-I/O executor thread, not the
    // authoring/UI thread).
    QualifiedDisplayProcessorBootstrap(runtime::TaskScheduler& scheduler,
                                       TaskUiBridge& taskUiBridge,
                                       runtime::QualifiedDisplayProcessorProvider& provider,
                                       QObject* parent = nullptr);

  private:
    void pollOnce();

    runtime::TaskScheduler& scheduler_;
    runtime::QualifiedDisplayProcessorProvider& provider_;
    runtime::TaskHandle<std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle>> handle_;
    bool submitted_ = false;
    bool completed_ = false;
};

} // namespace bloom::ui
