#pragma once

#include <bloom/document/document.hpp>
#include <bloom/runtime/gpu_memory_budget.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/viewer_editor_probe.hpp>

#include <QElapsedTimer>
#include <QObject>
#include <QRectF>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::ui {

class CompositionSession;
class PlaybackController;
class TaskUiBridge;

inline constexpr std::size_t kDefaultPreviewPixelStorageByteLimit =
    std::size_t{512} * 1024U * 1024U;

// Playback misses use Visible priority; an idle tick always submits, while a busy tick (an active
// or pending foreground request) is skipped rather than queued.
enum class PreviewRequestKind : std::uint8_t {
    Interactive,
    Visible,
    Playback,
};

struct CompositionPreviewSettings final {
    runtime::PreviewResolutionPolicy resolutionPolicy = runtime::PreviewResolutionPolicy::Auto;
    runtime::EvaluationQuality quality = runtime::EvaluationQuality::Reference;
    runtime::EvaluationColorIntent colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene;
    std::string displayName;
    std::string viewName;
    bool showLook = true;
    // Capacity-aware: sized from the RAM-preview allocation (gpuPreviewRequestByteAllowance()),
    // so the request allowance follows the machine and the operator's memory overrides instead of a
    // fixed constant. A constrained injected budget still refuses cleanly and takes the honest CPU
    // fallback.
    std::size_t pixelStorageByteLimit = runtime::gpuPreviewRequestByteAllowance();
    // The first Interactive request is immediate. Subsequent requests inside this 16 ms window
    // coalesce to the newest value; an active worker remains the admission gate.
    std::chrono::milliseconds interactiveTrailingCadence = std::chrono::milliseconds{16};
    // The RAM preview cache's memory budget, used only when this controller has to create its own
    // cache (see the constructor). The application reads it from QSettings.
    std::size_t ramPreviewByteBudget = defaultPreviewFrameCacheByteBudget();

    friend bool operator==(const CompositionPreviewSettings&,
                           const CompositionPreviewSettings&) = default;
};

using PreviewPreparationResultHandle = runtime::PreviewPreparationResultHandle;
// The fourth parameter carries the session's active-interaction override (docs/architecture/
// animation-and-time.md, "Direct Manipulation And Preview Overrides"): populated only for
// Interactive requests built while a position interaction is armed, empty otherwise. It
// rides straight into SnapshotCompileRequest::parameterOverrides in
// makeCompositionPreviewPipeline().
using PreviewPreparationFunction =
    std::function<runtime::TaskResult<PreviewPreparationResultHandle>(
        const document::Snapshot&, const runtime::PreviewRequestIdentity&, std::size_t,
        const std::vector<runtime::SnapshotParameterOverride>&, runtime::TaskContext&)>;

// An optional display-stage submitter: when provided, a controller hands the request it would
// otherwise submit itself to this callable and receives the same TaskSubmission shape. It exists so
// a runtime-owned service can own GPU display submission without the controller owning any GPU
// state. When absent, the existing CPU scheduler_.submit path is used unchanged.
using PreviewPreparationSubmitter =
    std::function<runtime::TaskSubmission<PreviewPreparationResultHandle>(
        runtime::TaskRequest, const document::Snapshot&, const runtime::PreviewRequestIdentity&,
        std::size_t, const std::vector<runtime::SnapshotParameterOverride>&)>;

enum class PreviewActivity : std::uint8_t {
    Rendering,
    Ready,
    Unsupported,
    Cancelled,
    Failed,
};

enum class FrameFreshness : std::uint8_t {
    None,
    Current,
    Stale,
};

// How far a RAM preview run has got, published here rather than on the RAM preview controller
// because the Viewer footer's one dependency is this controller -- the same reason the
// dropped-frame counter lives here (task S5, item 3b).
struct RamPreviewProgress final {
    std::uint64_t cachedFrames = 0;
    std::uint64_t totalFrames = 0;

    friend bool operator==(const RamPreviewProgress&, const RamPreviewProgress&) = default;
};

struct CompositionPreviewState final {
    PreviewActivity activity = PreviewActivity::Rendering;
    FrameFreshness freshness = FrameFreshness::None;
    std::optional<runtime::PreviewRequestIdentity> desiredIdentity;
    std::optional<runtime::TaskId> taskId;
    PreparedPreviewFrameHandle frame;
    std::vector<runtime::TaskDiagnostic> diagnostics;
    QString message;
};

class CompositionPreviewController final : public QObject {
    Q_OBJECT

  public:
    // `frameCache` is the RAM preview cache (task PERF1): a request whose key is already cached is
    // answered from it immediately, with no evaluation and without entering the coalescing path,
    // and frames without diagnostics enter it -- so playing a warning-free range once makes the
    // second pass a sequence of lookups. Pass one to share it with the RAM preview controller; omit
    // it and this controller owns a cache of its own.
    CompositionPreviewController(CompositionSession& session, runtime::TaskScheduler& scheduler,
                                 TaskUiBridge& taskUiBridge, PreviewPreparationFunction preparation,
                                 const CompositionPreviewSettings& settings = {},
                                 PreviewFrameCacheHandle frameCache = nullptr,
                                 QObject* parent = nullptr,
                                 PreviewPreparationSubmitter submitter = {});
    ~CompositionPreviewController() override;

