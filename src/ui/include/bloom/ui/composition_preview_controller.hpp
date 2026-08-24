#pragma once

#include <bloom/document/document.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace bloom::ui {

class CompositionSession;
class TaskUiBridge;

inline constexpr std::size_t kDefaultPreviewPixelStorageByteLimit = 512U * 1024U * 1024U;

struct CompositionPreviewSettings final {
    runtime::EvaluationResolution resolution = runtime::CompositionFormatResolution{};
    runtime::EvaluationQuality quality = runtime::EvaluationQuality::Reference;
    runtime::EvaluationColorIntent colorIntent =
        runtime::EvaluationColorIntent::ReferenceLinearSrgb;
    std::size_t pixelStorageByteLimit = kDefaultPreviewPixelStorageByteLimit;

    friend bool operator==(const CompositionPreviewSettings&,
                           const CompositionPreviewSettings&) = default;
};

using PreparedPreviewFrameHandle = std::shared_ptr<const runtime::PreparedPreviewFrame>;
using PreviewPreparationResultHandle = runtime::PreviewPreparationResultHandle;
using PreviewPreparationFunction =
    std::function<runtime::TaskResult<PreviewPreparationResultHandle>(
        const document::Snapshot&, const runtime::PreviewRequestIdentity&, std::size_t,
        runtime::TaskContext&)>;

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

  signals:
    void stateChanged();

  private:
    struct ActiveRequest final {
        runtime::TaskHandle<PreviewPreparationResultHandle> handle;
        runtime::PreviewRequestIdentity desiredIdentity;
    };

    void requestPreview(bool clearLastGoodFrame);
    void handleCompositionChanged();
    void handleCurrentTimeChanged();
    void consumeReadyResult();
    void cancelAndDetachActive() noexcept;
    void publish(CompositionPreviewState state);
    [[nodiscard]] bool isCurrent(const ActiveRequest& request) const noexcept;
    [[nodiscard]] bool
    liveSessionMatches(const runtime::PreviewRequestIdentity& desiredIdentity) const noexcept;

    CompositionSession& session_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& taskUiBridge_;
    PreviewPreparationFunction preparation_;
    CompositionPreviewSettings settings_;
    CompositionPreviewState state_;
    std::optional<ActiveRequest> active_;
    std::uint64_t generation_ = 0;
    bool shuttingDown_ = false;
};

} // namespace bloom::ui
