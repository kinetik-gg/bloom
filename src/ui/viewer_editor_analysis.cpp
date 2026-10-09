#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/viewer_gpu_resident.hpp>

#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <QWindow>

namespace bloom::ui {

QString ViewerEditor::analysisSettingsPrefix() const {
    for (const auto* ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget())
        if (const auto* area = qobject_cast<const EditorArea*>(ancestor))
            return QStringLiteral("viewer/analysis/%1/").arg(area->areaId());
    return QStringLiteral("viewer/analysis/default/");
}

QString ViewerEditor::displayViewSettingsKey() const {
    const auto digest =
        session_.colorSettings().ocioConfig.expectedRevision.digest.toLowercaseHex();
    const auto revision = QString::fromLatin1(digest.data(), static_cast<int>(digest.size()));
    for (const auto* ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget())
        if (const auto* area = qobject_cast<const EditorArea*>(ancestor))
            return QStringLiteral("viewer/display-view/%1/%2").arg(revision).arg(area->areaId());
    return QStringLiteral("viewer/display-view/%1/default").arg(revision);
}

QString ViewerEditor::lookSettingsKey() const {
    for (const auto* ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget())
        if (const auto* area = qobject_cast<const EditorArea*>(ancestor))
            return QStringLiteral("viewer/look/%1").arg(area->areaId());
    return QStringLiteral("viewer/look/default");
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

bool ViewerEditor::frameInCurrentDomain(const PreparedPreviewFrameHandle& frame) const {
    return frame && session_.composition() &&
           frame->desiredIdentity().projectId == session_.snapshot().project().id() &&
           frame->desiredIdentity().compositionId == session_.compositionId();
}

PreparedPreviewFrameHandle ViewerEditor::preparedDisplayFrame() const {
    auto base = previewController_.state().frame;
    if (!frameInCurrentDomain(base))
        return nullptr;
    if (viewAdjust_.neutral())
        return base;
    auto desired = base->desiredIdentity();
    desired.viewAdjust = viewAdjust_;
    return adjustedFrame_ && adjustedFrame_->desiredIdentity() == desired ? adjustedFrame_
                                                                          : nullptr;
}

std::optional<ViewerDisplayDescriptor> ViewerEditor::actualDisplayDescriptor() const {
    if (!isVisible())
        return std::nullopt;
    if (const auto* topLevel = window()->windowHandle(); topLevel && !topLevel->isExposed())
        return std::nullopt;
    if (gpuResident_ && gpuResident_->cpuCoverVisible()) {
        if (cpuCoverDisplay_ &&
            (!cpuCoverDisplay_->frame || frameInCurrentDomain(cpuCoverDisplay_->frame)))
            return cpuCoverDisplay_;
        return std::nullopt;
    }
    if (gpuResident_ && gpuResident_->hasLiveTarget()) {
        if (!gpuResident_->nativeDisplayAvailable() || !gpuResident_->appliedDisplay())
            return std::nullopt;
        const auto& applied = *gpuResident_->appliedDisplay();
        if (!frameInCurrentDomain(applied.frame) || !applied.geometry)
            return std::nullopt;
        const auto descriptor = render::ReferenceDisplayBufferDescriptor::create(
            applied.geometry->displayWindow, applied.geometry->pixelAspect);
        if (!descriptor)
            return std::nullopt;
        ViewerDisplayDescriptor result;
        result.frame = applied.frame;
        result.mapping =
            ViewerMapping{applied.destination, applied.frame->processIdentity().plan->format(),
                          applied.frame->desiredIdentity().resolution,
                          applied.geometry->pixelAspect, *descriptor.value()};
        result.captureRect = applied.containerRect;
        result.devicePixelRatio = applied.devicePixelRatio;
        result.channel = static_cast<ViewerChannel>(applied.params.channel);
        result.background = static_cast<ViewerBackground>(applied.params.background);
        result.overlayRevision = applied.domainRevision;
        result.native = true;
        return result;
    }
    if (cpuPaintedDisplay_ && frameInCurrentDomain(cpuPaintedDisplay_->frame) &&
        visibleRegion().intersects(contentRect().toAlignedRect()))
        return cpuPaintedDisplay_;
    return std::nullopt;
}

PreparedPreviewFrameHandle ViewerEditor::displayedFrame() const {
    const auto actual = actualDisplayDescriptor();
    return actual ? actual->frame : nullptr;
}

PreparedPreviewFrameHandle ViewerEditor::appliedDisplayFrame() const { return displayedFrame(); }

CompositionPreviewState ViewerEditor::displayedPreviewState() const {
    auto state = previewController_.state();
    state.viewerDisplayState = true;
    if (state.desiredIdentity)
        state.desiredIdentity->viewAdjust = viewAdjust_;
    const auto actual = actualDisplayDescriptor();
    state.frame = actual ? actual->frame : nullptr;
    QRectF desiredDestination;
    if (state.frame) {
        const auto format = state.frame->processIdentity().plan->format();
        const auto extent = render::ImageExtent::create(format.width(), format.height());
        if (extent)
            desiredDestination = viewTransformedDisplayRect(canvasRect(), *extent.value(),
                                                            format.pixelAspect(), transform_);
    }
    const bool current = state.frame && state.desiredIdentity &&
                         state.frame->desiredIdentity() == *state.desiredIdentity && actual &&
                         actual->channel == channel_ && actual->background == background_ &&
                         actual->overlayRevision == gpuOverlayRevision_ &&
                         actual->captureRect == contentRect() && actual->mapping &&
                         actual->devicePixelRatio == devicePixelRatioF() &&
                         actual->mapping->displayRect == desiredDestination &&
                         (!actual->native || gpuResident_->publicationCurrent());
    state.ownerAppliedDisplay = actual && actual->native;
    state.freshness = !state.frame ? FrameFreshness::None
                      : current    ? FrameFreshness::Current
                                   : FrameFreshness::Stale;
    const bool sharedFailure =
        state.activity == PreviewActivity::Failed || state.activity == PreviewActivity::Unsupported;
    if (!session_.composition() || sharedFailure) {
        // Shared pipeline/configuration diagnostics remain authoritative, including when a
        // local adjustment has no candidate. Display freshness is still projected separately.
    } else if (!adjustmentFailure_.isEmpty() || !nativeFailure_.isEmpty() ||
               !channelFailure_.isEmpty()) {
        state.activity = PreviewActivity::Failed;
        state.message = !adjustmentFailure_.isEmpty() ? adjustmentFailure_
                        : !nativeFailure_.isEmpty()   ? nativeFailure_
                                                      : channelFailure_;
    } else if (channelTask_ && actual &&
               (actual->channel != channel_ ||
                !sameViewerPackedSource(actual->frame, channelWantedFrame_))) {
        state.activity = PreviewActivity::Rendering;
        state.message = tr("Preparing Viewer channel; previous complete display retained");
    } else if (!viewAdjust_.neutral() && !preparedDisplayFrame() &&
               (adjustTask_ || frameInCurrentDomain(previewController_.state().frame))) {
        state.activity = PreviewActivity::Rendering;
        state.message = tr("Preparing viewer adjustment; previous display intent retained");
    } else if (!state.frame && state.activity == PreviewActivity::Ready) {
        state.activity = PreviewActivity::Rendering;
        state.message = tr("Prepared frame awaiting viewer presentation");
    } else if (state.frame && !current) {
        state.message = tr("Previous complete display is retained (out of date)");
    }
    if (state.frame && actual) {
        const auto shown = state.frame->desiredIdentity().viewAdjust;
        const QStringList channels{tr("RGBA"),  tr("RGB"),  tr("Red"),
                                   tr("Green"), tr("Blue"), tr("Alpha")};
        const QStringList backgrounds{tr("Solid"), tr("Checkerboard"), tr("Black"), tr("White")};
        state.viewerStatusText =
            state.activity == PreviewActivity::Failed ||
                    state.activity == PreviewActivity::Unsupported
                ? state.message
                : tr("Shown EV %1%2 · %3%4 · %5")
                      .arg(shown.exposure, 0, 'g', 5)
                      .arg(shown.gamma == 1.0 ? QString{}
                                              : tr(", gamma %1").arg(shown.gamma, 0, 'g', 5))
                      .arg(channels.value(static_cast<int>(actual->channel)))
                      .arg(actual->cover ? tr(" · CPU cover") : QString{})
                      .arg(current ? tr("Current") : tr("Previous display"));
        state.message = tr("Shown EV %1, gamma %2 · %3 · %4%5 · %6")
                            .arg(shown.exposure, 0, 'g', 5)
                            .arg(shown.gamma, 0, 'g', 5)
                            .arg(channels.value(static_cast<int>(actual->channel)))
                            .arg(backgrounds.value(static_cast<int>(actual->background)))
                            .arg(actual->cover ? tr(" · CPU cover") : QString{})
                            .arg(state.message);
        if (!actual->native && actual->channel != ViewerChannel::Rgba) {
            state.viewerStatusText += tr(" · CPU channel presentation");
            state.message += tr(" · CPU channel presentation");
        }
    }
    if (state.viewerStatusText.isEmpty())
        state.viewerStatusText = state.message;
    return state;
}

void ViewerEditor::refreshViewAdjustment() {
    if (probePosition_)
        refreshProbe(*probePosition_);
    const auto base = previewController_.state().frame;
    if (previewController_.isShuttingDown() || !frameInCurrentDomain(base) ||
        viewAdjust_.neutral()) {
        if (adjustTask_)
            adjustTask_->cancel();
        adjustedFrame_.reset();
        adjustIdentity_.reset();
        adjustmentFailure_.clear();
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
    adjustmentFailure_.clear();
    const auto submission =
        base->provenance().provider == runtime::PreviewDisplayProvider::GpuPreparedPacked
            ? previewController_.submitViewerDisplay(desired)
            : previewController_.submitViewerAnalysis(desired);
    if (!submission.accepted()) {
        adjustmentFailure_ = submission.diagnostic && !submission.diagnostic->summary.empty()
                                 ? QString::fromStdString(submission.diagnostic->summary)
                                 : tr("Viewer adjustment could not start for the requested frame");
        exposureField_->setToolTip(adjustmentFailure_);
        updatePreviewAccessibility();
        return;
    }
    adjustTask_ = submission.handle;
    exposureField_->setToolTip(tr("Preparing viewer adjustment…"));
}

void ViewerEditor::consumeViewAdjustment() {
    if (!adjustTask_)
        return;
    auto result = adjustTask_->tryTakeResult();
    if (!result)
        return;
    adjustTask_.reset();
    if (result->state() == runtime::TaskState::Cancelled)
        adjustIdentity_.reset();
    const auto base = previewController_.state().frame;
    const auto& value = result->value();
    if (!previewController_.isShuttingDown() && base && value && *value && (*value)->frame()) {
        auto desired = base->desiredIdentity();
        desired.viewAdjust = viewAdjust_;
        if ((*value)->frame()->desiredIdentity() == desired) {
            adjustedFrame_ = (*value)->frame();
            adjustmentFailure_.clear();
            exposureField_->setToolTip(tr("Exposure in EV stops; viewer display only"));
        }
    } else if (result->state() != runtime::TaskState::Cancelled) {
        adjustmentFailure_ = result->diagnostics().empty()
                                 ? tr("Viewer adjustment returned no supported display frame")
                                 : QString::fromStdString(result->diagnostics().front().summary);
        exposureField_->setToolTip(adjustmentFailure_);
    }
    refreshViewAdjustment();
    updatePreviewAccessibility();
    updateGpuResidentPresentation();
    if (probePosition_)
        refreshProbe(*probePosition_);
    update();
}

} // namespace bloom::ui
