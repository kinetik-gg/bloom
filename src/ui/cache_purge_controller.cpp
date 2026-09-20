#include <bloom/ui/cache_purge_controller.hpp>

#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <bloom/ui/media_disk_cache_settings.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <utility>

namespace bloom::ui {
namespace {

// Bounded wait for in-flight work to retire before the clear. A long export must not leave the
// purge commands disabled indefinitely; on expiry the purge fails safely without clearing.
constexpr auto kPurgeQuiescenceTimeout = std::chrono::seconds(5);
// Bound on the GPU owner-thread clear so a stalled service cannot wedge the purge worker.
constexpr auto kGpuPurgeTimeout = std::chrono::seconds(2);

} // namespace

CachePurgeController::CachePurgeController(
    runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
    media::cache::MediaDiskCache* const diskCache, runtime::OperationCache* const operationCache,
    runtime::GpuPreparedUploadCache* const preparedUploadCache,
    runtime::GpuSceneCoverageCache* const coverageCache,
    std::function<void()> clearDecodedVideoMemory, std::function<void(bool)> setProducerGate,
    std::function<bool(std::chrono::milliseconds)> purgeGpuRetainedCaches, QObject* parent)
    : QObject(parent), scheduler_(scheduler), bridge_(bridge), diskCache_(diskCache),
      operationCache_(operationCache), preparedUploadCache_(preparedUploadCache),
      coverageCache_(coverageCache), clearDecodedVideoMemory_(std::move(clearDecodedVideoMemory)),
      setProducerGate_(std::move(setProducerGate)),
      purgeGpuRetainedCaches_(std::move(purgeGpuRetainedCaches)) {
    connect(&bridge_, &TaskUiBridge::snapshotsPolled, this, &CachePurgeController::poll);
}

CachePurgeController::~CachePurgeController() {
    if (task_.has_value()) {
        // Request cancellation; the shutdown coordinator's quiescence gate is what makes the
        // handle's destruction safe rather than a UI-thread join. The worker captures only cache
        // owners by value, never this controller, so a late result cannot reach a destroyed object.
        task_->cancel();
    }
    if (setProducerGate_) {
        setProducerGate_(false);
    }
}

bool CachePurgeController::requestPreviewPurge() { return start(Mode::Preview); }

bool CachePurgeController::requestMediaPurge() { return start(Mode::Media); }

bool CachePurgeController::start(const Mode mode) {
    if (mode_ != Mode::None) {
        return false;
    }
    mode_ = mode;
    // Gate the preview/background/RAM producers BEFORE waiting, so no request for the generation
    // being purged can be submitted between the quiescence observation and the clear.
    if (setProducerGate_) {
        setProducerGate_(true);
    }
    waitStartedAt_ = std::chrono::steady_clock::now();
    waitingForQuiescence_ = true;
    emit purgeStarted();
    poll();
    return true;
}

void CachePurgeController::poll() {
    if (mode_ == Mode::None) {
        return;
    }
    if (task_.has_value()) {
        auto result = task_->tryTakeResult();
        if (!result.has_value()) {
            return;
        }
        const bool success = result->state() == runtime::TaskState::Succeeded;
        const bool gpuIncomplete = success && result->value().has_value() && *result->value() != 0;
        const bool wasPreview = mode_ == Mode::Preview;
        task_.reset();
        if (!success) {
            finish(false, tr("The cache purge did not complete"));
        } else if (gpuIncomplete) {
            // The CPU-side stores were cleared, but the GPU resident cache did not confirm its
            // clear in time. Report the partial result honestly; do not claim everything purged.
            finish(false, wasPreview ? tr("Preview cache partially purged; the GPU cache is busy")
                                     : tr("Media cache partially purged; the GPU cache is busy"));
        } else {
            finish(true, wasPreview ? tr("Preview cache purged") : tr("Media cache purged"));
        }
        return;
    }
    if (!scheduler_.isQuiescent()) {
        if (waitingForQuiescence_ &&
            std::chrono::steady_clock::now() - waitStartedAt_ > kPurgeQuiescenceTimeout) {
            // Bounded safe failure: do not clear while work is still in flight, and do not leave
            // the commands disabled indefinitely. The artist can retry once the export finishes.
            finish(false, tr("Cache purge postponed; work is still running"));
        }
        return;
    }
    waitingForQuiescence_ = false;
    submitPurgeTask(mode_);
}

void CachePurgeController::submitPurgeTask(const Mode mode) {
    const auto executor =
        mode == Mode::Media ? runtime::TaskExecutor::BlockingIo : runtime::TaskExecutor::Cpu;
    runtime::TaskRequest request(
        "Purge derived caches",
        {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(1)},
        runtime::TaskPriority::Background, executor);
    // Capture the BORROWED store pointers and callbacks by value, never `this`: a result that
    // arrives after this controller is destroyed cannot touch the controller. The stores are not
    // owned here; the application composition root declares them after the scheduler and before
    // this controller, and the shutdown coordinator's quiescence gate guarantees no purge task is
    // still running when they are destroyed.
    auto* const disk = diskCache_;
    auto* const operation = operationCache_;
    auto* const prepared = preparedUploadCache_;
    auto* const coverage = coverageCache_;
    auto clearVideo = clearDecodedVideoMemory_;
    auto purgeGpu = purgeGpuRetainedCaches_;
    auto submission = scheduler_.submit<std::uint64_t>(
        std::move(request),
        [mode, disk, operation, prepared, coverage, clearVideo, purgeGpu](runtime::TaskContext&) {
            if (mode == Mode::Media) {
                // On the BlockingIo lane: flush the pending writer and remove every on-disk entry.
                // The source files are never opened for writing.
                purgeMediaCaches(disk, operation);
                if (clearVideo) {
                    clearVideo();
                }
            } else if (operation != nullptr) {
                operation->clearOperations();
            }
            // The GPU scene's derived CPU stores belong to both paths.
            if (prepared != nullptr) {
                prepared->clear();
            }
            if (coverage != nullptr) {
                coverage->clear();
            }
            std::uint64_t incomplete = 0;
            if (purgeGpu) {
                // Both purges clear the resident GPU scene cache: it retains decoded/uploaded media
                // source images as well as operation intermediates, so a media purge that skipped
                // it would let the next GPU frame reuse a source the artist just purged. The owning
                // service clears on its own owner thread, preserving every outstanding pin/lease.
                // Runs here, on a worker, never the UI.
                if (!purgeGpu(kGpuPurgeTimeout)) {
                    incomplete = 1;
                }
            }
            return runtime::TaskResult<std::uint64_t>::succeeded(incomplete);
        });
    if (!submission.accepted()) {
        finish(false, tr("The cache purge could not start"));
        return;
    }
    task_.emplace(std::move(submission.handle));
    bridge_.wake();
}

void CachePurgeController::finish(const bool success, const QString& message) {
    mode_ = Mode::None;
    waitingForQuiescence_ = false;
    if (setProducerGate_) {
        setProducerGate_(false);
    }
    emit purgeFinished(success, message);
}

} // namespace bloom::ui
