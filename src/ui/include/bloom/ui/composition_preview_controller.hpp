#pragma once

#include <bloom/document/document.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace bloom::ui {

class CompositionSession;
class TaskUiBridge;

inline constexpr std::size_t kDefaultPreviewPixelStorageByteLimit =
    std::size_t{512} * 1024U * 1024U;

// docs/architecture/animation-and-time.md, "Session Time And Scrubbing": scrub, playback, and
// direct manipulation submit at Interactive priority; discrete typed time entry, key selection,
// and document refresh submit at Visible priority (today's only submission priority).
enum class PreviewRequestKind : std::uint8_t {
    Interactive,
    Visible,
};

struct CompositionPreviewSettings final {
    runtime::EvaluationResolution resolution = runtime::CompositionFormatResolution{};
    runtime::EvaluationQuality quality = runtime::EvaluationQuality::Reference;
    runtime::EvaluationColorIntent colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene;
    std::size_t pixelStorageByteLimit = kDefaultPreviewPixelStorageByteLimit;
    // The injectable 16 ms trailing cadence for Interactive requests (pointer storms): a burst of
    // Interactive requests inside this window coalesces to only the newest, submitted once the
    // window elapses. Tests inject a tiny interval; production keeps the default.
    std::chrono::milliseconds interactiveTrailingCadence = std::chrono::milliseconds{16};
    // The RAM preview cache's memory budget, used only when this controller has to create its own
    // cache (see the constructor). The application reads it from QSettings.
    std::size_t ramPreviewByteBudget = kDefaultPreviewFrameCacheByteBudget;

    friend bool operator==(const CompositionPreviewSettings&,
                           const CompositionPreviewSettings&) = default;
};

using PreviewPreparationResultHandle = runtime::PreviewPreparationResultHandle;
// The fourth parameter carries the session's active-interaction override (docs/architecture/
// animation-and-time.md, "Direct Manipulation And Preview Overrides"): populated only for
// Interactive requests built while a position interaction is armed, std::nullopt otherwise. It
// rides straight into SnapshotCompileRequest::parameterOverride in
// makeCompositionPreviewPipeline().
using PreviewPreparationFunction =
    std::function<runtime::TaskResult<PreviewPreparationResultHandle>(
        const document::Snapshot&, const runtime::PreviewRequestIdentity&, std::size_t,
        const std::optional<runtime::SnapshotParameterOverride>&, runtime::TaskContext&)>;

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
    // and every frame this controller publishes is put into it -- so playing a range once makes the
    // second pass a sequence of lookups. Pass one to share it with the RAM preview controller; omit
    // it and this controller owns a cache of its own.
    CompositionPreviewController(CompositionSession& session, runtime::TaskScheduler& scheduler,
                                 TaskUiBridge& taskUiBridge, PreviewPreparationFunction preparation,
                                 CompositionPreviewSettings settings = {},
                                 PreviewFrameCacheHandle frameCache = nullptr,
                                 QObject* parent = nullptr);
    ~CompositionPreviewController() override;

    [[nodiscard]] const CompositionPreviewState& state() const noexcept;
    [[nodiscard]] bool isShuttingDown() const noexcept;
    [[nodiscard]] PreviewFrameCache& frameCache() const noexcept;
    // The identity this controller WOULD request for `time` in the live composition, which is what
    // a caller asks the cache about when it wants to know whether a frame is already there (the RAM
    // preview controller, and the transport deciding which clock to keep). Only the request
    // generation is missing from it, and the cache key does not carry one.
    [[nodiscard]] std::optional<PreviewFrameCacheKey>
    cacheKeyForTime(core::RationalTime time) const;

    // --- Dropped-frame accounting (task S5, item 3b) -------------------------------------------
    //
    // How many preview frames this controller was ASKED for and never delivered, while counting is
    // armed. A frame is counted dropped at exactly the three places this controller discards work
    // it was asked to do:
    //
    //   * a newer request supersedes a pending one that had not been submitted yet (both the
    //     active-task gate and the Interactive trailing-cadence window);
    //   * a Visible request bypasses the cadence and discards an Interactive request still waiting
    //     it out;
    //   * an active task reaches terminal while a newer pending request exists, so its finished
    //     result is thrown away unpublished.
    //
    // This is deliberately NOT a frame rate and makes no real-time claim: it counts requests the
    // coalescing path dropped, which is the only honest number this layer actually knows. The
    // transport's own decision to SKIP frame indices (PlaybackController recomputes its target from
    // total elapsed time) never reaches this controller as a request at all and is therefore not
    // counted here.
    [[nodiscard]] std::uint64_t droppedFrameCount() const noexcept;
    [[nodiscard]] bool isCountingDroppedFrames() const noexcept;
    // Arms counting and RESETS the count to zero (the transport calls this from play()); disarms it
    // (from pause()). While disarmed nothing is counted and the last run's total is kept at zero,
    // so a surface showing it cannot display a stale figure from a previous playback run.
    void beginDroppedFrameCounting();
    void endDroppedFrameCounting();

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
    void stateChanged();
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
    };

    struct PendingRequest final {
        document::Snapshot snapshot;
        runtime::PreviewRequestIdentity desiredIdentity;
        std::size_t pixelStorageByteLimit;
        PreviewRequestKind kind = PreviewRequestKind::Visible;
        // Sourced fresh from the session at requestPreview() build time; never cached across
        // requests (docs/architecture/animation-and-time.md).
        std::optional<runtime::SnapshotParameterOverride> interactionOverride;
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
    void handleCurrentTimeChanged();
    void handlePositionInteractionChanged();
    void consumeReadyResult();
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
    CompositionPreviewSettings settings_;
    PreviewFrameCacheHandle frameCache_;
    CompositionPreviewState state_;
    std::optional<ActiveRequest> active_;
    std::optional<PendingRequest> pending_;
    QTimer interactiveCadenceTimer_;
    bool interactiveTimeChangeArmed_ = false;
    std::optional<RamPreviewProgress> ramPreviewProgress_;
    bool countingDroppedFrames_ = false;
    std::uint64_t droppedFrameCount_ = 0;
    std::uint64_t generation_ = 0;
    bool shuttingDown_ = false;
};

} // namespace bloom::ui
