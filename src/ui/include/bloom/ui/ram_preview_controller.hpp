#pragma once

#include <functional>

#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/ram_preview_pipeline.hpp>

#include <QObject>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace bloom::ui {

class CompositionSession;
class TaskUiBridge;

// Exact frame-grid coverage independent of pixel payloads. Each insertion/removal touches its
// neighboring intervals; callers may enumerate all indices only at an explicit requalification
// boundary. Exposed as a small metadata object so large-range work can be tested deterministically
// without manufacturing thousands of rendered frames.
class RamPreviewCoverageIndex final {
  public:
    struct Work final {
        std::uint64_t mutations = 0;
        std::uint64_t neighborIntervalsVisited = 0;
    };
    [[nodiscard]] bool add(std::uint64_t index);
    [[nodiscard]] bool remove(std::uint64_t index);
    void clear() noexcept;
    [[nodiscard]] bool contains(std::uint64_t index) const { return indices_.contains(index); }
    [[nodiscard]] std::size_t size() const noexcept { return indices_.size(); }
    [[nodiscard]] const std::set<std::uint64_t>& indices() const noexcept { return indices_; }
    // Half-open [first,second) longest span, earliest start on ties.
    [[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>> longest() const noexcept;
    [[nodiscard]] Work work() const noexcept { return work_; }
    void resetWorkForTesting() noexcept { work_ = {}; }

  private:
    struct RankLess final {
        bool operator()(const std::pair<std::uint64_t, std::uint64_t>& left,
                        const std::pair<std::uint64_t, std::uint64_t>& right) const noexcept {
            if (left.first != right.first)
                return left.first > right.first;
            return left.second < right.second;
        }
    };
    std::set<std::uint64_t> indices_;
    std::map<std::uint64_t, std::uint64_t> spans_;
    std::set<std::pair<std::uint64_t, std::uint64_t>, RankLess> ranked_;
    Work work_;
};

// "RAM Preview": pre-render a composition's whole frame range into the RAM preview cache, then play
// it back from there (docs/architecture/animation-and-time.md, "RAM preview").
//
// The requested range is the live half-open work area, or the full duration when unset.
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
// connects to cachingFinished() and autoplays only a still-current fully retained range. A RAM
// preview with no Timeline open therefore fills the cache and stops, which is the honest outcome
// rather than a second transport.
class RamPreviewController final : public QObject {
    Q_OBJECT

  public:
    enum class Completion : std::uint8_t { Complete, PartialBudget, Cancelled, Failed };
    struct PlayableSpan final {
        core::RationalTime start;
        core::RationalTime end; // exclusive
        std::uint64_t frameCount = 0;
        friend bool operator==(const PlayableSpan&, const PlayableSpan&) = default;
    };
    // Historical successful preparations this run; includes results later evicted or made stale.
    [[nodiscard]] std::uint64_t preparedFrameCount() const noexcept { return preparedFrameCount_; }
    // Current accepted frame-grid intersection, without cache-hit/LRU side effects.
    [[nodiscard]] const std::vector<std::uint64_t>& retainedFrameIndices() const {
        if (retainedIndicesDirty_) {
            retainedFrameIndices_.assign(coverageIndex_.indices().begin(),
                                         coverageIndex_.indices().end());
            retainedIndicesDirty_ = false;
        }
        return retainedFrameIndices_;
    }
    [[nodiscard]] const std::optional<PlayableSpan>& playableSpan() const noexcept {
        return playableSpan_;
    }
    [[nodiscard]] std::optional<Completion> lastCompletion() const noexcept {
        return lastCompletion_;
    }
    RamPreviewController(CompositionSession& session,
                         CompositionPreviewController& previewController,
                         runtime::TaskScheduler& scheduler, TaskUiBridge& taskUiBridge,
                         PreviewPreparationFunction preparation, QObject* parent = nullptr,
                         PreviewPreparationSubmitter submitter = {},
                         std::optional<std::size_t> preparationWidth = std::nullopt);
    ~RamPreviewController() override;

    // Refresh at the autoplay boundary, including external callbacks/rebinds and expired leases.
    [[nodiscard]] bool isFullRangeRetained();
    [[nodiscard]] bool canAutoPlay();
    // A still-current run that ended PartialBudget (the range outgrew memory): playback starts
    // anyway from the range start, and Play's read-ahead renders the rest ahead of the playhead
    // while the cache evicts least-recently-used frames. Output length is never limited by RAM.
    [[nodiscard]] bool canPlayPartial();
    [[nodiscard]] bool isCaching() const noexcept { return caching_; }
    // Live exact-grid retention and requested range size. Coverage follows cache mutations after
    // completion too; preparedFrameCount()/lastCompletion() alone describe historical work.
    [[nodiscard]] std::uint64_t cachedFrameCount() const noexcept { return cachedFrameCount_; }
    [[nodiscard]] std::uint64_t totalFrameCount() const noexcept { return totalFrameCount_; }
    [[nodiscard]] bool isShuttingDown() const noexcept { return shuttingDown_; }
    struct MetadataWork final {
        std::uint64_t fullRequalifications = 0;
        std::uint64_t requalifiedEntries = 0;
        std::uint64_t deltaUpdates = 0;
    };
    [[nodiscard]] MetadataWork metadataWork() const noexcept { return metadataWork_; }
    void resetMetadataWorkForTesting() noexcept { metadataWork_ = {}; }
    // Frames in flight are sized from measured capacity: the provider answers how many frames of
    // `framePixels` fit the processing root's free capacity now (at least one is always tried).
    // Without a provider the constructor's width applies.
    using FillWidthProvider = std::function<std::size_t(std::uint64_t framePixels)>;
    void setFillWidthProvider(FillWidthProvider provider) {
        fillWidthProvider_ = std::move(provider);
    }
    [[nodiscard]] std::size_t fillWidth() const noexcept { return pipeline_.width(); }
    // The high-water mark of frames ever preparing at once for this controller (across its runs).
    // Exposed so a test can pin the bound without racing the workers.
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
    // Complete means every exact requested key is live at completion. PartialBudget preserves
    // useful retained samples without autoplay; Cancelled and Failed also keep live overlap.
    // Historical completion never certifies current coverage after eviction, purge or rebind.
    void cachingFinished(Completion outcome);

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
    void finish(Completion outcome);
    void refreshCoverage(bool notify = true, bool force = false);
    void onEntryRetained(const PreviewFrameCacheKey& key);
    void onEntryRemoved(const PreviewFrameCacheKey& key);
    void onEntriesCleared();
    void resolvePendingRetentionLoss() noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    acceptedFrameIndex(const PreviewFrameCacheKey& key) const;
    void addRetainedIndex(std::uint64_t index);
    void removeRetainedIndex(std::uint64_t index);
    void updatePlayableSpan();
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
    FillWidthProvider fillWidthProvider_;
    bool capacityRetryScheduled_ = false;
    RamPreviewPipeline pipeline_;
    // TEMPORAL-2B: the run no longer pins one snapshot. Each submitted time resolves the session's
    // genuine snapshot for that time, so a finite clip-range edit can leave several retained
    // revisions along the range without mixing provenance within any one frame.
    document::CompositionId compositionId_;
    std::uint64_t nextFrameIndex_ = 0;
    std::uint64_t firstFrameIndex_ = 0;
    std::uint64_t cachedFrameCount_ = 0;
    std::uint64_t preparedFrameCount_ = 0;
    document::ProjectId projectId_;
    mutable std::vector<std::uint64_t> retainedFrameIndices_;
    mutable bool retainedIndicesDirty_ = false;
    RamPreviewCoverageIndex coverageIndex_;
    std::optional<PreviewFrameCacheKey> coverageProbe_;
    core::RationalTime coverageStart_;
    core::RationalTime coverageEnd_;
    std::optional<PlayableSpan> playableSpan_;
    std::optional<Completion> lastCompletion_;
    std::uint64_t totalFrameCount_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t runSerial_ = 0;
    std::optional<Completion> retentionLoss_;
    // Memory evicted or refused some of this run's frames. A RAM preview never stops for memory:
    // it keeps filling (the cache evicts its least recently used frames) and ends PartialBudget.
    bool partialForMemory_ = false;
    // A cache insert can remove an expired entry and retain its exact replacement before it
    // returns. Judge removals after that transaction, when the final coverage is known.
    std::map<std::uint64_t, Completion> pendingRetentionLoss_;
    std::uint64_t pressureDropsObserved_ = 0;
    std::uint64_t staleDropsObserved_ = 0;
    std::optional<PreviewFrameCacheKey> completedProbe_;
    core::RationalTime completedEnd_;
    document::Revision completedRevision_;
    // The cache's eviction count when this run began. An eviction after that means the range has
    // outgrown the budget.
    std::uint64_t evictionsAtStart_ = 0;
    bool caching_ = false;
    bool shuttingDown_ = false;
    // Set by a range/provenance edit while at least one frame is in flight; the run lands what it
    // has, rebases once the pipeline is empty, and then resubmits only the still-missing frames, so
    // no frame of the new range is skipped or submitted twice.
    bool rangeDirty_ = false;
    MetadataWork metadataWork_;
};

} // namespace bloom::ui
