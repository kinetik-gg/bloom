#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/cryptomatte_controller.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/viewer_gpu_resident.hpp>
#include <bloom/ui/viewer_gpu_resident_overlay.hpp>
#include <memory>

#include "composition_editor_support.hpp"
#include "layer_drop.hpp"
#include "layer_mask_support.hpp"
#include "text_font_choices.hpp"
#include "viewer_editor_internal.hpp"
#include "viewer_editor_text_layout.hpp"
#include "viewer_timecode_readout.hpp"
#include "viewer_tool_geometry.hpp"

#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/render/image_types.hpp>

#include <QAbstractItemModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QInputMethodEvent>
#include <QIntValidator>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QRegion>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <QWindow>
#include <bloom/render/path_raster.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <numbers>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::ui {
using namespace viewer_detail;
namespace {
std::vector<document::LayerId>
pointTextLayers(const CompositionSession& session,
                std::span<const runtime::EvaluatedOperationBounds> bounds) {
    std::vector<document::LayerId> result;
    const auto* composition = session.composition();
    if (!composition)
        return result;
    for (const auto& bound : bounds) {
        const auto sourceId = session.directSourceNodeForLayer(bound.layerId);
        const auto* source = sourceId ? composition->graph().findNode(*sourceId) : nullptr;
        if (!source || source->typeId != document::kTextSourceNodeType)
            continue;
        const auto box = std::ranges::find(source->parameters, document::kTextBoxParameterRole,
                                           &document::ParameterBinding::role);
        const auto value =
            box != source->parameters.end() ? session.liveValue(box->parameterId) : std::nullopt;
        const auto* size = value ? std::get_if<document::Vec2d>(&*value) : nullptr;
        if (!size || size->x == 0 || size->y == 0)
            result.push_back(bound.layerId);
    }
    return result;
}

// Keyboard steps use current authored ancestor linear transforms, so auto-repeat never waits
// for the previous nudge's preview. Pivot translations do not affect displacement vectors.
void nudgeViewerSelection(CompositionSession& session, const QPointF compositionDelta) {
    const auto* layer = std::get_if<document::LayerId>(&session.selection().primary);
    const auto position = session.effectiveVec2Value(document::kPositionParameterRole);
    if (!layer || !position || !session.composition())
        return;
    QTransform parentWorld;
    for (auto parent = session.parentOf(*layer); parent; parent = session.parentOf(*parent)) {
        const auto boundary = session.boundaryNodeForLayer(*parent);
        const auto* node = boundary ? session.composition()->graph().findNode(*boundary) : nullptr;
        if (!node)
            return;
        const auto scaleBinding = std::ranges::find(node->parameters, document::kScaleParameterRole,
                                                    &document::ParameterBinding::role);
        const auto rotationBinding = std::ranges::find(
            node->parameters, document::kRotationParameterRole, &document::ParameterBinding::role);
        if (scaleBinding == node->parameters.end() || rotationBinding == node->parameters.end())
            return;
        const auto scale = session.effectiveVec2Value(scaleBinding->parameterId);
        const auto rotation = session.effectiveScalarValue(rotationBinding->parameterId);
        if (!scale || !rotation)
            return;
        QTransform local;
        local.rotate(*rotation);
        local.scale(scale->x, scale->y);
        parentWorld *= local;
    }
    bool invertible = false;
    const auto inverse = parentWorld.inverted(&invertible);
    if (!invertible)
        return;
    const auto delta = inverse.map(compositionDelta);
    (void)session.setSelectedPosition(position->x + delta.x(), position->y + delta.y());
}

} // namespace

QRectF fitDisplayRect(const QRectF& available, const render::ImageExtent extent,
                      const core::PixelAspectRatio pixelAspect) noexcept {
    if (available.isEmpty()) {
        return {};
    }

    const long double displayWidth = static_cast<long double>(extent.width()) *
                                     pixelAspect.numerator() / pixelAspect.denominator();
    const long double displayHeight = extent.height();
    if (displayWidth <= 0.0L || displayHeight <= 0.0L) {
        return {};
    }

    const long double scale =
        std::min(static_cast<long double>(available.width()) / displayWidth,
                 static_cast<long double>(available.height()) / displayHeight);
    const qreal fittedWidth = static_cast<qreal>(displayWidth * scale);
    const qreal fittedHeight = static_cast<qreal>(displayHeight * scale);
    return QRectF(available.center().x() - fittedWidth / 2.0,
                  available.center().y() - fittedHeight / 2.0, fittedWidth, fittedHeight);
}

QRectF actualPixelRect(const QRectF& available, const render::ImageExtent extent,
                       const core::PixelAspectRatio pixelAspect) noexcept {
    if (available.isEmpty()) {
        return {};
    }

    const long double displayWidth = static_cast<long double>(extent.width()) *
                                     pixelAspect.numerator() / pixelAspect.denominator();
    const long double displayHeight = extent.height();
    if (displayWidth <= 0.0L || displayHeight <= 0.0L) {
        return {};
    }

    const auto width = static_cast<qreal>(displayWidth);
    const auto height = static_cast<qreal>(displayHeight);
    return QRectF(available.center().x() - width / 2.0, available.center().y() - height / 2.0,
                  width, height);
}

QRectF viewTransformedDisplayRect(const QRectF& available, const render::ImageExtent extent,
                                  const core::PixelAspectRatio pixelAspect,
                                  const ViewTransform& transform) noexcept {
    if (transform.fitToWindow) {
        return fitDisplayRect(available, extent, pixelAspect);
    }
    const QRectF actual = actualPixelRect(available, extent, pixelAspect);
    if (actual.isEmpty()) {
        return {};
    }
    const qreal zoom = std::clamp(transform.zoom, ViewTransform::kMinZoom, ViewTransform::kMaxZoom);
    const qreal width = actual.width() * zoom;
    const qreal height = actual.height() * zoom;
    const QPointF centeredTopLeft(available.center().x() - width / 2.0,
                                  available.center().y() - height / 2.0);
    return QRectF(centeredTopLeft + transform.pan, QSizeF(width, height));
}

bool cpuFallbackCompletionIsCurrent(const runtime::PreviewRequestIdentity& completed,
                                    const runtime::PreviewRequestIdentity& current) {
    return completed == current;
}

ViewTransform zoomAboutPoint(const ViewTransform& transform, const QRectF& available,
                             const render::ImageExtent extent,
                             const core::PixelAspectRatio pixelAspect, const QPointF screenPoint,
                             const double factor) noexcept {
    const QRectF actual = actualPixelRect(available, extent, pixelAspect);
    const QRectF current = viewTransformedDisplayRect(available, extent, pixelAspect, transform);
    if (actual.isEmpty() || current.isEmpty() || !(actual.width() > 0.0) ||
        !(actual.height() > 0.0) || !(factor > 0.0)) {
        return transform;
    }

    const qreal currentZoom = current.width() / actual.width();
    const qreal newZoom =
        std::clamp(currentZoom * factor, ViewTransform::kMinZoom, ViewTransform::kMaxZoom);
    // Fraction of the CURRENT display rectangle the cursor sits at -- fixed across the step by
    // construction below (the zoom-about-cursor invariant this function exists to guarantee).
    const qreal fractionX = (screenPoint.x() - current.left()) / current.width();
    const qreal fractionY = (screenPoint.y() - current.top()) / current.height();
    const qreal newWidth = actual.width() * newZoom;
    const qreal newHeight = actual.height() * newZoom;
    const QPointF newTopLeft(screenPoint.x() - fractionX * newWidth,
                             screenPoint.y() - fractionY * newHeight);
    const QPointF centeredTopLeft(available.center().x() - newWidth / 2.0,
                                  available.center().y() - newHeight / 2.0);
    return ViewTransform{
        .fitToWindow = false, .zoom = newZoom, .pan = newTopLeft - centeredTopLeft};
}

double ViewerEditor::effectiveZoom(const QRectF& displayRect,
                                   const DisplayGeometry& geometry) const {
    const QRectF actual = actualPixelRect(canvasRect(), geometry.extent, geometry.pixelAspect);
    return actual.width() > 0.0 ? displayRect.width() / actual.width() : 1.0;
}