    [[nodiscard]] const CompositionPreviewState& state() const noexcept;
    // Geometry belongs to the delivered immutable process frame, including its sampled time.
    // No layout, evaluation, or image walk takes place on the UI thread.
    [[nodiscard]] std::vector<runtime::EvaluatedOperationBounds> selectedLayerBounds() const;
    // SPLIT-2. The displayed frame's evaluated geometry, translated from its retained snapshot's
    // layer/node identities to the CURRENT live graph. This is the one place viewer paint/hit/text
    // consumers read geometry, so a retained frame after an equivalent split targets the live tail.
    // Metadata only: no pixel copy and no reevaluation.
    [[nodiscard]] std::vector<runtime::EvaluatedOperationBounds> currentLayerBounds() const;
    [[nodiscard]] bool isShuttingDown() const noexcept;
    [[nodiscard]] bool backgroundWorkAllowed() const noexcept;
    [[nodiscard]] PreviewFrameCache& frameCache() const noexcept;
    [[nodiscard]] PlaybackController& playbackController();
    // The identity this controller WOULD request for `time` in the live composition, which is what
    // a caller asks the cache about when it wants to know whether a frame is already there (the RAM
    // preview controller, and the transport deciding which clock to keep). Only the request
    // generation is missing from it, and the cache key does not carry one.
    [[nodiscard]] std::optional<PreviewFrameCacheKey>
    cacheKeyForTime(core::RationalTime time) const;

    // Playback counts skipped frame indices, rejected misses, and requests discarded or late.
    // Scrubbing outside playback never contributes. The last total survives pause().
    [[nodiscard]] std::uint64_t droppedFrameCount() const noexcept;
    [[nodiscard]] bool isCountingDroppedFrames() const noexcept;
    // Arms counting and RESETS the count to zero (the transport calls this from play()); disarms it
    // (from pause()). While disarmed nothing is counted and the last run's total is kept at zero,
    // so a surface showing it cannot display a stale figure from a previous playback run.
    void beginDroppedFrameCounting();
    void endDroppedFrameCounting();
    void noteDroppedFrames(std::uint64_t count);
    void setPlaybackActive(bool playing);
    void presentPlaybackFrame(std::chrono::nanoseconds untilNextTick);
    // Includes admission/queue and UI delivery latency. Only matching live identities contribute.
    void recordPreparationDuration(const runtime::PreviewRequestIdentity& identity,
                                   std::chrono::nanoseconds duration);

    // --- RAM preview progress (task PERF1, item 3) ---------------------------------------------
    //
    // Engaged exactly while a RAM preview run is caching, so a surface reading it says nothing at
    // all outside a run rather than "0/0". Driven by RamPreviewController; this controller neither
    // starts nor interprets a run.
    [[nodiscard]] const std::optional<RamPreviewProgress>& ramPreviewProgress() const noexcept;
    void beginRamPreviewProgress(std::uint64_t totalFrames);
    void setRamPreviewProgress(std::uint64_t cachedFrames);
    void endRamPreviewProgress();

    [[nodiscard]] const CompositionPreviewSettings& settings() const noexcept;
    [[nodiscard]] runtime::EvaluationResolution resolution() const;
    [[nodiscard]] std::uint32_t resolutionDivisor() const noexcept;
    void setResolutionPolicy(runtime::PreviewResolutionPolicy policy);
    void setViewerDisplayView(std::string displayName, std::string viewName);
    void setViewerLookEnabled(bool enabled);
    void setRegionOfInterest(std::optional<QRectF> region);
    [[nodiscard]] runtime::TaskSubmission<PreviewPreparationResultHandle>
    submitViewerAnalysis(const runtime::PreviewRequestIdentity& identity);
    [[nodiscard]] std::optional<render::ImageWindow> regionOfInterest() const;
    // Display pixels per composition pixel, including device pixel ratio. Unknown geometry uses 1.
    void setDisplayedCompositionScale(double scale);

  public slots:
    void requestRefresh();
    void beginShutdown();

    // Wired from the timeline's mouse-press (or any other Interactive-time-change gesture, e.g. a
    // future playback transport): while armed, the currentTimeChanged-triggered request below
    // submits at Interactive priority through the trailing cadence instead of Visible.
    void beginInteractiveScrub();
    // Wired from the timeline's mouse-release: disarms beginInteractiveScrub() and bypasses any
    // remaining trailing-cadence delay, submitting the newest pending Interactive request
    // immediately -- while still honoring the one-active/one-newest gate below.
    void notifyScrubEnded();

