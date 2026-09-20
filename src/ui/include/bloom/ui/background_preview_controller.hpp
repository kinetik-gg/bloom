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
                                PreviewPreparationFunction preparation, QObject* parent = nullptr,
                                PreviewPreparationSubmitter submitter = {});
    ~BackgroundPreviewController() override;

    void setPlaying(bool playing);

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
    std::optional<runtime::TaskHandle<PreviewPreparationResultHandle>> active_;
    std::optional<runtime::PreviewRequestIdentity> activeIdentity_;
    std::chrono::steady_clock::time_point submittedAt_;
    std::uint64_t generation_ = 0;
    std::uint64_t cursor_ = 0;
    std::uint64_t considered_ = 0;
    bool discardActive_ = false;
    bool playing_ = false;
    bool exhausted_ = false;
    bool shuttingDown_ = false;
};

} // namespace bloom::ui
