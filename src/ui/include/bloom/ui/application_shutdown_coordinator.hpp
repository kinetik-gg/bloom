#pragma once

#include <QObject>
#include <QTimer>

class QEvent;

namespace bloom::ui {

class CompositionPreviewController;
class TaskUiBridge;

class ApplicationShutdownCoordinator final : public QObject {
    Q_OBJECT

  public:
    ApplicationShutdownCoordinator(CompositionPreviewController& previewController,
                                   TaskUiBridge& taskUiBridge, QObject* parent = nullptr);

    [[nodiscard]] bool isShuttingDown() const noexcept;

  public slots:
    void beginShutdown();

  signals:
    void shutdownStarted();
    void shutdownQuiescent();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    // FORMAL AMENDMENT 1 (S1-C): no shape in application_shutdown_tests.cpp reproduced a genuine
    // stuck shutdown (a solid layer's preview delivered then close, close while a preview is
    // in flight, close while playback is armed, close right after a completed frame export --
    // every one reaches shutdownQuiescent well within 2s). Per S1-C, this is a diagnostic, not a
    // force-quit fallback: if shutdownQuiescent still has not arrived 5s after beginShutdown(),
    // log the task bridge's outstanding task snapshots once so a real future occurrence is
    // self-explaining instead of silently unresponsive.
    void logStillShuttingDownDiagnostic() const;

    CompositionPreviewController& previewController_;
    TaskUiBridge& taskUiBridge_;
    QTimer stuckShutdownDiagnosticTimer_;
    bool shuttingDown_ = false;
    bool quiescencePublished_ = false;
};

} // namespace bloom::ui
