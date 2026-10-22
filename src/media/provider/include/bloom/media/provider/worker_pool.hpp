#pragma once
#include <atomic>
#include <bloom/media/provider/protocol.hpp>
#include <bloom/platform/process_supervisor.hpp>
#include <bloom/runtime/task_scheduler.hpp>

namespace bloom::media::provider {
using WorkerReply = std::variant<ProbeResult, FrameProduct, DemuxIndex, AudioBlock, Unavailable>;
struct WorkerPoolOptions {
    platform::ProcessOptions process;
    Handshake expected;
    std::chrono::milliseconds timeout{5000};
    std::size_t capacity = 2;
    // FFmpeg video sessions may retain one process for a pinned immutable source generation.
    // Other providers keep the existing one-call worker lifecycle.
    bool retainedVideo = false;
    // Optional process-lifecycle telemetry, invoked on the BlockingIo task.
    std::function<void(std::int64_t)> launched;
};
class WorkerTicket final {
  public:
    runtime::TaskSubmission<void> submission;
    [[nodiscard]] std::shared_ptr<const WorkerReply> result() const;

  private:
    friend class MediaWorkerPool;
    struct State;
    std::shared_ptr<State> state_;
};
// Host-only facade. This target depends on the scheduler, while bloom_media_provider does not.
// Video sessions can retain one bounded worker for an explicitly retained source revision.
class MediaWorkerPool final {
  public:
    explicit MediaWorkerPool(WorkerPoolOptions options);
    ~MediaWorkerPool();
    MediaWorkerPool(const MediaWorkerPool&) = delete;
    MediaWorkerPool& operator=(const MediaWorkerPool&) = delete;
    [[nodiscard]] WorkerTicket
    submit(runtime::TaskScheduler& scheduler, runtime::TaskOwner owner, const MediaAttempt& attempt,
           std::size_t step, CallRequest request,
           std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot = {});
    // Synchronous execution on the caller's existing BlockingIo task. This does not create a
    // private scheduler or poll another task; cancellation is propagated to the supervisor.
    [[nodiscard]] WorkerReply
    call(const MediaAttempt& attempt, std::size_t step, CallRequest request,
         const std::shared_ptr<const platform::ReadOnlyFileSnapshot>& snapshot,
         const platform::ProcessCancellation& cancel = {}, bool retainWorker = false);
    // Drop a pinned worker before capturing a fresh source revision. The caller runs off UI.
    void releaseRetained();
    void shutdown();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace bloom::media::provider
