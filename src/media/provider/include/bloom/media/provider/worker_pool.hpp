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
// Each bounded slot owns one fresh process per call; no provider or attempt reuse in v1.
class MediaWorkerPool final {
  public:
    explicit MediaWorkerPool(WorkerPoolOptions options);
    ~MediaWorkerPool();
    MediaWorkerPool(const MediaWorkerPool&) = delete;
    MediaWorkerPool& operator=(const MediaWorkerPool&) = delete;
    [[nodiscard]] WorkerTicket submit(runtime::TaskScheduler& scheduler, runtime::TaskOwner owner,
                                      const MediaAttempt& attempt, std::size_t step,
                                      CallRequest request);
    void shutdown();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace bloom::media::provider
