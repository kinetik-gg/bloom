#pragma once

#include <QObject>

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
    CompositionPreviewController& previewController_;
    TaskUiBridge& taskUiBridge_;
    bool shuttingDown_ = false;
    bool quiescencePublished_ = false;
};

} // namespace bloom::ui
