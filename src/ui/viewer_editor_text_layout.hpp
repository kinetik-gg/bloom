#pragma once

#include <QObject>
#include <QPointF>
#include <QTimer>
#include <QTransform>
#include <atomic>
#include <bloom/commands/operations.hpp>
#include <bloom/document/document.hpp>
#include <bloom/render/text_raster.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <functional>
#include <optional>

namespace bloom::ui {

// One active task and one newest request. The worker owns all font I/O and placement;
// destroying the viewer cancels work without joining a worker on the event loop.
class ViewerTextLayout final : public QObject {
  public:
    struct Request {
        document::Snapshot snapshot;
        document::CompositionId composition;
        document::ParameterId parameter;
        std::string content;
        render::TextRasterParameters parameters;
        render::TextLayoutOptions options;
    };
    struct Result {
        render::TextLayout layout;
        render::TextFont font;
        std::string content;
        QString diagnostic;
    };
    explicit ViewerTextLayout(QObject* parent);
    ~ViewerTextLayout() override;
    void request(Request request);
    void cancel();
    std::function<void(Result)> ready;

  private:
    void start();
    void poll();
    QTimer timer_;
    std::shared_ptr<runtime::TaskScheduler> scheduler_;
    std::shared_ptr<std::atomic_bool> retire_;
    runtime::TaskHandle<std::shared_ptr<Result>> task_;
    std::optional<Request> pending_;
    std::optional<render::TextFont> font_;
    bool active_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t activeGeneration_ = 0;
};

struct ViewerTextEdit final {
    document::ParameterId parameter;
    document::LayerId layer;
    QString text;
    QString preedit;
    int cursor = 0;
    int anchor = 0;
    int preeditCursor = 0;
    bool preeditCursorVisible = true;
    bool multiline = false;
    bool dragging = false;
    bool layoutReady = false;
    std::optional<QPointF> pendingClick;
    render::TextLayout layout;
    QString layoutContent;
    QTransform world;
    QTransform emptyWorld;
    QString layoutStatus;
    ViewerTextLayout* worker = nullptr;
    // Set when the Text tool created this layer for this edit. Committing folds the typed text into
    // the creation, so the new layer stays one undo step, and empty text leaves no layer.
    struct Created final {
        document::Revision revision;
        QString name;
        QString placeholder;
        double size = 0.0;
        core::Color4d color;
        document::Vec2d position;
        // The context bar's font and alignment for new text (ADR 0031).
        commands::TextLayerStyle style;
    };
    std::optional<Created> created;
};

} // namespace bloom::ui
