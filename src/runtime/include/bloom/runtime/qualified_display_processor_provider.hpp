#pragma once

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/runtime/task_types.hpp>

#include <cstdint>
#include <memory>
#include <mutex>

// The one-time blocking-stage handoff (issue #97, task C3, design decision 3): "Processor
// preparation is a one-time blocking stage, not per-frame ... at pipeline/session construction,
// resolve Bloom Neutral through the C2 registry and build the processor ON THE EXISTING
// BLOCKING-I/O WORKER PATH ... The handle reaches the pipeline through the established handoff
// idiom (typed mailbox/shared state as the controller already does for frames)." This header owns
// the pure (Qt-free) build step and the thread-safe published-state slot; bloom::ui::
// QualifiedDisplayProcessorBootstrap (src/ui) owns submitting the build through
// bloom::runtime::TaskScheduler's TaskExecutor::BlockingIo lane and polling its completion the same
// non-blocking way bloom::ui::CompositionPreviewController already polls preview results (see
// TaskUiBridge::snapshotsPolled) -- no task ever calls wait/get, joins a worker, or blocks a UI
// callback.
namespace bloom::runtime {

// Runs entirely on a blocking-I/O worker thread; never on the UI thread. Resolves exactly the
// embedded Bloom Neutral v1 built-in through the C2 registry (bloom::color::
// resolveBloomNeutralV1BuiltIn) and, on success, builds its CPU display processor (bloom::color::
// buildBloomNeutralCpuDisplayProcessor). Per docs/architecture/color-management.md's "Configuration
// Resolution States", this should not fail for the embedded neutral asset, but the states are real
// -- every non-Ready registry outcome and every processor-build error is mapped to a typed,
// human-readable diagnostic rather than assumed unreachable.
class QualifiedDisplayProcessorBuildResult final {
  public:
    [[nodiscard]] static QualifiedDisplayProcessorBuildResult
    ready(std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> handle) noexcept;
    [[nodiscard]] static QualifiedDisplayProcessorBuildResult
    failed(TaskDiagnostic diagnostic) noexcept;

    [[nodiscard]] bool succeeded() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] const std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle>&
    handle() const& noexcept {
        return handle_;
    }
    [[nodiscard]] const std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle>&
    handle() const&& = delete;
    // Valid content only when !succeeded().
    [[nodiscard]] const TaskDiagnostic& diagnostic() const& noexcept { return diagnostic_; }
    [[nodiscard]] const TaskDiagnostic& diagnostic() const&& = delete;

  private:
    explicit QualifiedDisplayProcessorBuildResult(
        std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> handle) noexcept;
    explicit QualifiedDisplayProcessorBuildResult(TaskDiagnostic diagnostic) noexcept;

    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> handle_;
    TaskDiagnostic diagnostic_;
};

[[nodiscard]] QualifiedDisplayProcessorBuildResult
buildBloomNeutralQualifiedDisplayProcessor() noexcept;

enum class QualifiedDisplayProcessorReadiness : std::uint8_t {
    // The honest startup window (design decision 3): the blocking build has not completed yet.
    // Preview requests take the reference path, labeled unqualified -- never a fallback from
    // failure.
    Pending,
    Ready,
    // Fail-closed, terminal (design decision 4 / decision 6 "no caches" -- no retry slice exists
    // yet): registry resolution or processor build failed. Preview requests never auto-substitute
    // the reference transform for a qualified one once this state is observed.
    Failed,
};

// A single consistent read of readiness + handle + failure diagnostic together (one lock
// acquisition). Preview-pipeline callers must use this rather than separate readiness()/handle()
// calls whenever the same decision depends on more than one field: publish() is one-shot but not
// synchronous with a preview-render worker's own reads, so two SEPARATE locked reads could
// otherwise straddle the one publish() call and observe an inconsistent pair (for example,
// readiness() == Pending followed moments later by handle() == nullptr because publish() landed
// Failed in between -- which would look identical to "still Pending" to a caller that only checks
// handle() for nullness, silently reintroducing the reference fallback design decision 4 forbids
// after a genuine failure).
struct QualifiedDisplayProcessorSnapshot final {
    QualifiedDisplayProcessorReadiness readiness = QualifiedDisplayProcessorReadiness::Pending;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> handle;
    TaskDiagnostic failureDiagnostic;
};

// Mutex-guarded rather than lock-free: the one write happens once per process lifetime (or never,
// if the build never completes before shutdown); every preview-render worker thread's read is a
// short, uncontended critical section. This stays auditable without relying on
// std::atomic<std::shared_ptr<T>> library support.
class QualifiedDisplayProcessorProvider final {
  public:
    QualifiedDisplayProcessorProvider() = default;
    QualifiedDisplayProcessorProvider(const QualifiedDisplayProcessorProvider&) = delete;
    QualifiedDisplayProcessorProvider& operator=(const QualifiedDisplayProcessorProvider&) = delete;

    [[nodiscard]] QualifiedDisplayProcessorReadiness readiness() const noexcept;
    // Non-null exactly when readiness() == Ready; nullptr otherwise (including Failed).
    [[nodiscard]] std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle>
    handle() const noexcept;
    // Meaningful only when readiness() == Failed; default-constructed otherwise.
    [[nodiscard]] TaskDiagnostic failureDiagnostic() const;
    // The single-locked-read accessor documented above; prefer this over readiness()/handle()/
    // failureDiagnostic() whenever a caller's decision depends on more than one of them.
    [[nodiscard]] QualifiedDisplayProcessorSnapshot snapshot() const;

    // Called at most once, from the authoring/UI thread, by QualifiedDisplayProcessorBootstrap once
    // the blocking-stage task reaches a scheduler-terminal state. A call after the first is a no-op
    // -- this is a one-shot startup handoff (design decision 3), not a retryable or cached slot
    // (design decision 6: "no caches").
    void publish(const QualifiedDisplayProcessorBuildResult& result);

  private:
    mutable std::mutex mutex_;
    QualifiedDisplayProcessorReadiness readiness_ = QualifiedDisplayProcessorReadiness::Pending;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> handle_;
    TaskDiagnostic failureDiagnostic_;
};

} // namespace bloom::runtime