void ViewerEditor::zoomInAtCenter() {
    if (const auto geometry = currentDisplayGeometry(); geometry.has_value()) {
        const QRectF frame = canvasRect();
        transform_ = zoomAboutPoint(transform_, frame, geometry->extent, geometry->pixelAspect,
                                    frame.center(), kZoomStepFactor);
        refreshZoomDropdown();
        update();
    }
}

void ViewerEditor::zoomOutAtCenter() {
    if (const auto geometry = currentDisplayGeometry(); geometry.has_value()) {
        const QRectF frame = canvasRect();
        transform_ = zoomAboutPoint(transform_, frame, geometry->extent, geometry->pixelAspect,
                                    frame.center(), 1.0 / kZoomStepFactor);
        refreshZoomDropdown();
        update();
    }
}

ViewerEditor::ViewerEditor(CompositionSession& session,
                           CompositionPreviewController& previewController,
                           RamPreviewController* const ramPreview, QWidget* parent)
    : QWidget(parent), session_(session), previewController_(previewController),
      ramPreview_(ramPreview) {
    setObjectName("viewerEditor");
    setAccessibleName(tr("Composition viewer"));
    // Assets items dropped on the Viewer become layers at the playhead, centred (#380).
    installLayerDropTarget(*this, session_);
    // ADR 0031: every Viewer follows the one app-wide tool.
    connect(&session_.tools(), &ToolController::toolChanged, this, &ViewerEditor::toolChanged);

    // The canvas floor. Hosted, the panel adds its header and footer to this
    // (EditorChromeSpec::contentMinimumHeight); unhosted, the footer strip is inside it.
    setMinimumSize(kit::px(kit::Size::ViewerMinWidth), kit::px(kit::Size::ViewerMinHeight));
    chrome_.contentMinimumHeight = kit::px(kit::Size::ViewerMinHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // StrongFocus lets a press-to-drag gesture also receive the Escape key that confirms it, and
    // lets the widget receive Space/Z/F without a prior click.
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    connect(this, &ViewerEditor::probeChanged, &previewController_,
            &CompositionPreviewController::probeChanged);
    connect(&previewController_, &CompositionPreviewController::stateChanged, this, [this] {
        if (probePosition_)
            refreshProbe(*probePosition_);
    });

    connect(&session_, &CompositionSession::snapshotChanged, this, [this] { cancelCreation(); });
    connect(&session_, &CompositionSession::compositionChanged, this, [this] {
        cancelCreation();
        roiRect_.reset();
        if (roiAction_)
            roiAction_->setChecked(false);
        publishRoi();
    });
    connect(&session_, &CompositionSession::currentTimeChanged, this, [this] { cancelCreation(); });
    connect(&session_, &CompositionSession::selectionChanged, this, [this] {
        cancelAnchorGesture();
        selectedAnchors_.clear();
        notifyAnchorModeState();
        update();
    });
    connect(&session_, &CompositionSession::maskSelectionChanged, this, [this] {
        cancelAnchorGesture();
        selectedAnchors_.clear();
        notifyAnchorModeState();
        update();
    });
    // The anchor mode strip follows the edited path: edits, keyed time and late layer bounds.
    connect(&session_, &CompositionSession::snapshotChanged, this,
            [this] { notifyAnchorModeState(); });
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            [this] { notifyAnchorModeState(); });
    connect(&previewController_, &CompositionPreviewController::stateChanged, this,
            [this] { notifyAnchorModeState(); });
    connect(&session_, &CompositionSession::liveValueChanged, this, [this] {
        if (textEdit_ && !session_.isValueEditing(textEdit_->parameter))
            finishTextEditing(false);
        update();
    });
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        // Color readiness also refreshes projections without changing the document revision.
        // The session invalidates its value edit before publishing an actual revision change.
        if (textEdit_ && !session_.isValueEditing(textEdit_->parameter))
            finishTextEditing(false);
    });
    connect(&session_, &CompositionSession::compositionChanged, this,
            [this] { finishTextEditing(false); });
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            [this] { finishTextEditing(false); });
    connect(&session_, &CompositionSession::selectionChanged, this,
            [this] { finishTextEditing(true); });
    playback_ = &previewController.playbackController();
    // The footer first: the menus the header offers mirror its controls.
    buildFooter(ramPreview);
    buildHeader();
    buildEmptyState();
    wireTransport();
    connect(&session_, &CompositionSession::colorSettingsChanged, this,
            [this] { rebuildDisplayViewControl(); });
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] { updateLookControl(); });
    connect(&session_, &CompositionSession::compositionChanged, this,
            [this] { updateLookControl(); });
    // The composition background is presentation only (its command is not render-affecting): an
    // edit re-presents the frame it already has -- a CPU repaint, or a native present with the new
    // frame colour -- and never asks for a new evaluation.
    presentedFrameColor_ = frameBackgroundColor().rgba();
    const auto refreshFrameColor = [this] {
        const QRgb color = frameBackgroundColor().rgba();
        if (color == presentedFrameColor_)
            return;
        presentedFrameColor_ = color;
        updateGpuResidentPresentation();
        update();
    };
    connect(&session_, &CompositionSession::snapshotChanged, this, refreshFrameColor);
    connect(&session_, &CompositionSession::compositionChanged, this, refreshFrameColor);

    // Every one of these already repainted the status bar for free when it was part of this

    // out into its own widget by also nudging statusBarFooter_ (a no-op update() call until then,
    // since it starts null).
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        rebuildCompositionSelector();
        rebuildObjectSelector();
        updateCompositionActions();
        updateOverlayActions();
        updateEmptyState();
        updatePreviewResolution();
        update();
        if (statusBarFooter_ != nullptr) {
            statusBarFooter_->update();
        }
    });
    connect(&session_, &CompositionSession::compositionChanged, this, [this] {
        rebuildCompositionSelector();
        rebuildObjectSelector();
        updateCompositionActions();
        updateOverlayActions();
        updateEmptyState();
        updatePreviewResolution();
        update();
        if (statusBarFooter_ != nullptr) {
            statusBarFooter_->update();
        }
    });
    connect(&session_, &CompositionSession::selectionChanged, this, [this] {
        rebuildObjectSelector();
        ++gpuOverlayRevision_;
        updateGpuResidentPresentation();
        update();
        if (statusBarFooter_ != nullptr) {
            statusBarFooter_->update();
        }
    });
    // New for the status bar's exact readout (decision 3): the original Viewer never needed
    // current-time updates before, since nothing it drew depended on session time.
    connect(&session_, &CompositionSession::currentTimeChanged, this, [this] {
        timeReadout_->refresh();
        update();
        if (statusBarFooter_ != nullptr) {
            statusBarFooter_->update();
        }
    });
    // The readout also follows a composition switch, which resets session time to exact zero
    // (docs/architecture/animation-and-time.md, "Session Time And Scrubbing") -- so it never shows
    // the previous composition's stale frame for one repaint.
    connect(&session_, &CompositionSession::compositionChanged, this,
            [this] { timeReadout_->refresh(); });
    // Deleting the last composition changes the snapshot, not necessarily the composition id.
    connect(&session_, &CompositionSession::snapshotChanged, this,
            [this] { timeReadout_->refresh(); });
    // Task S5, item 3b: the dropped-frame readout follows the SAME refresh idiom as every other
    // footer element -- connect to whatever changes it, then update() this widget and the footer.
    connect(&previewController_, &CompositionPreviewController::droppedFrameCountChanged, this,
            [this] {
                update();
                if (statusBarFooter_ != nullptr) {
                    statusBarFooter_->update();
                }
            });
    // Task PERF1, item 3: the RAM preview progress readout, through that same idiom.
    connect(&previewController_, &CompositionPreviewController::ramPreviewProgressChanged, this,
            [this] {
                update();
                if (statusBarFooter_ != nullptr) {
                    statusBarFooter_->update();
                }
            });
    connect(&previewController_, &CompositionPreviewController::stateChanged, this, [this] {
        updatePreviewAccessibility();
        resolutionReadout_->setText(viewerResolutionText(previewController_));
        resolutionDropdown_->setToolTip(viewerResolutionText(previewController_));
        // A newly delivered frame may carry a format/proxy/pixel-aspect/display-descriptor change
        // (docs/architecture/animation-and-time.md); a mid-drag mapping change cancels the gesture
        // rather than silently mis-mapping the rest of it.
        if (dragActive_ && !mappingStillValid()) {
            endDrag(false);
        }
        update();
        if (statusBarFooter_ != nullptr) {
            statusBarFooter_->update();
        }
    });
    connect(&previewController_, &CompositionPreviewController::stateChanged, this,
            &ViewerEditor::refreshViewAdjustment);
    connect(&previewController_, &CompositionPreviewController::viewerResultsAvailable, this,
            &ViewerEditor::consumeChannelDisplay);
    connect(&previewController_, &CompositionPreviewController::viewerResultsAvailable, this,
            &ViewerEditor::consumeChannelListing);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &ViewerEditor::refreshChannelInspectionChoices);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &ViewerEditor::refreshCryptomattePreview);
    if (auto* cryptomatte = session_.cryptomatteController()) {
        connect(cryptomatte, &CryptomatteController::changed, this,
                &ViewerEditor::refreshCryptomattePreview);
        connect(cryptomatte, &CryptomatteController::pickRequested, this,
                [this] { session_.tools().setTool(ViewerTool::Cryptomatte); });
    }
    refreshCryptomattePreview();
    connect(&session_, &CompositionSession::snapshotChanged, this,
            &ViewerEditor::refreshChannelInspectionChoices);
    connect(&previewController_, &CompositionPreviewController::viewerResultsAvailable, this,
            &ViewerEditor::consumeViewAdjustment);
    connect(&previewController_, &CompositionPreviewController::viewerResultsAvailable, this,
            &ViewerEditor::consumeProbe);
    connect(&previewController_, &CompositionPreviewController::viewerResultsAvailable, this,
            &ViewerEditor::pollResidentCpuFallback);
    connect(&session_, &CompositionSession::documentRebound, this, [this] {
        resetChannelDisplay();
        // Different document owners may have colliding numeric project/composition/revision IDs.
        // Detach cancelled per-view work and every local retained display before re-evaluation;
        // no late completion can become a candidate for the newly bound document.
        if (adjustTask_)
            adjustTask_->cancel();
        adjustTask_.reset();
        adjustIdentity_.reset();
        adjustedFrame_.reset();
        adjustmentFailure_.clear();
        if (cpuFallbackTask_)
            cpuFallbackTask_->cancel();
        cpuFallbackTask_.reset();
        cpuFallbackIdentity_.reset();
        cpuFallbackFailedIdentity_.reset();
        cpuFallbackFrame_.reset();
        lastCpuFrame_.reset();
        gpuRequestedFrame_.reset();
        cpuPaintedDisplay_.reset();
        cpuCoverDisplay_.reset();
        coverCaptureDisplay_.reset();
        clearProbe();
        probeTask_.reset();
        probeIdentity_.reset();
        probeTaskFrame_.reset();
        probeCacheFrame_.reset();
        probeCacheReference_.reset();
        probeCacheDisplayLinear_.reset();
        probeCacheDisplay_.reset();
        probeFailure_.clear();
        ++gpuOverlayRevision_;
        gpuPublicationDomain_.reset();
        updateGpuResidentPresentation();
        updatePreviewAccessibility();
        update();
    });
    updatePreviewResolution();
    updatePreviewAccessibility();
    updateOverlayActions();

    // GPU-resident presentation is inert until setGpuPresentationDependencies() supplies a live
    // client. It never creates a device/thread here; the adapter owns the only native objects.
    gpuResident_ = std::make_unique<ViewerGpuResidentController>();
    gpuResident_->setFrameTrace(previewController_.frameTrace());
    gpuResident_->setPresentAck([this](const bool presented, const std::string& diagnostic) {
        // True only for an owner-applied coherent pair; its exact frame may remain stale.
        residentActive_ = presented || gpuResident_->canRetainAppliedDisplay();
        if (!presented)
            nativeFailure_ = QString::fromStdString(diagnostic);
        else if (gpuResident_->publicationCurrent())
            nativeFailure_.clear();
        updatePreviewAccessibility();
        update();
    });
    gpuResident_->setCpuFallback([this] {
        // This bounded failure notification also covers asynchronous overlay/admission failures,
        // which have no present sequence and therefore must not manufacture a failed owner ack.
        nativeFailure_ = QString::fromStdString(gpuResident_->diagnostic());
        residentActive_ = gpuResident_->canRetainAppliedDisplay();
        updatePreviewAccessibility();
        requestResidentCpuFallback();
    });
    gpuResident_->setCpuCoverSnapshot([this]() -> QPixmap { return renderCpuCoverSnapshot(); });
    gpuResident_->setInputSink(
        [this](const ViewerGpuInputEvent& event) { forwardGpuInput(event); });
    gpuResident_->setStateChanged([this] { updateGpuResidentPresentation(); });
    gpuResidentTimer_ = new QTimer(this);
    gpuResidentTimer_->setInterval(16);
    connect(gpuResidentTimer_, &QTimer::timeout, this, &ViewerEditor::pollGpuResident);
}

