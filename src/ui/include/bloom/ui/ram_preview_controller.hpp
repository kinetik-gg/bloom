#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/ram_preview_pipeline.hpp>

#include <QObject>

#include <cstdint>
#include <optional>

namespace bloom::ui {

class CompositionSession;
class TaskUiBridge;

// "RAM Preview": pre-render a composition's whole frame range into the RAM preview cache, then play
// it back from there (docs/architecture/animation-and-time.md, "RAM preview").
//
// The range is the composition's own [0, duration) frame indices. Bloom has no work-area range to
// scope it to -- the timeline's work-area strip is honestly the whole duration, with no in/out
// points in the document model -- so there is nothing narrower to offer yet.
//
// TWO frames are prepared at a time, and that is the whole point of this controller's shape. The
// previous one-frame version was correct but throughput-bound: the row-band pool finishes one frame
// and then the next frame starts, so the CPU never overlaps with the previous frame's display
// stage. Keeping the next frame's CPU preparation in flight while the previous one is still being
// displayed is what a RAM preview is for. RamPreviewPipeline owns the bound (never more than two)
// and the identity of each in-flight frame; this controller owns the range cursor, the per-time
// snapshot resolution, and the out-of-order result validation.
//
// Frames already in the cache are counted as cached without being rendered again, so asking for a
// RAM preview twice is immediate the second time. A range or provenance edit never starts a new
// run: the in-flight frames are allowed to land (retained only if the new per-time evaluation still
// accepts them), then the range is rebased and only the still-missing frames are prepared.
//
// This controller never starts playback itself: the transport belongs to the Timeline editor, which
// connects to cachingFinished() and plays when the range is complete. A RAM preview with no
// Timeline open therefore fills the cache and stops, which is the honest outcome rather than a
// second transport.
class RamPreviewController final : public QObject {
    Q_OBJECT

  public:
    RamPreviewController(CompositionSession& session,
                         CompositionPreviewController& previewController,
                         runtime::TaskScheduler& scheduler, TaskUiBridge& taskUiBridge,
                         PreviewPreparationFunction preparation, QObject* parent = nullptr,
                         PreviewPreparationSubmitter submitter = {});
    ~RamPreviewController() override;

    [[nodiscard]] bool isCaching() const noexcept { return caching_; }
    // How many frames of the range reached the cache, and how many the range holds. Both are zero
    // before the first run and are kept after one ends, so a caller can ask what the last run
    // achieved; isCaching() is what says whether they are still moving.
    [[nodiscard]] std::uint64_t cachedFrameCount() const noexcept { return cachedFrameCount_; }
    [[nodiscard]] std::uint64_t totalFrameCount() const noexcept { return totalFrameCount_; }
    [[nodiscard]] bool isShuttingDown() const noexcept { return shuttingDown_; }
    // The high-water mark of frames ever preparing at once for this controller (across its runs).
    // Never exceeds kMaxInFlight; exposed so a test can pin the bound without racing the workers.
    [[nodiscard]] std::uint32_t peakInFlightFrames() const noexcept {
        return pipeline_.peakInFlight();
    }

  public slots:
    // Caches the composition's frame range, then asks the transport to play it. A no-op while
    // already caching, with no composition, or with a duration/frame rate that cannot form a frame
    // mapping.
    void start();
    // Ends the run where it is, keeping every frame already cached. Wired to Escape.
    void cancel();
    // Start, or cancel a run in progress -- the one method both the transport button and the
    // Composition menu call.
    void toggle();
    void beginShutdown();

  signals:
    void stateChanged();
    // True when the run finished on its own -- the whole range cached, or as much of it as the
    // memory budget holds -- and false when it ended early (cancelled, or a frame could not be
    // prepared). The Timeline's transport plays on true: a partly cached range is still worth
    // playing, and the transport's own clock rule already decides frame by frame which frames it
    // can present exactly.
    void cachingFinished(bool completed);

  private:
    // The fill engine. Lands every ready result, honours a pending range/provenance rebase once no
    // frame is still preparing, submits until the pipeline is full, and finishes exactly when the
    // whole range has landed. Safe to call from a signal handler and from start().
    void advance();
    // Fills the pipeline up to kMaxInFlight, counting already-cached frames and skipping a frame
    // that is somehow already in flight. Never starts a new run.
    void requestFrames();
    // Moves every landed result out of the pipeline and validates it against the CURRENT per-time
    // evaluation snapshot and frame key before it is retained or counted. Returns false when it
    // ended the run (cancel/failure/budget), true otherwise.
    [[nodiscard]] bool drainResults();
    [[nodiscard]] bool resultIsCurrent(const PreviewFrameCacheKey& identityKey,
                                       const runtime::PreviewRequestIdentity& identity) const;
    [[nodiscard]] runtime::PreviewRequestIdentity identityFor(core::RationalTime time,
                                                              const document::Snapshot& snapshot);
    void consumeReadyResult();
    void finish(bool completed);
    void cancelAndDetachActive() noexcept;
    void publishProgress();
    // WORKAREA-1: adapt a run in progress to the live work area. A frame in flight is allowed to
    // land (and is retained only if it is still in range); the range is rebased only once no frame
    // is still preparing, then the run continues. Never turns a range edit into a new run.
    void handleWorkAreaChanged();
    // TEMPORAL-2B. A document edit either invalidates the whole range (a unitary live provenance:
    // cancel and let a later run start clean) or only a time interval (adapt and rescan, submitting
    // only the changed frames).
    void handleDocumentEvaluationChanged();
    // Recomputed from the live work area: first frame index, total count, and the counters the
    // progress readout and budget gate use. Cached frames inside the new range are skipped.
    void rebaseRange();

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& taskUiBridge_;
    PreviewPreparationFunction preparation_;
    PreviewPreparationSubmitter submitter_;

    // At most TWO frames preparing at once; the bound and cancellation live in the helper.
    RamPreviewPipeline pipeline_;
    // TEMPORAL-2B: the run no longer pins one snapshot. Each submitted time resolves the session's
    // genuine snapshot for that time, so a finite clip-range edit can leave several retained
    // revisions along the range without mixing provenance within any one frame.
    document::CompositionId compositionId_;
    std::uint64_t nextFrameIndex_ = 0;
    std::uint64_t firstFrameIndex_ = 0;
    std::uint64_t cachedFrameCount_ = 0;
    std::uint64_t totalFrameCount_ = 0;
    std::uint64_t generation_ = 0;
    // The cache's eviction count when this run began. An eviction after that means the range has
    // outgrown the budget.
    std::uint64_t evictionsAtStart_ = 0;
    bool caching_ = false;
    bool shuttingDown_ = false;
    // Set by a range/provenance edit while at least one frame is in flight; the run lands what it
    // has, rebases once the pipeline is empty, and then resubmits only the still-missing frames, so
    // no frame of the new range is skipped or submitted twice.
    bool rangeDirty_ = false;
};

} // namespace bloom::ui
