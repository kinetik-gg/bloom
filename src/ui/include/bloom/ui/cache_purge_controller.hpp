#pragma once

#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>
#include <QString>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace bloom::media::cache {
class MediaDiskCache;
} // namespace bloom::media::cache

namespace bloom::runtime {
class GpuPreparedUploadCache;
class GpuSceneCoverageCache;
class OperationCache;
} // namespace bloom::runtime

namespace bloom::ui {

class TaskUiBridge;

// Owns the application's two derived-cache purge commands (Edit | Purge…,
// docs/user-guide/memory.md "Purging caches"). Both commands free rebuildable runtime data and
// never touch project truth, revision, dirty state, or undo.
//
// The purge is asynchronous and off the UI thread. It gates the preview/background/RAM producers
// for the duration, so no request for the generation being purged can be submitted; then it waits
// for the scheduler to retire the work already in flight before it clears, so a value that was
// already being computed cannot land in a cache after the clear. If work does not retire within a
// bounded wait (for example a long export), the purge fails safely without clearing and releases
// the gate, rather than leaving the commands disabled indefinitely.
//
// The heavy step -- MediaDiskCache::clear(), which flushes the pending writer and removes every
// entry from disk -- runs on the existing runtime::TaskScheduler's BlockingIo lane, never on the
// UI thread. The GPU resident scene cache is cleared through the owning service's own owner-thread
// API. Clearing a cache drops only that cache's own references; a frame already handed to the
// viewer, an export, or a task stays valid until its owner releases it, and no lease is globally
// invalidated.
class CachePurgeController final : public QObject {
    Q_OBJECT

  public:
    // Every pointer may be null (that store is simply not cleared for this session).
    // `clearDecodedVideoMemory` runs on the purge worker; the composition root binds it to the
    // evaluator's decoded-video cache. `setProducerGate` runs on the UI thread and is bound to the
    // preview controller's purge gate. `purgeGpuRetainedCaches` runs on the purge worker, is passed
    // a timeout, and is bound to the preview service's owner-thread resident-cache clear.
    // `bridge` is borrowed and must outlive this controller; it owns the poll that observes the
    // purge task, and `scheduler` must be the one it wraps.
    CachePurgeController(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                         media::cache::MediaDiskCache* diskCache,
                         runtime::OperationCache* operationCache,
                         runtime::GpuPreparedUploadCache* preparedUploadCache,
                         runtime::GpuSceneCoverageCache* coverageCache,
                         std::function<void()> clearDecodedVideoMemory = {},
                         std::function<void(bool)> setProducerGate = {},
                         std::function<bool(std::chrono::milliseconds)> purgeGpuRetainedCaches = {},
                         QObject* parent = nullptr);
    ~CachePurgeController() override;

    [[nodiscard]] bool isPurging() const noexcept { return mode_ != Mode::None; }

    // Start one purge. Returns false while another is already running; the caller then reports the
    // in-progress state rather than stacking a second purge.
    bool requestPreviewPurge();
    bool requestMediaPurge();

  public slots:
    // Drains the in-flight purge task and, once the scheduler is quiescent, submits the next one.
    // Wired to TaskUiBridge::snapshotsPolled and also callable directly by a test.
    void poll();

  signals:
    void purgeStarted();
    void purgeFinished(bool success, const QString& message);

  private:
    enum class Mode : std::uint8_t {
        None,
        Preview,
        Media,
    };

    bool start(Mode mode);
    void submitPurgeTask(Mode mode);
    void finish(bool success, const QString& message);

    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    media::cache::MediaDiskCache* diskCache_ = nullptr;
    runtime::OperationCache* operationCache_ = nullptr;
    runtime::GpuPreparedUploadCache* preparedUploadCache_ = nullptr;
    runtime::GpuSceneCoverageCache* coverageCache_ = nullptr;
    std::function<void()> clearDecodedVideoMemory_;
    std::function<void(bool)> setProducerGate_;
    std::function<bool(std::chrono::milliseconds)> purgeGpuRetainedCaches_;
    std::optional<runtime::TaskHandle<std::uint64_t>> task_;
    std::chrono::steady_clock::time_point waitStartedAt_{};
    bool waitingForQuiescence_ = false;
    Mode mode_ = Mode::None;
};

} // namespace bloom::ui