ViewerEditor::~ViewerEditor() {
    resetChannelDisplay();
    if (gpuResidentTimer_ != nullptr) {
        gpuResidentTimer_->stop();
    }
    if (cpuFallbackTask_.has_value()) {
        cpuFallbackTask_->cancel();
        cpuFallbackTask_.reset();
    }
    // The host retire-before-mutation gate must have already settled any live native target. A
    // target still live here is the ownership-contract violation ViewerGpuPresenter documents.
    gpuResident_.reset();
    gpuContainer_ = nullptr;
    clearProbe();
    if (adjustTask_)
        adjustTask_->cancel();
    finishTextEditing(true);
    cancelCreation();
    QObject::disconnect(focusConnection_);
}

// Everything the transport needs that is not the construction of its buttons: the shared
// PlaybackController, the RAM preview command, the four frame-stepping QActions, and the
// arrow-key reconciliation that keeps those actions from swallowing another panel's navigation.
void ViewerEditor::wireTransport() {
    connect(playPauseButton_, &QToolButton::clicked, playback_, &PlaybackController::toggle);
    connect(playback_, &PlaybackController::stateChanged, this,
            &ViewerEditor::updatePlaybackButton);
    updatePlaybackButton(playback_->state());

    // Looping is a persisted transport preference, read once here and written on every toggle.
    // PlaybackController itself stays free of QSettings: it owns the behavior, not the preference.
    playback_->setLooping(QSettings().value(kLoopSetting, true).toBool());
    connect(loopButton_, &QToolButton::clicked, this, [this](const bool checked) {
        playback_->setLooping(checked);
        QSettings().setValue(kLoopSetting, checked);
    });
    connect(playback_, &PlaybackController::loopingChanged, this, &ViewerEditor::updateLoopButton);
    updateLoopButton();

    // RAM Preview's KEYS are not declared here. Ctrl+Shift+Space and the Escape that cancels a run
    // are application-wide commands owned by the Composition menu (main_window.cpp): one
    // Qt::WindowShortcut owner per sequence, or Qt reports an ambiguous overload and fires neither.
    // This button is the transport's own affordance for that same command, and it calls the same
    // RamPreviewController::toggle() the menu item calls -- never a synthesized key press.
    if (ramPreview_ != nullptr) {
        connect(ramPreviewButton_, &QAbstractButton::clicked, ramPreview_,
                &RamPreviewController::toggle);
        connect(ramPreview_, &RamPreviewController::stateChanged, this, [this] {
            updateRamPreviewButton();
            updatePlaybackButton(playback_->state());
        });
        updateRamPreviewButton();
    }

    // The four frame-stepping actions are created with the footer (buildTransportActions()).
    connect(stepBackwardAction_, &QAction::triggered, this, [this] { stepFrame(-1); });
    // The visible button triggers this SAME action -- one behavior, two entry points. Wired through
    // trigger() rather than QToolButton::setDefaultAction() so the button's own icon, tooltip and
    // objectName stay under this class's control instead of mirroring the action's text.
    connect(stepBackButton_, &QToolButton::clicked, stepBackwardAction_, &QAction::trigger);

    connect(stepForwardAction_, &QAction::triggered, this, [this] { stepFrame(1); });
    connect(stepForwardButton_, &QToolButton::clicked, stepForwardAction_, &QAction::trigger);

    connect(stepToStartAction_, &QAction::triggered, this, &ViewerEditor::stepToStart);
    connect(stepToStartButton_, &QToolButton::clicked, stepToStartAction_, &QAction::trigger);

    connect(stepToEndAction_, &QAction::triggered, this, &ViewerEditor::stepToEnd);
    connect(stepToEndButton_, &QToolButton::clicked, stepToEndAction_, &QAction::trigger);

    // Arrow-key conflict reconciliation, carried over from TimelineEditor, which owned these four
    // actions before task VIEW-1 moved the transport here. The timeline's layer stack consumes
    // Up/Down/Home/End for its OWN row navigation but, unlike a text-entry widget, does not claim
    // the ShortcutOverride event for them, so a same-key WindowShortcut action would silently
    // swallow that navigation. The frozen rule -- widget focus wins, the step action fires
    // otherwise -- is implemented by disabling these four actions outright while such a widget
    // holds keyboard focus: a disabled QAction never claims ShortcutOverride, so the key event
    // reaches the widget and its navigation runs unchanged.
    //
    // The Viewer must not know which panels exist, so the marker is a dynamic property
    // (kDefersTransportKeysProperty) the claiming widget sets on itself rather than a type check.
    focusConnection_ =
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
            bool deferred = false;
            for (const QWidget* candidate = now; candidate != nullptr;
                 candidate = candidate->parentWidget()) {
                if (candidate->property(kDefersTransportKeysProperty).toBool()) {
                    deferred = true;
                    break;
                }
            }
            transportKeysDeferred_ = deferred;
            applyTransportAvailability();
        });
    transportHasComposition_ = session_.composition() != nullptr;
    applyTransportAvailability();
}

