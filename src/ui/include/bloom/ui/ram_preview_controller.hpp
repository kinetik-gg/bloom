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
// scope it to -- the timeline's work-area strip is honestly the whole duration, with no in/out points
// in the document model -- so there is nothing narrower to offer yet.
//
// One frame is cached at a time, as its own Foreground task, and the next is submitted when the last
// one lands. That is deliberate: the row-band pool already spends every core on the frame in flight,
// so running two frames at once would not cache the range faster, and a queue of frames would make
// cancellation and progress both harder to state truthfully.
//
// Frames already in the cache are counted as cached without being rendered again, so asking for a RAM
// preview twice is immediate the second time.
//
// This controller never starts playback itself: the transport belongs to the Timeline editor, which
// connects to cachingFinished() and plays when the range is complete. A RAM preview with no Timeline
// open therefore fills the cache and stops, which is the honest outcome rather than a second transport.
class RamPreviewController final : public QObject {
    Q_OBJECT

  public:
    RamPreviewController(CompositionSession& session,
                         CompositionPreviewController& previewController,
                         runtime::TaskScheduler& scheduler, TaskUiBridge& taskUiBridge,
                         PreviewPreparationFunction preparation, QObject* parent = nullptr);
    ~RamPreviewController() override;

    [[nodiscard]] bool isCaching() const noexcept { return caching_; }
    // How many frames of the range reached the cache, and how many the range holds. Both are zero
    // before the first run and are kept after one ends, so a caller can ask what the last run
    // achieved; isCaching() is what says whether they are still moving.
    [[nodiscard]] std::uint64_t cachedFrameCount() const noexcept { return cachedFrameCount_; }
    [[nodiscard]] std::uint64_t totalFrameCount() const noexcept { return totalFrameCount_; }
    [[nodiscard]] bool isShuttingDown() const noexcept { return shuttingDown_; }

  public slots:
    // Caches the composition's frame range, then asks the transport to play it. A no-op while already
    // caching, with no composition, or with a duration/frame rate that cannot form a frame mapping.
    void start();
    // Ends the run where it is, keeping every frame already cached. Wired to Escape.
    void cancel();
    // Start, or cancel a run in progress -- the one method both the transport button and the
    // Composition menu call.
    void toggle();
    void beginShutdown();

  signals:
    void stateChanged();
    // True when the whole range reached the cache, false when the run ended early (cancelled, or a
    // frame could not be prepared). The Timeline's transport plays on true.
    void cachingFinished(bool completed);

  private:
    void submitNextFrame();
    void consumeReadyResult();
    void finish(bool completed);
    void cancelAndDetachActive() noexcept;
    void publishProgress();

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& taskUiBridge_;
    PreviewPreparationFunction preparation_;

    std::optional<runtime::TaskHandle<PreviewPreparationResultHandle>> active_;
    // The document the whole range is rendered from, pinned when the run starts: every frame of one
    // RAM preview has to be the same document, and the cache key's revision is how that is enforced.
    std::optional<document::Snapshot> snapshot_;
    document::CompositionId compositionId_;
    std::uint64_t nextFrameIndex_ = 0;
    std::uint64_t cachedFrameCount_ = 0;
    std::uint64_t totalFrameCount_ = 0;
    std::uint64_t generation_ = 0;
    bool caching_ = false;
    bool shuttingDown_ = false;
};

} // namespace bloom::ui
