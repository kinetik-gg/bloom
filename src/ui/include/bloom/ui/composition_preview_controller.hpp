#pragma once

#include <bloom/document/document.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

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

    friend bool operator==(const CompositionPreviewSettings&,
                           const CompositionPreviewSettings&) = default;
};

using PreparedPreviewFrameHandle = std::shared_ptr<const runtime::PreparedPreviewFrame>;
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
    CompositionPreviewController(CompositionSession& session, runtime::TaskScheduler& scheduler,
                                 TaskUiBridge& taskUiBridge, PreviewPreparationFunction preparation,
                                 CompositionPreviewSettings settings = {},
                                 QObject* parent = nullptr);
    ~CompositionPreviewController() override;

    [[nodiscard]] const CompositionPreviewState& state() const noexcept;
    [[nodiscard]] bool isShuttingDown() const noexcept;

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

  private:
    struct ActiveRequest final {
        runtime::TaskHandle<PreviewPreparationResultHandle> handle;
        runtime::PreviewRequestIdentity desiredIdentity;
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

    void requestPreview(bool clearLastGoodFrame, PreviewRequestKind kind);
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
    void flushCadence();
    [[nodiscard]] bool isCurrent(const ActiveRequest& request) const;
    [[nodiscard]] bool
    liveSessionMatches(const runtime::PreviewRequestIdentity& desiredIdentity) const noexcept;

    CompositionSession& session_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& taskUiBridge_;
    PreviewPreparationFunction preparation_;
    CompositionPreviewSettings settings_;
    CompositionPreviewState state_;
    std::optional<ActiveRequest> active_;
    std::optional<PendingRequest> pending_;
    QTimer interactiveCadenceTimer_;
    bool interactiveTimeChangeArmed_ = false;
    std::uint64_t generation_ = 0;
    bool shuttingDown_ = false;
};

} // namespace bloom::ui