  signals:
    void probeChanged(ProbeReadout readout);
    void stateChanged();
    void resolutionChanged();
    // Synchronous cancellation seam for speculative work, before foreground admission.
    void foregroundWorkRequested();
    void playbackActiveChanged(bool playing);
    void interactiveScrubStarted();
    // Emitted whenever droppedFrameCount() or isCountingDroppedFrames() changes, so a footer
    // reading it never has to poll (the viewer's own refresh idiom is exactly this: connect, then
    // update()).
    void droppedFrameCountChanged();
    // Emitted whenever ramPreviewProgress() changes, so the footer reading it never polls.
    void ramPreviewProgressChanged();

  private:
    struct ActiveRequest final {
        runtime::TaskHandle<PreviewPreparationResultHandle> handle;
        runtime::PreviewRequestIdentity desiredIdentity;
        // An overridden request's pixels are the gesture's, not the revision's, and nothing in
        // PreviewRequestIdentity distinguishes the two -- so its frame must never reach the cache.
        bool carriedInteractionOverride = false;
        std::chrono::steady_clock::time_point submittedAt;
        std::optional<std::chrono::steady_clock::time_point> playbackDeadline;
        bool playbackOutstanding = false;
    };

    struct PendingRequest final {
        document::Snapshot snapshot;
        runtime::PreviewRequestIdentity desiredIdentity;
        std::size_t pixelStorageByteLimit;
        PreviewRequestKind kind = PreviewRequestKind::Visible;
        // Sourced fresh from the session at requestPreview() build time; never cached across
        // requests (docs/architecture/animation-and-time.md).
        std::vector<runtime::SnapshotParameterOverride> interactionOverride;
    };

    // `allowCachedFrame` is false for an explicit refresh: a refresh asks for the frame to be
    // re-derived because something the cache key does not cover may have changed -- the qualified
    // display transform becoming available, or failing, is the live example -- so answering it from
    // the cache would be answering a question nobody asked.
    void requestPreview(bool clearLastGoodFrame, PreviewRequestKind kind,
                        bool allowCachedFrame = true);
    void submitPreview(PendingRequest request, PreparedPreviewFrameHandle retainedFrame);
    void publishRendering(runtime::PreviewRequestIdentity desiredIdentity,
                          std::optional<runtime::TaskId> taskId,
                          PreparedPreviewFrameHandle retainedFrame);
    void handleCompositionChanged();
    // TEMPORAL-2B. A document command moved the time-indexed provenance. Re-scope cache retention,
    // then preserve the displayed frame (and any in-flight work) when its time still resolves to
    // the same genuine snapshot; only a frame whose time was inside the changed interval is
    // rebuilt, and it may still be answered from another retained segment's cache entry.
    void handleDocumentEvaluationChanged();
    // WORKAREA-1: re-scope the shared cache to the live work area. Prunes out-of-range entries and
    // bounds every later insertion; it never forces a pixel refresh, because the range is
    // render-neutral for the frame already displayed.
    void handleWorkAreaChanged();
    void refreshRetentionRange();
    void handleCurrentTimeChanged();
    void handleTransformInteractionChanged();
    void handleLiveValueChanged();
    void consumeReadyResult();
    [[nodiscard]] static FrameFreshness
    freshnessFor(const PreparedPreviewFrameHandle& frame,
                 const std::optional<runtime::PreviewRequestIdentity>& desiredIdentity);
    void cancelAndDetachActive() noexcept;
    void publish(CompositionPreviewState state);
    // Publishes `frame` as the answer to `desiredIdentity` with no task at all. Used only when the
    // cache already holds the exact frame the request asks for.
    void publishCachedFrame(const runtime::PreviewRequestIdentity& desiredIdentity,
                            PreparedPreviewFrameHandle frame);
    void flushCadence();
    // One place increments the counter, so the three drop sites cannot disagree about whether a
    // discard counts.
    void noteDroppedFrame();
    [[nodiscard]] bool isCurrent(const ActiveRequest& request) const;
    [[nodiscard]] bool
    liveSessionMatches(const runtime::PreviewRequestIdentity& desiredIdentity) const noexcept;

    CompositionSession& session_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& taskUiBridge_;
    PreviewPreparationFunction preparation_;
    PreviewPreparationSubmitter submitter_;
    CompositionPreviewSettings settings_;
    PreviewFrameCacheHandle frameCache_;
    CompositionPreviewState state_;
    std::optional<ActiveRequest> active_;
    std::optional<PendingRequest> pending_;
    QTimer interactiveCadenceTimer_;
    QTimer completionPollTimer_;
    QElapsedTimer interactiveSubmissionClock_;
    bool interactiveTimeChangeArmed_ = false;
    bool valueEditPreviewActive_ = false;
    std::optional<RamPreviewProgress> ramPreviewProgress_;
    bool countingDroppedFrames_ = false;
    std::uint64_t droppedFrameCount_ = 0;
    std::uint64_t generation_ = 0;
    bool shuttingDown_ = false;
    double displayedCompositionScale_ = 1.0;
    std::optional<QRectF> regionOfInterest_;
    std::unique_ptr<PlaybackController> playbackController_;
    bool playbackActive_ = false;
    std::chrono::nanoseconds playbackBudget_{};
    std::optional<std::chrono::nanoseconds> preparationEstimate_;
};

} // namespace bloom::ui