void ViewerEditor::applyTransportAvailability() {
    const bool hasComposition = transportHasComposition_;
    const bool stepsEnabled = hasComposition && !transportKeysDeferred_;
    for (auto* action :
         {stepBackwardAction_, stepForwardAction_, stepToStartAction_, stepToEndAction_}) {
        if (action != nullptr)
            action->setEnabled(stepsEnabled);
    }
    // The visible buttons mirror their action's enabled state exactly, so the mouse affordance
    // never shows a clickable button that would silently do nothing.
    const auto apply = [hasComposition](QToolButton* button, const bool enabled) {
        if (button == nullptr)
            return;
        constexpr const char* kBase = "bloomTransportBaseToolTip";
        const QString unavailable =
            QCoreApplication::translate("ViewerEditor", " (unavailable: no composition is open)");
        const QString current = button->toolTip();
        if (!hasComposition) {
            if (!current.endsWith(unavailable)) {
                button->setProperty(kBase, current);
                button->setToolTip(current + unavailable);
            }
        } else if (current.endsWith(unavailable)) {
            button->setToolTip(button->property(kBase).toString());
        }
        button->setEnabled(enabled);
    };
    apply(stepBackButton_, stepsEnabled);
    apply(stepForwardButton_, stepsEnabled);
    apply(stepToStartButton_, stepsEnabled);
    apply(stepToEndButton_, stepsEnabled);
    apply(playPauseButton_, hasComposition);
    apply(loopButton_, hasComposition);
}

void ViewerEditor::seekToFrame(const std::uint64_t frameIndex) {
    const auto context = frameContextFor(session_);
    if (!context.has_value()) {
        return;
    }
    // Stepping while playing pauses playback FIRST through PlaybackController's own public
    // transport API (design decision 1) -- composing with pause() explicitly here rather than
    // relying on handleCurrentTimeChanged()'s existing "any external setCurrentTime() while playing
    // pauses" side effect, so this call site is honest about what it does and the transport state
    // change is never a coincidental side effect of the time write below. Called unconditionally
    // (idempotent no-op if already Stopped), not only when the seek actually moves the playhead.
    playback_->pause();
    const auto targetTime = frameTimeForIndex(context->frameRate, context->duration,
                                              std::min(frameIndex, context->maxFrameIndexValue));
    if (targetTime.has_value()) {
        // A landing on the CURRENT exact time is a true no-op through
        // CompositionSession::setCurrentTime()'s own early-return-on-equal-time guard -- no
        // currentTimeChanged signal churn.
        (void)session_.setCurrentTime(*targetTime);
    }
}

void ViewerEditor::stepFrame(const int delta) {
    const auto context = frameContextFor(session_);
    if (!context.has_value()) {
        return;
    }
    // Left/Right move exactly one frame index from the nearest index to the CURRENT (possibly
    // subframe) time, clamped to [0, maxFrameIndex] (design decision 1). nearestFrameIndex()'s own
    // tie rule decides which frame a subframe time steps from, not this call site.
    const auto nearest =
        nearestFrameIndexForTime(context->frameRate, context->duration, session_.currentTime());
    if (!nearest.has_value()) {
        return;
    }
    std::uint64_t target = *nearest;
    if (delta < 0) {
        target = target > 0 ? target - 1 : 0;
    } else {
        target = target < context->maxFrameIndexValue ? target + 1 : context->maxFrameIndexValue;
    }
    seekToFrame(target);
}

void ViewerEditor::stepToStart() { seekToFrame(0); }

void ViewerEditor::stepToEnd() {
    const auto context = frameContextFor(session_);
    if (context.has_value()) {
        seekToFrame(context->maxFrameIndexValue);
    }
}

void ViewerEditor::setChannel(const ViewerChannel channel) {
    if (channel_ == channel) {
        return;
    }
    channel_ = channel;
    // Keep at most one complete older channel while the newest worker derivation is pending.
    if (channelDropdown_ != nullptr &&
        channelDropdown_->currentIndex() != static_cast<int>(channel)) {
        const QSignalBlocker blocker(channelDropdown_);
        channelDropdown_->setCurrentIndex(static_cast<int>(channel));
    }
    // The dropdown changed silently; the View > Channel menu re-reads it.
    if (channelMenu_ != nullptr)
        Q_EMIT channelMenu_->aboutToShow();
    updateGpuResidentPresentation();
    update();
}

void ViewerEditor::setBackground(const ViewerBackground background) {
    if (background_ == background) {
        return;
    }
    background_ = background;
    QSettings().setValue(
        kBackgroundSetting,
        QString::fromLatin1(kBackgroundNames[static_cast<std::size_t>(background)]));
    if (backgroundDropdown_ != nullptr &&
        backgroundDropdown_->currentIndex() != static_cast<int>(background)) {
        const QSignalBlocker blocker(backgroundDropdown_);
        backgroundDropdown_->setCurrentIndex(static_cast<int>(background));
    }
    if (backgroundMenu_ != nullptr)
        Q_EMIT backgroundMenu_->aboutToShow();
    updateGpuResidentPresentation();
    update();
}

void ViewerEditor::setTransparencyGrid(const bool enabled) {
    if (transparencyGrid_ == enabled)
        return;
    transparencyGrid_ = enabled;
    QSettings().setValue(kTransparencyGridSetting, enabled);
    if (transparencyGridAction_ != nullptr && transparencyGridAction_->isChecked() != enabled) {
        const QSignalBlocker blocker(transparencyGridAction_);
        transparencyGridAction_->setChecked(enabled);
    }
    updateGpuResidentPresentation();
    update();
}

void ViewerEditor::applyApplicationPreferences(const ApplicationPreferences& preferences) {
    // Drive the panel's own controls rather than reaching past them, so their existing
    // persistence and the preview controller's resolution update stay the one code path. An index
    // equal to the current one emits nothing, which is what makes this idempotent.
    const auto resolutionIndex = [&preferences] {
        switch (preferences.viewerResolution) {
        case ViewerResolutionPreference::Full:
            return 1;
        case ViewerResolutionPreference::Half:
            return 2;
        case ViewerResolutionPreference::Quarter:
            return 3;
        case ViewerResolutionPreference::Auto:
            break;
        }
        return 0;
    }();
    if (resolutionDropdown_ != nullptr && resolutionDropdown_->currentIndex() != resolutionIndex) {
        resolutionDropdown_->setCurrentIndex(resolutionIndex);
    }

    const int backgroundIndex = static_cast<int>(viewerBackgroundFor(preferences.viewerBackground));
    if (backgroundDropdown_ != nullptr && backgroundDropdown_->currentIndex() != backgroundIndex) {
        backgroundDropdown_->setCurrentIndex(backgroundIndex);
    }

    const auto syncAction = [](QAction* action, const bool desired) {
        if (action != nullptr && action->isChecked() != desired) {
            action->setChecked(desired);
        }
    };
    syncAction(transparencyGridAction_, preferences.viewerTransparencyGrid);
    syncAction(safeAreasAction_, preferences.viewerSafeAreas);
    syncAction(centreCrossAction_, preferences.viewerCentreCross);
    syncAction(thirdsAction_, preferences.viewerThirds);
    syncAction(rulersAction_, preferences.viewerRulers);
    syncAction(pixelGridAction_, preferences.viewerPixelGrid);
}

