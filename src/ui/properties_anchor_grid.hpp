#pragma once

#include <QWidget>
#include <atomic>
#include <bloom/document/document.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <optional>

class QTimer;
namespace bloom::ui {
class CompositionSession;

// Resolves the same immutable local geometry as the viewer, off the UI thread.
class PropertiesAnchorGrid final : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int selectedPoint READ selectedPoint)
  public:
    PropertiesAnchorGrid(CompositionSession& session, QWidget* parent);
    ~PropertiesAnchorGrid() override;
    void refresh();
    [[nodiscard]] int selectedPoint() const;
    [[nodiscard]] QRect pointRect(int index) const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    struct Result {
        std::optional<runtime::ContentBounds> bounds;
        QString diagnostic;
    };
    void start();
    void poll();
    void choose(int index);
    CompositionSession& session_;
    std::optional<document::LayerId> layer_;
    std::optional<runtime::ContentBounds> bounds_;
    QString identity_;
    std::uint64_t generation_ = 0;
    std::uint64_t activeGeneration_ = 0;
    std::shared_ptr<runtime::TaskScheduler> scheduler_;
    std::shared_ptr<std::atomic_bool> retire_;
    runtime::TaskHandle<std::shared_ptr<Result>> task_;
    QTimer* timer_;
    bool active_ = false;
};
} // namespace bloom::ui
