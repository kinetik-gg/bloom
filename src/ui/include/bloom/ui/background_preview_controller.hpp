#pragma once

#include <bloom/ui/composition_preview_controller.hpp>

#include <QObject>
#include <QTimer>

#include <cstdint>
#include <optional>

namespace bloom::ui {

// Session-owned speculative work. Never publishes Viewer pixels or changes session time.
// A cancelled handle remains the one-in-flight gate until terminal, including across restarts.
class BackgroundPreviewController final : public QObject {
    Q_OBJECT

  public:
    BackgroundPreviewController(CompositionSession& session,
                                CompositionPreviewController& previewController,
                                runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                                PreviewPreparationFunction preparation, QObject* parent = nullptr);
    ~BackgroundPreviewController() override;

    void setPlaying(bool playing);

  public slots:
    void restart();
    void beginShutdown();
    // Also callable by tests without waiting for the idle timer.
    void fillNextFrame();

  private:
    void cancelActive();
    void consumeReadyResult();

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    PreviewPreparationFunction preparation_;
    QTimer idleTimer_;
    std::optional<runtime::TaskHandle<PreviewPreparationResultHandle>> active_;
    std::optional<runtime::PreviewRequestIdentity> activeIdentity_;
    std::uint64_t generation_ = 0;
    std::uint64_t cursor_ = 0;
    std::uint64_t considered_ = 0;
    bool discardActive_ = false;
    bool playing_ = false;
    bool exhausted_ = false;
    bool shuttingDown_ = false;
};

} // namespace bloom::ui