ViewTransform ViewerEditor::viewTransformForTest() const noexcept { return transform_; }

QString ViewerEditor::statusBarReadoutTextForTest() const {
    return viewerResolutionText(previewController_);
}

ViewerChannel ViewerEditor::channelForTest() const noexcept { return channel_; }

ViewerBackground ViewerEditor::backgroundForTest() const noexcept { return background_; }

QString ViewerEditor::timeReadoutTextForTest() const { return timeReadout_->text(); }

kit::KDropdown* ViewerEditor::zoomDropdownForTest() const noexcept { return zoomDropdown_; }

QRectF ViewerEditor::statusBarRect() const {

    // hosted footer widget, this widget's own rect no longer reserves any space for it at all.
    if (statusBarFooterTaken_) {
        return {};
    }
    const qreal barHeight = kit::px(kit::Size::FooterRow);
    return QRectF(0.0, static_cast<qreal>(height()) - barHeight, static_cast<qreal>(width()),
                  barHeight);
}

QRectF ViewerEditor::contentRect() const {
    const QRectF bar = statusBarRect();
    return QRectF(rect()).adjusted(0.0, 0.0, 0.0, -bar.height());
}

QRectF ViewerEditor::canvasRect() const {
    const QRectF bar = statusBarRect();
    const qreal padding = kit::px(kit::Size::ViewerWorkPadding);
    return QRectF(rect()).adjusted(padding, padding, -padding, -bar.height() - padding);
}

QColor ViewerEditor::frameBackgroundColor() const {
    return compositionFrameColor(session_.composition());
}

void ViewerEditor::layoutStatusBar() {
    // FORMAL AMENDMENT 1: once the footer has been taken, its new owner positions it; this widget
    // no longer has a bar rect to place it in at all. Until then it fills the bottom strip
    // canvasRect() reserves, and the footer's own resizeEvent lays its controls out inside itself.
    if (statusBarFooterTaken_ || statusBarFooter_ == nullptr) {
        return;
    }
    statusBarFooter_->setGeometry(statusBarRect().toRect());
}

void ViewerEditor::updatePreviewResolution() {
    // A resident frame is presented at its native resolution. Auto stays stable Full so a zoom/pan
    // only updates the present parameters (destination/source) and never forces a re-evaluation or
    // an implicit proxy resolution change. Half/Quarter remain explicit choices made elsewhere.
    const auto base = previewController_.state().frame;
    if (residentFrameIsDisplayed() ||
        (frameInCurrentDomain(base) &&
         base->provenance().provider == runtime::PreviewDisplayProvider::GpuResident)) {
        return;
    }
    const auto geometry = currentDisplayGeometry();
    if (!geometry.has_value()) {
        return;
    }
    const auto actual = actualPixelRect(canvasRect(), geometry->extent, geometry->pixelAspect);
    const auto displayed = viewTransformedDisplayRect(canvasRect(), geometry->extent,
                                                      geometry->pixelAspect, transform_);
    if (actual.isEmpty() || displayed.isEmpty()) {
        return;
    }
    previewController_.setDisplayedCompositionScale(
        std::max(displayed.width() / actual.width(), displayed.height() / actual.height()) *
        devicePixelRatioF());
}

bool ViewerEditor::event(QEvent* event) {
    if (textEdit_ && event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    if (textEdit_ && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide))
        finishTextEditing(true);
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate ||
        event->type() == QEvent::Hide || event->type() == QEvent::DevicePixelRatioChange)
        cancelCreation();
    if (dragActive_ && (event->type() == QEvent::UngrabMouse ||
                        event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide))
        endDrag(false);
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::DevicePixelRatioChange) {
        if (dragActive_) {
            endDrag(false);
        }
        updatePreviewResolution();
        updateGpuResidentPresentation();
    }
    return handled;
}

void ViewerEditor::setZoomFit() {
    transform_ = ViewTransform{};
    refreshZoomDropdown();
    update();
}

void ViewerEditor::setZoomActualSize() { setZoomPercent(100); }

void ViewerEditor::setZoomPercent(const int percent) {
    transform_ = ViewTransform{.fitToWindow = false, .zoom = percent / 100.0, .pan = {0.0, 0.0}};
    refreshZoomDropdown();
    update();
}

std::optional<ViewerEditor::DisplayGeometry> ViewerEditor::currentDisplayGeometry() const {
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return std::nullopt;
    }
    const auto format = composition->format();
    const auto extent = render::ImageExtent::create(format.width(), format.height());
    return DisplayGeometry{.extent = *extent.value(), .pixelAspect = format.pixelAspect()};
}

void ViewerEditor::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), kit::color(kit::Color::Canvas));

    // The surround fills EVERY pixel of the content area -- above the
    // footer -- not just the padded fit rectangle (owner, 2026-09-15: "it should be 100% to
    // viewer content width and height, not partial"). canvasRect() stays the fit target only.
    const QRectF surround = contentRect();
    const auto* composition = session_.composition();

    if (composition == nullptr) {
        // Honest empty state (decision 5): the plain surround -- no frame background or grid,
        // which belong inside a composition frame and there is none -- under the no-composition
        // invitation widget (buildEmptyState()), which offers New Composition.
        drawCanvasBackground(painter, surround, background_);
        return;
    }

    drawCanvasBackground(painter, surround, background_);

    // While a native present is genuinely active the child QWindow occludes this widget's painting
    // and owns the pixels AND the overlays (they were recorded into the native overlay). Do not
    // paint a CPU image behind it.
    if (residentActive_ && !coverSnapshotInProgress_) {
        return;
    }

    // A composition with no layers shows just its frame and background: no invitation.
    paintViewerContent(painter);
}

