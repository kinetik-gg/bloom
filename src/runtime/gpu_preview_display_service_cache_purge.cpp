// "Purge preview cache" owner-thread handling for GpuPreviewDisplayService.
//
// The service owns its resident GpuSceneCache on the device owner thread. This translation unit is
// the one place that touches that native cache for a purge, so the clear can never run on the wrong
// thread. Clearing GpuSceneCache drops only the cache's own shared_ptr references: an image that
// another owner still pins -- a displayed frame, an export product, an in-flight lease -- stays
// valid, and no lease is globally invalidated. Nothing native is destroyed here.
//
// Request/claim/clear/cancel are serialized by the single `cachePurgeMutex`:
//   * `purgeServiceCaches()` (worker thread) queues `cachePurgePending` under the mutex, wakes the
//     owner, then waits on the condition variable with the mutex released.
//   * `processServiceCachePurge()` (owner thread) takes the mutex, claims the pending request,
//     clears the cache WHILE HOLDING THE MUTEX, publishes `cachePurgeCompleted`, and releases.
//   * A caller that times out reacquires the mutex. If the owner has already claimed the request it
//     is either mid-clear (so the caller blocks until the clear completes and then observes
//     completion) or finished; either way the caller cannot return "timed out" while a clear is
//     still pending. If the request is still pending, the caller withdraws it under the same mutex,
//     and the owner will never clear it.
// `cachePurgePending` is always false once a request is claimed or withdrawn, so one purge cannot
// poison the next. The API is deliberately serial: a request is refused while one is pending.

#include "gpu_preview_display_service_private.hpp"

namespace bloom::runtime::detail {

void processServiceCachePurge(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    std::unique_lock lock(core->cachePurgeMutex);
    if (!core->cachePurgePending) {
        return;
    }
    core->cachePurgePending = false;
    if (core->residentSceneCache != nullptr) {
        core->residentSceneCache->clear();
    }
    ++core->cachePurgeCompleted;
    lock.unlock();
    core->cachePurgeCondition.notify_all();
}

bool purgeServiceCaches(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                        const std::chrono::milliseconds timeout) noexcept {
    // Immutable construction-time state only: the resident route exists exactly when a GPU scene
    // stage was supplied. The native `residentSceneCache` pointer is created, read, and destroyed
    // ONLY on the service owner thread, so a caller thread must never inspect it.
    if (core->gpuStageFunction == nullptr) {
        return true;
    }
    std::unique_lock lock(core->cachePurgeMutex);
    if (core->stopping.load(std::memory_order_acquire) || core->cachePurgePending) {
        return false;
    }
    const std::uint64_t target = core->cachePurgeCompleted + 1;
    core->cachePurgePending = true;
    lock.unlock();
    core->notify();
    lock.lock();
    static_cast<void>(core->cachePurgeCondition.wait_for(lock, timeout, [&] {
        return core->cachePurgeCompleted >= target ||
               core->stopping.load(std::memory_order_acquire);
    }));
    if (core->cachePurgeCompleted >= target) {
        return true;
    }
    // Timed out (or stopping) before the owner claimed it: withdraw under the mutex. The owner
    // checks `cachePurgePending` under this same mutex before clearing, so a withdrawn request can
    // never clear late.
    if (core->cachePurgePending) {
        core->cachePurgePending = false;
    }
    return false;
}

} // namespace bloom::runtime::detail
