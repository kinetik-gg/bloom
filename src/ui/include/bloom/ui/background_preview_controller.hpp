#pragma once

#include <bloom/ui/composition_preview_controller.hpp>

#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace bloom::ui {

// Session-owned speculative work. Never publishes Viewer pixels or changes session time.
// A cancelled handle remains the one-in-flight gate until terminal, including across restarts.
// It runs only on explicit user intent: while playback is active it renders and caches the frames
// just ahead of the playhead (read-ahead sized from capacity) and stops when playback stops. An
// idle or edited composition is never filled in the background (owner decision, 2026-10-30); the
// idle outward fill exists only behind setIdleFill for qualification.
class BackgroundPreviewController final : public QObject {
    Q_OBJECT

  public:
    BackgroundPreviewController(CompositionSession& session,
                                CompositionPreviewController& previewController,
                                runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                                PreviewPreparationFunction preparation, QObject* parent = nullptr,
                                PreviewPreparationSubmitter submitter = {});
    ~BackgroundPreviewController() override;

    // Playback read-ahead: true while the transport plays. Driven by
    // CompositionPreviewController::playbackActiveChanged.
    void setPlaying(bool playing);
    // Idle outward fill around a stopped playhead. Off by default; production never enables it.
    void setIdleFill(bool enabled);
    [[nodiscard]] bool active() const noexcept { return playing_ || idleFill_; }
    // Frames in flight ahead of the playhead come from measured capacity: the provider answers
    // how many more frames of `framePixels` fit the processing root now. Without one, a single
    // frame is in flight.
    using ReadAheadWidthProvider = std::function<std::size_t(std::uint64_t framePixels)>;
    void setReadAheadWidthProvider(ReadAheadWidthProvider provider) {
        widthProvider_ = std::move(provider);
    }
    [[nodiscard]] std::size_t inFlightCount() const noexcept { return inFlight_.size(); }

  public slots:
    void restart();
    // "Purge preview cache": drops the in-flight speculative frame without retaining it and parks
    // the pass until the next genuine restart trigger (an edit, a time change, a resolution
    // change), so an explicit purge is not immediately undone by the idle timer.
    void suspendForCachePurge();
    void beginShutdown();
    // Also callable by tests without waiting for the idle timer.
    void fillNextFrame();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void cancelActive();
    void consumeReadyResult();

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    PreviewPreparationFunction preparation_;
    PreviewPreparationSubmitter submitter_;
    QTimer idleTimer_;
    struct InFlight final {
        runtime::TaskHandle<PreviewPreparationResultHandle> handle;
        runtime::PreviewRequestIdentity identity;
        std::chrono::steady_clock::time_point submittedAt;
        // A cancelled frame stays a slot until terminal; its result is discarded.
        bool discard = false;
    };
    std::vector<InFlight> inFlight_;
    ReadAheadWidthProvider widthProvider_;
    std::size_t readAheadWidth_ = 1;
    // Average time from submitting a read-ahead frame to its landing; sets the playback lead.
    std::chrono::steady_clock::duration landedLatency_{};
    std::uint64_t passAnchor_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t cursor_ = 0;
    std::uint64_t considered_ = 0;
    bool playing_ = false;
    bool idleFill_ = false;
    bool exhausted_ = false;
    bool shuttingDown_ = false;
};

} // namespace bloom::ui