void ViewerEditor::paintViewerContent(QPainter& painter) {
    // During a cover snapshot the pixmap must show the CURRENT state, never a stale frame. When the
    // current frame is the resident arm it has no CPU pixels, so the last valid CPU fallback / last
    // displayed CPU frame is the honest image. When it is not (including the composition-less blank
    // state, where displayedFrame() is null), paint exactly that current frame: a blank state stays
    // blank (canvas background + invitation) instead of flashing the previous composition behind
    // the still-mapped native container.
    PreparedPreviewFrameHandle cpuFrame;
    if (coverSnapshotInProgress_) {
        const auto current = preparedDisplayFrame();
        if (current != nullptr &&
            current->provenance().provider == runtime::PreviewDisplayProvider::GpuResident) {
            cpuFrame = frameInCurrentDomain(cpuFallbackFrame_) ? cpuFallbackFrame_
                       : frameInCurrentDomain(lastCpuFrame_)   ? lastCpuFrame_
                                                               : nullptr;
        } else {
            cpuFrame = current                               ? current
                       : frameInCurrentDomain(lastCpuFrame_) ? lastCpuFrame_
                                                             : nullptr;
        }
    } else {
        cpuFrame = paintableCpuFrame();
    }
    const auto packedDisplay = choosePackedPaintDisplay(cpuFrame);
    cpuFrame = packedDisplay.frame;
    const QRectF frame = canvasRect();
    ViewerDisplayDescriptor painted{.frame = nullptr,
                                    .mapping = std::nullopt,
                                    .captureRect = contentRect(),
                                    .devicePixelRatio = devicePixelRatioF(),
                                    .channel = packedDisplay.channel,
                                    .surroundColor = viewerSurroundColor(background_).rgba(),
                                    .frameColor = frameBackgroundColor().rgba(),
                                    .transparencyGrid = transparencyGrid_,
                                    .overlayRevision = gpuOverlayRevision_,
                                    .native = false,
                                    .cover = coverSnapshotInProgress_};
    if (cpuFrame != nullptr) {
        // displayBufferView() normalizes both display-product alternatives (reference and
        // qualified) to the same packed-RGBA8 shape -- the viewer draws pixels identically either
        // way; isOcioQualified is only ever read for the status bar's color-state chip, never to
        // change how pixels are drawn.
        const auto bufferView = cpuFrame->displayBufferView();
        if (bufferView.has_value()) {
            const auto dataWindow = bufferView->dataWindow.value_or(bufferView->displayWindow);
            const auto extent = dataWindow.extent();
            const auto& layout = bufferView->layout;
            if (extent.width() <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
                extent.height() <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
                layout.rowStrideBytes <=
                    static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
                const auto pixels = packedDisplay.pixels;
                // cpuFrame owns the immutable bytes for this entire paint. The const-data
                // QImage constructor borrows them, so presentation does not copy or convert a
                // full frame on the UI thread.
                const QImage image(
                    reinterpret_cast<const uchar*>(pixels.data()), static_cast<int>(extent.width()),
                    static_cast<int>(extent.height()),
                    static_cast<qsizetype>(layout.rowStrideBytes), QImage::Format_RGBA8888);
                if (!image.isNull()) {
                    const auto format = cpuFrame->processIdentity().plan->format();
                    const auto fullExtent =
                        render::ImageExtent::create(format.width(), format.height());
                    const std::optional<DisplayGeometry> geometry =
                        fullExtent ? std::optional{DisplayGeometry{*fullExtent.value(),
                                                                   format.pixelAspect()}}
                                   : std::nullopt;
                    const QRectF displayRect =
                        geometry.has_value()
                            ? viewTransformedDisplayRect(frame, geometry->extent,
                                                         geometry->pixelAspect, transform_)
                            : QRectF{};
                    // The composition background (or the grid) sits inside the frame only, under
                    // the image; the image composites over it in display space, exactly as the
                    // GPU present composites.
                    drawFrameBackground(painter, contentRect(), displayRect, frameBackgroundColor(),
                                        transparencyGrid_);
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                    const auto& fullWindow = bufferView->displayWindow;
                    const QRectF dataRect{
                        displayRect.left() +
                            static_cast<double>(dataWindow.originX() - fullWindow.originX()) /
                                fullWindow.extent().width() * displayRect.width(),
                        displayRect.top() +
                            static_cast<double>(dataWindow.originY() - fullWindow.originY()) /
                                fullWindow.extent().height() * displayRect.height(),
                        static_cast<double>(extent.width()) / fullWindow.extent().width() *
                            displayRect.width(),
                        static_cast<double>(extent.height()) / fullWindow.extent().height() *
                            displayRect.height()};
                    painter.drawImage(dataRect, image, QRectF(image.rect()));
                    if (!coverSnapshotInProgress_ && !displayRect.isEmpty()) {
                        if (const auto& trace = previewController_.frameTrace(); trace) {
                            // QPainter submission, not compositor scanout. Use the actual painted
                            // CPU fallback identity, which may differ from current desired intent.
                            trace->record(*cpuFrame, PreviewTraceStage::CpuPaint,
                                          PreviewTraceOutcome::Accepted);
                        }
                    }
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
                    paintViewerCompositionFrame(painter, displayRect);
                    if (geometry.has_value()) {
                        const auto bounds = previewController_.selectedLayerBounds(cpuFrame);
                        const auto descriptor = render::ReferenceDisplayBufferDescriptor::create(
                            bufferView->displayWindow, bufferView->pixelAspect);
                        if (descriptor && session_.composition()) {
                            const ViewerMapping mapping{
                                displayRect, format, cpuFrame->desiredIdentity().resolution,
                                bufferView->pixelAspect, *descriptor.value()};
                            painted.frame = cpuFrame;
                            painted.mapping = mapping;
                            painter.save();
                            painter.setClipRect(contentRect());
                            paintViewerOverlays(
                                painter, frame, mapping, effectiveZoom(displayRect, *geometry),
                                overlayOptions_,
                                textEdit_ ? std::span<const runtime::EvaluatedOperationBounds>{}
                                          : bounds,
                                pointTextLayers(session_, bounds));
                            painter.restore();
                        }
                    }
                }
            }
        }
    }

    paintRoi(painter);
    paintCreation(painter);
    paintPathTools(painter);
    paintAnchorEditing(painter);
    paintTextEditing(painter);

    if (coverSnapshotInProgress_)
        coverCaptureDisplay_ = std::move(painted);
    else {
        const bool changed = !cpuPaintedDisplay_ || cpuPaintedDisplay_->frame != painted.frame ||
                             cpuPaintedDisplay_->mapping != painted.mapping ||
                             cpuPaintedDisplay_->channel != painted.channel ||
                             cpuPaintedDisplay_->surroundColor != painted.surroundColor ||
                             cpuPaintedDisplay_->frameColor != painted.frameColor ||
                             cpuPaintedDisplay_->transparencyGrid != painted.transparencyGrid ||
                             cpuPaintedDisplay_->overlayRevision != painted.overlayRevision;
        cpuPaintedDisplay_ = std::move(painted);
        if (changed)
            scheduleDisplayedStateRefresh();
    }

    // Readiness and diagnostics remain in the status bar. Selection geometry is derived from
    // the delivered process frame and painted in screen space above the composition.
}

void ViewerEditor::scheduleDisplayedStateRefresh() {
    if (displayedStateRefreshQueued_)
        return;
    displayedStateRefreshQueued_ = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            displayedStateRefreshQueued_ = false;
            updatePreviewAccessibility();
        },
        Qt::QueuedConnection);
}

void ViewerEditor::updatePreviewAccessibility() {
    const auto preview = displayedPreviewState();
    QString frameDescription;
    switch (preview.freshness) {
    case FrameFreshness::None:
        frameDescription = tr("No composition pixels are displayed");
        break;
    case FrameFreshness::Current:
        frameDescription = tr("Current composition pixels are displayed");
        break;
    case FrameFreshness::Stale:
        frameDescription = tr("Previous composition pixels are displayed and marked out of date");
        break;
    }
    // The local accessible description and the selected viewer's window status share exact actual
    // display metadata. Shared controller/cache activity remains preparation truth.
    const QString colorStateDescription = previewColorState(preview).text;
    setAccessibleDescription(
        tr("%1. %2. %3").arg(preview.message, frameDescription, colorStateDescription));
    emit displayedStateChanged();
    if (probePosition_)
        refreshProbe(*probePosition_);
}

std::optional<render::ReferenceDisplayBufferDescriptor>
viewerDisplayDescriptorForFrame(std::optional<runtime::PreviewDisplayBufferView> cpuView,
                                std::optional<ResidentFrameGeometry> residentFrame) noexcept {
    if (cpuView.has_value()) {
        const auto descriptorResult = render::ReferenceDisplayBufferDescriptor::create(
            cpuView->displayWindow, cpuView->pixelAspect);
        if (!descriptorResult) {
            return std::nullopt;
        }
        return *descriptorResult.value();
    }
    if (residentFrame.has_value()) {
        // The resident arm has no host pixels by construction; its descriptor is built from the
        // lease's immutable geometry exactly as buildResidentPresentRequest() already does for the
        // native present. This is metadata, never a readback.
        const auto descriptorResult = render::ReferenceDisplayBufferDescriptor::create(
            residentFrame->displayWindow, residentFrame->pixelAspect);
        if (!descriptorResult) {
            return std::nullopt;
        }
        return *descriptorResult.value();
    }
    return std::nullopt;
}

std::optional<ViewerMapping> ViewerEditor::currentMapping() const {
    const auto actual = actualDisplayDescriptor();
    if (!actual || !actual->frame || !actual->mapping)
        return std::nullopt;
    const auto& identity = actual->frame->desiredIdentity();
    const auto& live = session_.snapshot();
    // Reject the foreign/time domain BEFORE any retained-layer remapping.
    if (!frameInCurrentDomain(actual->frame) || identity.time != session_.currentTime())
        return std::nullopt;
    const auto& evaluation = session_.evaluationSnapshotForTime(identity.time);
    if (identity.projectId != evaluation.project().id() ||
        (identity.sourceRevision != live.revision() &&
         identity.sourceRevision != evaluation.revision()) ||
        actual->mapping->compositionFormat != session_.composition()->format() ||
        actual->overlayRevision != gpuOverlayRevision_ || actual->captureRect != contentRect() ||
        actual->devicePixelRatio != devicePixelRatioF())
        return std::nullopt;
    return actual->mapping;
}

