#pragma once

#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace bloom::runtime {
struct SnapshotCompileResult;
}

namespace bloom::ui {

class CompositionSession;
class TaskUiBridge;

using SnapshotCompileResultHandle = std::shared_ptr<const runtime::SnapshotCompileResult>;
using PreviewPreparationFunction = std::function<runtime::TaskResult<SnapshotCompileResultHandle>(
    const document::Snapshot&, document::CompositionId, runtime::TaskContext&)>;

enum class CompositionPreviewStatus {
    Preparing,
    Ready,
    Unsupported,
    Cancelled,
    Failed,
};

struct CompositionPreviewState {
    CompositionPreviewStatus status = CompositionPreviewStatus::Preparing;
    document::CompositionId compositionId;
    document::Revision sourceRevision;
    std::uint64_t requestGeneration = 0;
    std::optional<runtime::TaskId> taskId;
    SnapshotCompileResultHandle compileResult;
    std::vector<runtime::TaskDiagnostic> diagnostics;
    QString message;
};

class CompositionPreviewController final : public QObject {
    Q_OBJECT

  public:
    CompositionPreviewController(CompositionSession& session, runtime::TaskScheduler& scheduler,
                                 TaskUiBridge& taskUiBridge, PreviewPreparationFunction preparation,
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
    struct ActiveRequest {
        runtime::TaskHandle<SnapshotCompileResultHandle> handle;
        document::CompositionId compositionId;
        document::Revision sourceRevision;
        std::uint64_t generation = 0;
    };

    void requestPreview();
    void consumeReadyResult();
    void cancelAndDetachActive() noexcept;
    void publish(CompositionPreviewState state);
    [[nodiscard]] bool isCurrent(const ActiveRequest& request) const noexcept;

    CompositionSession& session_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& taskUiBridge_;
    PreviewPreparationFunction preparation_;
    CompositionPreviewState state_;
    std::optional<ActiveRequest> active_;
    std::uint64_t generation_ = 0;
    bool shuttingDown_ = false;
};

} // namespace bloom::ui
