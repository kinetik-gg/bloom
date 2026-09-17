#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QSettings>
#include <QTimer>

namespace bloom::ui {

QString ViewerEditor::analysisSettingsPrefix() const {
    for (const auto* ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget())
        if (const auto* area = qobject_cast<const EditorArea*>(ancestor))
            return QStringLiteral("viewer/analysis/%1/").arg(area->areaId());
    return QStringLiteral("viewer/analysis/default/");
}

void ViewerEditor::loadViewAdjust() {
    const auto prefix = analysisSettingsPrefix();
    const QSettings settings;
    runtime::ViewAdjust saved{settings.value(prefix + "exposure", 0.0).toDouble(),
                              settings.value(prefix + "gamma", 1.0).toDouble()};
    viewAdjust_ = saved.valid() ? saved : runtime::ViewAdjust{};
    exposureField_->setValue(viewAdjust_.exposure);
    gammaField_->setValue(viewAdjust_.gamma);
    refreshViewAdjustment();
}

PreparedPreviewFrameHandle ViewerEditor::displayedFrame() const {
    auto base = previewController_.state().frame;
    if (!base || viewAdjust_.neutral())
        return base;
    auto desired = base->desiredIdentity();
    desired.viewAdjust = viewAdjust_;
    return adjustedFrame_ && adjustedFrame_->desiredIdentity() == desired ? adjustedFrame_ : base;
}

void ViewerEditor::refreshViewAdjustment() {
    if (probePosition_)
        refreshProbe(*probePosition_);
    const auto base = previewController_.state().frame;
    if (previewController_.isShuttingDown() || !base || viewAdjust_.neutral()) {
        if (adjustTask_)
            adjustTask_->cancel();
        adjustedFrame_.reset();
        adjustIdentity_.reset();
        update();
        return;
    }
    auto desired = base->desiredIdentity();
    desired.viewAdjust = viewAdjust_;
    if (adjustTask_) {
        if (adjustIdentity_ != desired)
            adjustTask_->cancel();
        return;
    }
    // Includes a failed attempt: retry on a new frame or a changed adjustment, not every timer
    // tick.
    if (adjustIdentity_ == desired)
        return;
    adjustIdentity_ = desired;
    const auto submission = previewController_.submitViewerAnalysis(desired);
    if (!submission.accepted()) {
        exposureField_->setToolTip(tr("Viewer adjustment could not start"));
        return;
    }
    adjustTask_ = submission.handle;
    exposureField_->setToolTip(tr("Preparing viewer adjustment…"));
    analysisTimer_->start();
}

void ViewerEditor::consumeViewAdjustment() {
    if (!adjustTask_)
        return;
    auto result = adjustTask_->tryTakeResult();
    if (!result)
        return;
    adjustTask_.reset();
    analysisTimer_->stop();
    if (result->state() == runtime::TaskState::Cancelled)
        adjustIdentity_.reset();
    const auto base = previewController_.state().frame;
    const auto& value = result->value();
    if (!previewController_.isShuttingDown() && base && value && *value && (*value)->frame()) {
        auto desired = base->desiredIdentity();
        desired.viewAdjust = viewAdjust_;
        if ((*value)->frame()->desiredIdentity() == desired) {
            adjustedFrame_ = (*value)->frame();
            exposureField_->setToolTip(tr("Exposure in EV stops; viewer display only"));
        }
    } else if (result->state() == runtime::TaskState::Failed) {
        exposureField_->setToolTip(
            result->diagnostics().empty()
                ? tr("Viewer adjustment failed")
                : QString::fromStdString(result->diagnostics().front().summary));
    }
    refreshViewAdjustment();
    if (probePosition_)
        refreshProbe(*probePosition_);
    update();
}

} // namespace bloom::ui