ViewerHit ViewerEditor::hitAt(const ViewerMapping& mapping, const QPointF point) const {
    std::vector<runtime::EvaluatedOperationBounds> ordered;
    const auto actual = actualDisplayDescriptor();
    const auto frame = actual ? actual->frame : nullptr;
    if (!frameInCurrentDomain(frame) || frame->desiredIdentity().time != session_.currentTime())
        return {};
    // SPLIT-2: read geometry translated to the CURRENT live graph, then order by the live Merge
    // entries. The raw frame bounds carry the retained snapshot's layer IDs, which after an
    // equivalent split name the head.
    const auto bounds = previewController_.currentLayerBounds(frame);
    if (const auto* stack = session_.timelineMerge()) {
        for (const auto& entry : stack->entries()) {
            const auto found = std::ranges::find(bounds, entry.layerId,
                                                 &runtime::EvaluatedOperationBounds::layerId);
            if (found != bounds.end())
                ordered.push_back(*found);
        }
    }
    const auto selected = previewController_.selectedLayerBounds(frame);
    return hitTestViewer(mapping, point, ordered, selected, pointTextLayers(session_, selected));
}

bool ViewerEditor::mappingStillValid() const {
    if (!activeMapping_ || !activeGestureIdentity_ || !session_.composition() ||
        !session_.transformInteractionActive())
        return false;
    const auto& identity = *activeGestureIdentity_;
    if (identity.projectId != session_.snapshot().project().id() ||
        identity.compositionId != session_.compositionId() ||
        identity.time != session_.currentTime() ||
        activeMapping_->compositionFormat != session_.composition()->format() ||
        activeGestureViewport_ != contentRect() || activeGestureTransform_ != transform_ ||
        activeGestureSelectionRevision_ != gpuOverlayRevision_ || gpuNativeRetirePending_ ||
        gpuHostMutationPending_)
        return false;
    // An ongoing gesture uses its frozen mapping. Prepared/applied lag and a lease becoming
    // inadmissible cannot invalidate immutable geometry. A genuinely applied format/proxy change
    // can.
    const auto actual = actualDisplayDescriptor();
    return actual && actual->mapping && *actual->mapping == *activeMapping_;
}

void ViewerEditor::endDrag(const bool commit) {
    dragActive_ = false;
    activeMapping_.reset();
    activeGestureIdentity_.reset();
    if (commit) {
        (void)session_.commitTransformInteraction();
    } else {
        session_.cancelTransformInteraction();
    }
    // Reuses TimelineRuler's Interactive-cadence arming (docs/architecture/animation-and-time.md,
    // "Session Time And Scrubbing"): bypasses any remaining trailing delay and disarms it.
    previewController_.notifyScrubEnded();
}

void ViewerEditor::beginPan(const Qt::MouseButton button, const QPointF screenPoint,
                            const DisplayGeometry& geometry) {
    panActive_ = true;
    panButton_ = button;
    panOrigin_ = screenPoint;
    const QRectF frame = canvasRect();
    if (transform_.fitToWindow) {
        // Materializes the Fit-implied zoom into an equivalent Custom transform before panning:
        // Fit recomputes its rectangle from `frame` every call and has no stored zoom/pan to
        // accumulate onto, so panning while Fit means "start being Custom, at the zoom Fit
        // currently shows, with zero pan" -- an exact reproduction of the same rectangle (see
        // viewTransformedDisplayRect()'s own centering, identical to fitDisplayRect()'s).
        const QRectF fitted = fitDisplayRect(frame, geometry.extent, geometry.pixelAspect);
        const QRectF actual = actualPixelRect(frame, geometry.extent, geometry.pixelAspect);
        const double zoom = actual.width() > 0.0 ? fitted.width() / actual.width() : 1.0;
        panBaseTransform_ = ViewTransform{.fitToWindow = false, .zoom = zoom, .pan = {0.0, 0.0}};
    } else {
        panBaseTransform_ = transform_;
    }
    setFocus(Qt::MouseFocusReason);
    updatePanCursor();
}

ViewerTool ViewerEditor::tool() const noexcept { return session_.tools().tool(); }

void ViewerEditor::selectTool(const ViewerTool tool) {
    // This Viewer confirms what it has in flight even when the tool does not change (Escape on
    // Select); toolChanged() does the same in every Viewer when it does.
    toolChanged();
    session_.tools().setTool(tool);
    setFocus(Qt::ShortcutFocusReason);
}

void ViewerEditor::toolChanged() {
    finishTextEditing(true);
    cancelCreation();
    if (dragActive_)
        endDrag(false);
    updatePanCursor();
    notifyAnchorModeState();
    update();
}

void ViewerEditor::updatePanCursor() {
    if (panActive_) {
        setCursor(Qt::ClosedHandCursor);

    } else if (tool() == ViewerTool::Hand) {
        setCursor(Qt::OpenHandCursor);
    } else if (tool() == ViewerTool::Zoom || tool() == ViewerTool::Cryptomatte) {
        setCursor(Qt::CrossCursor);
    } else {
        unsetCursor();
    }
}

void ViewerEditor::mousePressEvent(QMouseEvent* event) {
    if (roiPress(event))
        return;
    if (cryptomattePress(event))
        return;
    if (textEditPress(event))
        return;
    if (textPress(event))
        return;
    if (anchorPress(event))
        return;
    if (pathPress(event))
        return;
    if (creationPress(event))
        return;
    if (dragActive_ && event->button() == Qt::RightButton) {
        endDrag(false);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton)
        setFocus(Qt::MouseFocusReason);
    if (!dragActive_ && !panActive_) {
        if (const auto geometry = currentDisplayGeometry();
            geometry.has_value() &&
            (event->button() == Qt::MiddleButton ||
             (event->button() == Qt::LeftButton && tool() == ViewerTool::Hand))) {
            beginPan(event->button(), event->position(), *geometry);
            event->accept();
            return;
        }
    }

    if (!dragActive_ && !panActive_ && tool() == ViewerTool::Zoom &&
        event->button() == Qt::LeftButton) {
        if (const auto geometry = currentDisplayGeometry()) {
            transform_ =
                zoomAboutPoint(transform_, canvasRect(), geometry->extent, geometry->pixelAspect,
                               event->position(),
                               event->modifiers().testFlag(Qt::AltModifier) ? 1.0 / kZoomStepFactor
                                                                            : kZoomStepFactor);
            refreshZoomDropdown();
            update();
        }
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton || tool() != ViewerTool::Select || dragActive_) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    auto mapping = currentMapping();
    if (!mapping.has_value() || !contentRect().contains(event->position())) {
        QWidget::mousePressEvent(event);
        return;
    }
    const auto hitDisplay = actualDisplayDescriptor();
    if (!hitDisplay || !hitDisplay->frame)
        return;
    const auto& pressedIdentity = hitDisplay->frame->desiredIdentity();
    const auto pressedViewport = contentRect();
    const auto pressedTransform = transform_;
    const auto hit = hitAt(*mapping, event->position());
    if (hit.region == ViewerHitRegion::Empty) {
        session_.clearSelection();
        event->accept();
        return;
    }
    QPointer<ViewerEditor> self(this);
    session_.selectLayer(hit.layerId, event->modifiers().testFlag(Qt::ShiftModifier));
    if (!self)
        return;
    const auto actual = actualDisplayDescriptor();
    // Selection emits synchronous callbacks. Start only if the same painted identity/mapping and
    // live geometry still own this press; never substitute shared preparation when display
    // vanished.
    if (!actual || !actual->frame || !actual->mapping || !frameInCurrentDomain(actual->frame) ||
        actual->frame->desiredIdentity() != pressedIdentity ||
        pressedIdentity.time != session_.currentTime() || *actual->mapping != *mapping ||
        mapping->compositionFormat != session_.composition()->format() ||
        actual->captureRect != pressedViewport || contentRect() != pressedViewport ||
        actual->devicePixelRatio != devicePixelRatioF() || transform_ != pressedTransform ||
        gpuNativeRetirePending_ || gpuHostMutationPending_)
        return;
    const auto selected = previewController_.selectedLayerBounds(actual->frame);
    const auto bounds =
        std::ranges::find(selected, hit.layerId, &runtime::EvaluatedOperationBounds::layerId);
    if (bounds == selected.end())
        return;
    auto kind = TransformGesture::Kind::Move;
    switch (hit.region) {
    case ViewerHitRegion::Scale:
        kind = TransformGesture::Kind::Scale;
        break;
    case ViewerHitRegion::Rotate:
        kind = TransformGesture::Kind::Rotate;
        break;
    case ViewerHitRegion::Anchor:
        kind = TransformGesture::Kind::Anchor;
        break;
    default:
        break;
    }
    if (session_.beginTransformInteraction({kind, hit.handle, event->position(), *bounds}, *mapping,
                                           {event->modifiers().testFlag(Qt::ShiftModifier),
                                            event->modifiers().testFlag(Qt::AltModifier)})) {
        event->accept();
        return;
    }
    dragActive_ = true;
    activeMapping_ = mapping;
    activeGestureIdentity_ = actual->frame->desiredIdentity();
    activeGestureViewport_ = contentRect();
    activeGestureTransform_ = transform_;
    activeGestureSelectionRevision_ = gpuOverlayRevision_;
    setFocus(Qt::MouseFocusReason);
    previewController_.beginInteractiveScrub();
    event->accept();
}

