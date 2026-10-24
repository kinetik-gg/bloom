#include <bloom/ui/viewer_editor.hpp>

#include <bloom/document/channel_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/cryptomatte_controller.hpp>

#include <QMouseEvent>
#include <QToolTip>

#include <cmath>

namespace bloom::ui {

// #364: the eyedropper samples the selected Cryptomatte's channel set at the composition pixel
// under the cursor. A Cryptomatte's channel set sits at its own frame origin in the matte preview
// (Viewer channel menu, "Inspect Cryptomatte matte") and under an untransformed layer, so that
// pixel is the object the artist sees.
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
    const auto x = static_cast<std::int64_t>(std::floor(coordinate.x));
    const auto y = static_cast<std::int64_t>(std::floor(coordinate.y));
    const auto modifiers = event->modifiers();
    const auto action = modifiers.testFlag(Qt::AltModifier) ||
                                modifiers.testFlag(Qt::ControlModifier) ||
                                modifiers.testFlag(Qt::MetaModifier)
                            ? CryptomatteController::PickAction::Remove
                            : CryptomatteController::PickAction::Add;
    cryptomattePickPosition_ = event->position();
    cryptomattePickOutstanding_ = true;
    connect(controller, &CryptomatteController::changed, this,
            &ViewerEditor::cryptomattePickFinished, Qt::UniqueConnection);
    controller->pick(node->id, x, y, action);
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

} // namespace bloom::ui
