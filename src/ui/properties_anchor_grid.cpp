#include "properties_anchor_grid.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <chrono>
#include <cmath>
#include <thread>

namespace bloom::ui {
PropertiesAnchorGrid::PropertiesAnchorGrid(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session), timer_(new QTimer(this)) {
    setObjectName("propertiesAnchorGrid");
    setAccessibleName(tr("Anchor Point"));
    setFocusPolicy(Qt::StrongFocus);
    const int pitch = kit::px(kit::Size::PropertiesAnchorDot) + kit::px(kit::Spacing::XS);
    setFixedSize(pitch * 3, pitch * 3);
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, [this] { poll(); });
}
PropertiesAnchorGrid::~PropertiesAnchorGrid() {
    if (scheduler_) {
        task_.cancel();
        scheduler_->beginShutdown();
        retire_->store(true);
        retire_->notify_one();
    }
}
QRect PropertiesAnchorGrid::pointRect(const int index) const {
    const int dot = kit::px(kit::Size::PropertiesAnchorDot);
    const int pitch = dot + kit::px(kit::Spacing::XS);
    return {index % 3 * pitch + kit::px(kit::Spacing::XXS),
            index / 3 * pitch + kit::px(kit::Spacing::XXS), dot, dot};
}
int PropertiesAnchorGrid::selectedPoint() const {
    const auto value = session_.effectiveVec2Value(document::kAnchorParameterRole);
    if (!bounds_ || !value)
        return -1;
    const double halfWidth = (bounds_->right - bounds_->left) / 2.0;
    const double halfHeight = (bounds_->bottom - bounds_->top) / 2.0;
    for (int index = 0; index < 9; ++index) {
        const int row = index / 3;
        if (std::abs(value->x - (index % 3 - 1) * halfWidth) < 1e-7 &&
            std::abs(value->y - static_cast<double>(row - 1) * halfHeight) < 1e-7)
            return index;
    }
    return -1;
}
void PropertiesAnchorGrid::refresh() {
    const auto* direct = std::get_if<document::LayerId>(&session_.selection().primary);
    layer_ = direct ? std::optional(*direct) : session_.selection().contextualLayer;
    const auto time = session_.currentTime();
    const auto identity = QString("%1/%2/%3/%4/%5")
                              .arg(session_.compositionId().value())
                              .arg(session_.snapshot().revision().value())
                              .arg(layer_ ? layer_->value() : 0)
                              .arg(time.numerator())
                              .arg(time.denominator());
    update();
    if (identity == identity_)
        return;
    identity_ = identity;
    ++generation_;
    bounds_.reset();
    setEnabled(false);
    setToolTip(layer_ ? tr("Resolving local bounds…") : tr("Select a layer to set its anchor"));
    if (active_)
        task_.cancel();
    else
        start();
}
void PropertiesAnchorGrid::start() {
    if (!layer_ || !session_.effectiveVec2Value(document::kAnchorParameterRole)) {
        timer_->stop();
        return;
    }
    const auto* layerRecord = session_.composition()->graph().findLayer(*layer_);
    if (layerRecord && layerRecord->locked) {
        timer_->stop();
        setToolTip(tr("The layer is locked"));
        return;
    }
    if (!scheduler_) {
        auto config = runtime::TaskSchedulerConfig::defaults();
        config.cpuWorkerCount = 1;
        config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
        scheduler_ = std::make_shared<runtime::TaskScheduler>(config);
        retire_ = std::make_shared<std::atomic_bool>(false);
        std::thread([scheduler = scheduler_, retire = retire_] {
            retire->wait(false);
            while (!scheduler->isQuiescent())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }).detach();
    }
    const auto snapshot = session_.snapshot();
    const auto compositionId = session_.compositionId();
    const auto time = session_.currentTime();
    const auto layer = *layer_;
    activeGeneration_ = generation_;
    const auto submission = scheduler_->submit<std::shared_ptr<Result>>(
        runtime::TaskRequest{
            "Resolve Anchor Bounds",
            {runtime::TaskOwnerKind::Application, runtime::TaskOwnerId::fromRaw(1)},
            runtime::TaskPriority::Interactive},
        [snapshot, compositionId, time, layer](runtime::TaskContext& context) {
            auto result = std::make_shared<Result>();
            const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
            const auto compiled =
                compiler.compile({snapshot, compositionId}, context.cancellation());
            if (compiled.plan) {
                // Proxy evaluation retains full-resolution local geometry. No display conversion
                // or full-size image is needed for this read-only inspection.
                const auto extent = render::ImageExtent::create(64, 64);
                const runtime::CpuCompositionEvaluator evaluator;
                const auto evaluated =
                    evaluator.evaluate(compiled.plan,
                                       {.time = time,
                                        .output = compiled.plan->output(),
                                        .resolution = runtime::ProxyResolution{*extent.value()},
                                        .pixelStorageByteLimit = std::size_t{64} * 1024U * 1024U},
                                       context.cancellation());
                if (evaluated.frame()) {
                    for (const auto& bounds : evaluated.frame()->evaluatedBounds())
                        if (bounds.layerId == layer && !bounds.local.empty())
                            result->bounds = bounds.local;
                } else if (!evaluated.diagnostics().empty())
                    result->diagnostic =
                        QString::fromStdString(evaluated.diagnostics().front().summary);
            } else if (!compiled.diagnostics.empty())
                result->diagnostic = QString::fromStdString(compiled.diagnostics.front().summary);
            if (context.isCancellationRequested())
                return runtime::TaskResult<std::shared_ptr<Result>>::cancelled();
            return runtime::TaskResult<std::shared_ptr<Result>>::succeeded(std::move(result));
        });
    task_ = submission.handle;
    active_ = submission.accepted();
    if (active_)
        timer_->start();
    else
        setToolTip(tr("Local bounds task could not start"));
}
void PropertiesAnchorGrid::poll() {
    if (const auto result = task_.tryTakeResult()) {
        active_ = false;
        if (activeGeneration_ != generation_) {
            start();
            return;
        }
        timer_->stop();
        QString diagnostic;
        if (result->value()) {
            bounds_ = (**result->value()).bounds;
            diagnostic = (**result->value()).diagnostic;
        }
        const auto* layerRecord = session_.composition() && layer_
                                      ? session_.composition()->graph().findLayer(*layer_)
                                      : nullptr;
        setEnabled(bounds_.has_value() && layerRecord && !layerRecord->locked &&
                   session_.effectiveVec2Value(document::kAnchorParameterRole).has_value());
        setToolTip(bounds_                ? tr("Set anchor to a point of the layer's local bounds")
                   : diagnostic.isEmpty() ? tr("Local bounds are unavailable for this layer")
                                          : diagnostic);
        update();
    }
}
void PropertiesAnchorGrid::choose(const int index) {
    if (!isEnabled() || !bounds_ || index < 0 || index > 8)
        return;
    const double x = (index % 3 - 1) * (bounds_->right - bounds_->left) / 2.0;
    const int row = index / 3;
    const double y = static_cast<double>(row - 1) * (bounds_->bottom - bounds_->top) / 2.0;
    (void)session_.setSelectedAnchor(x, y);
}
void PropertiesAnchorGrid::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        for (int index = 0; index < 9; ++index)
            if (pointRect(index)
                    .adjusted(-kit::px(kit::Spacing::XXS), -kit::px(kit::Spacing::XXS),
                              kit::px(kit::Spacing::XXS), kit::px(kit::Spacing::XXS))
                    .contains(event->pos())) {
                choose(index);
                event->accept();
                return;
            }
    }
    QWidget::mousePressEvent(event);
}
void PropertiesAnchorGrid::keyPressEvent(QKeyEvent* event) {
    const int current = selectedPoint() < 0 ? 4 : selectedPoint();
    switch (event->key()) {
    case Qt::Key_Left:
        choose(current % 3 > 0 ? current - 1 : current);
        break;
    case Qt::Key_Right:
        choose(current % 3 < 2 ? current + 1 : current);
        break;
    case Qt::Key_Up:
        choose(current >= 3 ? current - 3 : current);
        break;
    case Qt::Key_Down:
        choose(current < 6 ? current + 3 : current);
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}
void PropertiesAnchorGrid::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    for (int index = 0; index < 9; ++index)
        kit::fillRoundedSurface(
            painter, pointRect(index),
            kit::color(index == selectedPoint() ? kit::Color::Keyframe : kit::Color::BorderHover),
            hasFocus() && index == selectedPoint() ? kit::color(kit::Color::Accent) : QColor{},
            kit::Radius::Small);
}
} // namespace bloom::ui