void ViewerEditor::mouseMoveEvent(QMouseEvent* event) {
    refreshProbe(event->position());
    if (roiMove(event))
        return;
    if (textEdit_) {
        if (textEdit_->dragging)
            moveTextCaret(event->position(), true);
        event->accept();
        return;
    }
    if (anchorMove(event))
        return;
    if (pathMove(event))
        return;
    if (creationMove(event))
        return;
    if (panActive_) {
        transform_ = panBaseTransform_;
        transform_.pan += (event->position() - panOrigin_);
        refreshZoomDropdown();
        update();
        event->accept();
        return;
    }
    if (!dragActive_) {
        if (tool() == ViewerTool::Select) {
            const auto mapping = currentMapping();
            const auto hit = mapping ? hitAt(*mapping, event->position()) : ViewerHit{};
            switch (hit.region) {
            case ViewerHitRegion::Move:
                setCursor(Qt::SizeAllCursor);
                break;
            case ViewerHitRegion::Anchor:
                setCursor(Qt::CrossCursor);
                break;
            case ViewerHitRegion::Rotate:
                setCursor(Qt::OpenHandCursor);
                break;
            case ViewerHitRegion::Scale:
                switch (hit.handle % 4) {
                case 0:
                    setCursor(Qt::SizeFDiagCursor);
                    break;
                case 1:
                    setCursor(Qt::SizeVerCursor);
                    break;
                case 2:
                    setCursor(Qt::SizeBDiagCursor);
                    break;
                default:
                    setCursor(Qt::SizeHorCursor);
                    break;
                }
                break;
            case ViewerHitRegion::Empty:
                unsetCursor();
                break;
            }
        }
        QWidget::mouseMoveEvent(event);
        return;
    }
    if (!mappingStillValid()) {
        endDrag(false);
        QWidget::mouseMoveEvent(event);
        return;
    }
    // Total displacement from the ORIGINAL press point, not from the previous move -- base value
    // plus TOTAL gesture displacement, never a chain of already-rounded intermediates (docs/
    // architecture/animation-and-time.md).
    session_.updateTransformInteraction(event->position(),
                                        {event->modifiers().testFlag(Qt::ShiftModifier),
                                         event->modifiers().testFlag(Qt::AltModifier)});
    event->accept();
}

void ViewerEditor::mouseReleaseEvent(QMouseEvent* event) {
    if (roiRelease(event))
        return;
    if (textEdit_ && event->button() == Qt::LeftButton) {
        textEdit_->dragging = false;
        event->accept();
        return;
    }
    if (anchorRelease(event))
        return;
    if (pathRelease(event))
        return;
    if (creationRelease(event))
        return;
    if (panActive_ && event->button() == panButton_) {
        panActive_ = false;
        panButton_ = Qt::NoButton;
        updatePanCursor();
        event->accept();
        return;
    }
    if (!dragActive_ || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (mappingStillValid()) {
        session_.updateTransformInteraction(event->position(),
                                            {event->modifiers().testFlag(Qt::ShiftModifier),
                                             event->modifiers().testFlag(Qt::AltModifier)});
        endDrag(true);
    } else {
        endDrag(false);
    }
    event->accept();
}

void ViewerEditor::wheelEvent(QWheelEvent* event) {
    if (dragActive_ || panActive_ || creation_) {
        event->ignore();
        return;
    }
    const auto geometry = currentDisplayGeometry();
    const int notches = event->angleDelta().y() / 120;
    if (!geometry.has_value() || notches == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    const double factor = std::pow(kZoomStepFactor, notches);
    transform_ = zoomAboutPoint(transform_, canvasRect(), geometry->extent, geometry->pixelAspect,
                                event->position(), factor);
    refreshZoomDropdown();
    update();
    event->accept();
}

void ViewerEditor::keyPressEvent(QKeyEvent* event) {
    // Escape confirms every Viewer drag as it stands and returns to Select; Undo discards a
    // document edit. A region-of-interest drag keeps the region drawn so far.
    if (roiGesture_ && event->key() == Qt::Key_Escape) {
        commitRoiGesture(roiGesture_->pointer);
        selectTool(ViewerTool::Select);
        event->accept();
        return;
    }
    if (textEditKey(event))
        return;
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        tool() == ViewerTool::Select && beginTextEditing()) {
        event->accept();
        return;
    }
    if (pathKey(event))
        return;
    if (!dragActive_ && !panActive_ && event->modifiers() == Qt::NoModifier) {
        // The bare letters are the app-wide tool keys (toolForKey); Escape returns to Select.
        const auto next = event->key() == Qt::Key_Escape ? std::optional(ViewerTool::Select)
                                                         : toolForKey(event->key());
        if (next) {
            selectTool(*next);
            event->accept();
            return;
        }
    }
    if (dragActive_ && event->key() == Qt::Key_Escape) {
        // A move, rotate, scale or anchor-point gizmo drag keeps its change as one command; a
        // drag whose frozen mapping no longer holds cannot be confirmed and is cancelled.
        endDrag(mappingStillValid());
        selectTool(ViewerTool::Select);
        event->accept();
        return;
    }
    if (!dragActive_ && !panActive_) {

        if (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::ShiftModifier) {
            if (event->key() == Qt::Key_Delete) {
                std::set<document::NodeId> nodes;
                for (const auto node : session_.selectedNodes())
                    if (session_.layerForNode(node))
                        nodes.insert(node);
                if (!nodes.empty()) {
                    commands::Transaction transaction("Delete Layers",
                                                      session_.snapshot().revision());
                    transaction.emplace<commands::RemoveNodes>(session_.compositionId(),
                                                               std::move(nodes));
                    (void)session_.executeTransaction(std::move(transaction));
                }
                event->accept();
                return;
            }
            QPointF delta;
            const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 10.0 : 1.0;
            switch (event->key()) {
            case Qt::Key_Left:
                delta.setX(-step);
                break;
            case Qt::Key_Right:
                delta.setX(step);
                break;
            case Qt::Key_Up:
                delta.setY(-step);
                break;
            case Qt::Key_Down:
                delta.setY(step);
                break;
            default:
                break;
            }
            if (!delta.isNull()) {
                nudgeViewerSelection(session_, delta);
                event->accept();
                return;
            }
        }

        // Ctrl+0 fits and Ctrl+1 is actual size, in this canvas and in the node canvas alike (task
        // S1, item 8). F and Z are retired in both: the Adobe-standard pair is what an artist
        // arriving from another compositor reaches for, and a single-letter binding that far up the
        // alphabet is needed for tools. The other two ways to reach either are still the context
        // menu and the zoom dropdown (decision 2/4).
        if (event->modifiers() == Qt::ControlModifier) {
            if (event->key() == Qt::Key_1) {
                setZoomActualSize();
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_0) {
                setZoomFit();
                event->accept();
                return;
            }
        }
    }
    QWidget::keyPressEvent(event);
}

void ViewerEditor::resizeEvent(QResizeEvent* event) {
    cancelCreation();
    QWidget::resizeEvent(event);
    if (dragActive_) {
        endDrag(false);
    }
    if (panActive_) {
        panActive_ = false;
        panButton_ = Qt::NoButton;
        updatePanCursor();
    }
    layoutStatusBar();
    layoutEmptyState();
    updatePreviewResolution();
    updateGpuResidentPresentation();
}

#include "viewer_tools.ipp"
#include "viewer_tools_anchors.ipp"
#include "viewer_tools_mask.ipp"
#include "viewer_tools_path.ipp"

#include "viewer_editor_gpu.ipp"
#include "viewer_editor_gpu_retire.ipp"

} // namespace bloom::ui
