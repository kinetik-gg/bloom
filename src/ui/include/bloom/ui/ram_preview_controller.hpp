#pragma once

#include <bloom/document/document.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>

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
// One frame is cached at a time, as its own Foreground task, and the next is submitted when the
// last one lands. That is deliberate: the row-band pool already spends every core on the frame in
// flight, so running two frames at once would not cache the range faster, and a queue of frames
// would make cancellation and progress both harder to state truthfully.
//
// Frames already in the cache are counted as cached without being rendered again, so asking for a
// RAM preview twice is immediate the second time.
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
    void submitNextFrame();
    void consumeReadyResult();
    void finish(bool completed);
    void cancelAndDetachActive() noexcept;
    void publishProgress();
    // WORKAREA-1: adapt a run in progress to the live work area. If a frame is in flight it is
    // allowed to land (and is retained only if it is still in range); otherwise the range is
    // rebased and the run continues. Never turns a range edit into a new run.
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

    std::optional<runtime::TaskHandle<PreviewPreparationResultHandle>> active_;
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
    // Set by a work-area edit while a frame is in flight; the completion rebases before continuing
    // so the old range's index bookkeeping cannot skip or duplicate a frame of the new range.
    bool rangeDirty_ = false;
};

} // namespace bloom::ui
