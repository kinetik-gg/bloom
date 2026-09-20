#pragma once

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// docs/architecture/gpu-backend.md "Host gating and application integration": the GPU
// final-render bridge is wired to every real output-analysis caller (desktop frame export,
// frame-range/video sequence export, headless scripting, and MCP) from ONE application-owned,
// explicitly constructed provider -- never a global singleton, service locator, or per-frame
// create/destroy.
//
// `GpuProcessFrameEvaluator::create()` blocks the calling thread until the native device, cache,
// and executor bootstrap completes, so constructing it from a UI callback would violate
// gpu-backend.md's "Never ... destroy a busy GPU resource on the UI thread" rule. This provider is
// cheap to construct (no device, thread, or file I/O) and lazily schedules the one-time bootstrap
// on a `TaskScheduler` worker via `prepare()`.
//
// Retirement is a genuine asynchronous lifecycle, not a rename of a blocking join:
//   * `beginShutdown()` only signals the evaluator's owner worker to leave its request loop and
//     destroy the device/cache/executor generation on that owner thread. It never joins.
//   * `retirementComplete()` becomes true when that owner worker has fully retired (the runtime
//     evaluator publishes it after the owner thread function returns). Only then may a caller
//     release the evaluator; releasing an already-retired evaluator joins a finished thread.
//   * `collectRetired()` performs that release exactly once, and only when completion is proven.
//     It is the app shutdown coordinator's poll step in the UI: the UI event loop keeps running
//     and never blocks on the native owner, even after task admission has closed.
//   * The provider keeps the evaluator (and therefore a strong owner) until `collectRetired()`.
//     The async attempt retains the provider itself through the immutable request, so a completed
//     attempt's evaluator handle is released on the evaluation worker, never as the last owner on
//     the UI thread.
//
// The provider borrows the scheduler only for the one-time bootstrap submission call; it never
// stores or later dereferences a scheduler pointer, and no evaluator handle retains one.
// `prepare()`, `evaluator()`, `beginShutdown()`, and `retirementComplete()` never join a thread and
// never do device work on the caller's thread. `retirementComplete()` is genuine proof, not a
// signal: while a scheduled bootstrap is still in flight it stays false even after
// `beginShutdown()`, and it becomes true only when the bootstrap reaches its terminal state (the
// owned task's observable completion) or a published evaluator's owner worker has fully retired.
// The CPU reference evaluator remains the correctness oracle; this provider never flips an
// operation's qualification or claims an unsupported scene as GPU.
namespace bloom::host {

class GpuExportProvider final {
  public:
    // Cheap: stores options only. `options.enabled` is the explicit off switch; when false the
    // scheduled bootstrap creates the existing CPU-unavailable evaluator with no owner thread.
    [[nodiscard]] static std::shared_ptr<GpuExportProvider>
    create(runtime::GpuProcessFrameEvaluatorOptions options = {});

    ~GpuExportProvider();

    GpuExportProvider(const GpuExportProvider&) = delete;
    GpuExportProvider& operator=(const GpuExportProvider&) = delete;
    GpuExportProvider(GpuExportProvider&&) = delete;
    GpuExportProvider& operator=(GpuExportProvider&&) = delete;

    // Schedules the one-time lazy bootstrap on `scheduler`. Idempotent and non-blocking. The
    // scheduler must outlive the provider (it already must: it runs the bootstrap task). Safe to
    // call from any thread, including the authoring/UI thread.
    void prepare(runtime::TaskScheduler& scheduler);

    // Non-blocking. Null until the scheduled bootstrap has published an evaluator, and null when
    // the bootstrap genuinely failed. The returned handle is a strong owner; share it with each
    // async attempt.
    [[nodiscard]] std::shared_ptr<runtime::GpuProcessFrameEvaluator> evaluator() const;

    // True once the bootstrap has reached a terminal state, whether or not a device was found.
    [[nodiscard]] bool prepared() const noexcept;
    // True only when the published evaluator genuinely reports an available device.
    [[nodiscard]] bool deviceAvailable() const;

    // Non-blocking: signals the evaluator owner to retire. Never joins. Idempotent. Once called,
    // new evaluations are refused by the evaluator itself.
    void beginShutdown() noexcept;

    // Non-blocking. True only when retirement is genuinely complete:
    //   * no bootstrap was ever scheduled -> trivially complete;
    //   * a published evaluator's owner worker has fully retired; or
    //   * a scheduled bootstrap has reached its terminal state (its task completed, succeeded or
    //     cancelled) with no evaluator published.
    // While a scheduled bootstrap is still queued or constructing its device this stays FALSE even
    // after beginShutdown(): stopping is a signal, never completion proof. A cancelled-before-run
    // bootstrap is observed through the owned task handle's terminal result, so this can never wait
    // forever. The app shutdown coordinator polls this; a headless caller uses `shutdownAndWait()`.
    [[nodiscard]] bool retirementComplete() const;

    // Releases the evaluator if (and only if) retirement is complete; a no-op otherwise. Safe to
    // call repeatedly. Must be called before the provider is destroyed once retirement completes,
    // so the eventual release joins a finished thread. The app shutdown coordinator calls this
    // from its UI poll step after `retirementComplete()` turns true.
    void collectRetired() noexcept;

    // Convenience for synchronous headless callers (scripting/MCP render threads, never the UI):
    // begin shutdown, then poll `retirementComplete()` with a bounded sleep until it is true or the
    // timeout elapses, then `collectRetired()`. Returns true when retirement was proven complete.
    [[nodiscard]] bool shutdownAndWait(std::chrono::milliseconds timeout) noexcept;

    // Diagnostic/test seam: invoked with the id of the thread that performs the final evaluator
    // release inside `collectRetired()`.
    void setRetirementObserver(std::function<void(std::thread::id)> observer);

    // Qualified shader-tool paths (glslangValidator + spirv-val) for preparing GPU output-colour
    // commands on a CPU task. The composition root injects packaged, app-relative tool paths
    // through this typed seam; Bloom never searches PATH or accepts an ambient/user path. Empty
    // paths leave the GPU output-colour route unavailable (the CPU display path remains the honest
    // fallback).
    void setGpuDisplayCompileOptions(runtime::GpuOcioCompileOptions options);
    [[nodiscard]] bool gpuDisplayPreparationAvailable() const noexcept;

    // Blocking CPU-task preparation of one immutable DisplayRgba8 OCIO command from the exact
    // resolved display processor. Thread-safe; never called from the UI or the GPU owner thread.
    // The returned command is immutable and cache-shared across warm requests.
    [[nodiscard]] runtime::GpuOcioPreparationResult
    prepareGpuDisplayCommand(const color::ResolvedBloomNeutralConfig& config,
                             std::string_view display, std::string_view view,
                             runtime::GpuOcioCommandGeometry geometry,
                             const runtime::GpuOcioCancellation& cancel = {});

  private:
    explicit GpuExportProvider(runtime::GpuProcessFrameEvaluatorOptions options);

    struct State;
    std::shared_ptr<State> state_;
};

} // namespace bloom::host
