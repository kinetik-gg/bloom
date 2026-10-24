#include <bloom/ui/viewer_editor.hpp>

#include "layer_mask_support.hpp"

#include <bloom/document/channel_nodes.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/cryptomatte_controller.hpp>

#include <QMouseEvent>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace bloom::ui {

// #364: the eyedropper picks the object the artist sees under the cursor: the Viewer point is
// mapped into the Cryptomatte's channel-set frame through the displaying layer's world transform,
// and the worker reads the source frame at the playhead through the layer's time.
bool ViewerEditor::cryptomattePress(QMouseEvent* event) {
    if (tool_ != Tool::Cryptomatte || event->button() != Qt::LeftButton)
        return false;
    event->accept();
    setFocus(Qt::MouseFocusReason);
    const auto showMessage = [this, event](const QString& message) {
        QToolTip::showText(mapToGlobal(event->position().toPoint()), message, this);
    };
    const auto* node = session_.selectedNode();
    auto* controller = session_.cryptomatteController();
    if (node == nullptr || node->typeId != document::kCryptomatteNodeType) {
        showMessage(tr("Select a Cryptomatte node to pick its mattes"));
        return true;
    }
    if (controller == nullptr) {
        showMessage(tr("Cryptomatte picking is unavailable"));
        return true;
    }
    // The pick reads the channel set, not the displayed pixels, so a frame still on screen from an
    // earlier revision (a matte edit re-renders it) maps the cursor as long as the composition
    // geometry and the capture are unchanged.
    auto mapping = currentMapping();
    if (const auto actual = actualDisplayDescriptor();
        !mapping && actual && actual->mapping && session_.composition() != nullptr &&
        actual->mapping->compositionFormat == session_.composition()->format() &&
        actual->captureRect == contentRect() && actual->devicePixelRatio == devicePixelRatioF())
        mapping = actual->mapping;
    if (!mapping || !contentRect().contains(event->position()) ||
        !mapping->displayRect.contains(event->position()))
        return true;
    const auto coordinate = mapping->toComposition(event->position());
    cryptomattePickPosition_ = event->position();
    cryptomattePickOutstanding_ = true;
    connect(controller, &CryptomatteController::changed, this,
            &ViewerEditor::cryptomattePickFinished, Qt::UniqueConnection);
    // The Viewer point in the channel set's frame. The matte preview shows the channel set at its
    // own frame origin; the composition shows it through the layer that displays the node, whose
    // evaluated content-to-composition transform (moves, scales, rotations, parents and the proxy
    // all included) is inverted exactly as mask editing does.
    QPointF framePoint(coordinate.x, coordinate.y);
    const auto& inspection = previewController_.viewerChannelInspection();
    if (!inspection || inspection->node != node->id) {
        if (inspection) {
            controller->reportPick(tr("Show the composition or this Cryptomatte's matte to pick"));
            return true;
        }
        const auto layer = session_.layerForNode(node->id);
        if (!layer) {
            controller->reportPick(tr("The Cryptomatte is not shown in the Viewer"));
            return true;
        }
        const auto actual = actualDisplayDescriptor();
        const auto bounds = previewController_.currentLayerBounds(actual ? actual->frame : nullptr);
        const auto found =
            std::ranges::find(bounds, *layer, &runtime::EvaluatedOperationBounds::layerId);
        const auto world =
            found != bounds.end() ? masks::contentToComposition(*found) : std::nullopt;
        if (!world) {
            controller->reportPick(tr("No object under the cursor"));
            return true;
        }
        framePoint = world->inverted().map(framePoint);
        const auto& local = found->local;
        if (!(framePoint.x() >= local.left && framePoint.x() < local.right &&
              framePoint.y() >= local.top && framePoint.y() < local.bottom)) {
            controller->reportPick(tr("No object under the cursor"));
            return true;
        }
    }
    const auto x = static_cast<std::int64_t>(std::floor(framePoint.x()));
    const auto y = static_cast<std::int64_t>(std::floor(framePoint.y()));
    const auto modifiers = event->modifiers();
    const auto action = modifiers.testFlag(Qt::AltModifier) ||
                                modifiers.testFlag(Qt::ControlModifier) ||
                                modifiers.testFlag(Qt::MetaModifier)
                            ? CryptomatteController::PickAction::Remove
                            : CryptomatteController::PickAction::Add;
    controller->pick(node->id, x, y, action, inspection);
    return true;
}

void ViewerEditor::cryptomattePickFinished() {
    auto* controller = session_.cryptomatteController();
    if (!cryptomattePickOutstanding_ || controller == nullptr || controller->pickPending())
        return;
    cryptomattePickOutstanding_ = false;
    if (!controller->lastPickMessage().isEmpty())
        QToolTip::showText(mapToGlobal(cryptomattePickPosition_.toPoint()),
                           controller->lastPickMessage(), this);
}

void ViewerEditor::refreshCryptomattePreview() {
    auto* controller = session_.cryptomatteController();
    std::optional<runtime::ChannelInspection> desired;
    if (const auto* node = session_.selectedNode();
        node != nullptr && node->typeId == document::kCryptomatteNodeType && controller != nullptr &&
        controller->preview(node->id))
        desired = runtime::ChannelInspection{
            node->id, std::string(document::kCryptomattePreviewInspectionPort), {}};
    const auto& current = previewController_.viewerChannelInspection();
    const bool currentIsPreview =
        current && current->port == document::kCryptomattePreviewInspectionPort;
    if ((currentIsPreview && current != desired) || (!current && desired))
        previewController_.setViewerChannelInspection(desired);
}

} // namespace bloom::ui
