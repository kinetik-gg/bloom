// Private input-translation helper implementation. Qt event types only; no invented event names.

#include "viewer_gpu_presenter_input.hpp"

#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <functional>
#include <utility>

namespace bloom::ui::detail {
namespace {

void updateLastPosition(QPointF& lastLocal, QPointF& lastGlobal, const QPointF& local,
                        const QPointF& global) {
    lastLocal = local;
    lastGlobal = global;
}

void fillMouse(ViewerGpuInputEvent& input, QMouseEvent* event, QPointF& lastLocal,
               QPointF& lastGlobal) {
    input.local = event->position();
    input.global = event->globalPosition();
    input.button = event->button();
    input.buttons = event->buttons();
    input.modifiers = event->modifiers();
    updateLastPosition(lastLocal, lastGlobal, input.local, input.global);
}

} // namespace

bool forwardViewerGpuInput(QEvent* event, QPointF& lastLocal, QPointF& lastGlobal,
                           const std::function<void(const ViewerGpuInputEvent&)>& callback) {
    ViewerGpuInputEvent input;
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
    case QEvent::MouseButtonDblClick:
        input.kind = event->type() == QEvent::MouseButtonPress ? ViewerGpuInputKind::MousePress
                     : event->type() == QEvent::MouseButtonRelease
                         ? ViewerGpuInputKind::MouseRelease
                     : event->type() == QEvent::MouseMove ? ViewerGpuInputKind::MouseMove
                                                          : ViewerGpuInputKind::MouseDoubleClick;
        fillMouse(input, static_cast<QMouseEvent*>(event), lastLocal, lastGlobal);
        break;
    case QEvent::Wheel: {
        input.kind = ViewerGpuInputKind::Wheel;
        auto* wheel = static_cast<QWheelEvent*>(event);
        input.local = wheel->position();
        input.global = wheel->globalPosition();
        input.buttons = wheel->buttons();
        input.modifiers = wheel->modifiers();
        input.pixelDelta = wheel->pixelDelta();
        input.angleDelta = wheel->angleDelta();
        input.scrollPhase = wheel->phase();
        input.inverted = wheel->inverted();
        updateLastPosition(lastLocal, lastGlobal, input.local, input.global);
        break;
    }
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        input.kind = event->type() == QEvent::KeyPress ? ViewerGpuInputKind::KeyPress
                                                       : ViewerGpuInputKind::KeyRelease;
        auto* key = static_cast<QKeyEvent*>(event);
        input.key = key->key();
        input.modifiers = key->modifiers();
        input.autoRepeat = key->isAutoRepeat();
        input.text = key->text();
        input.local = lastLocal;
        input.global = lastGlobal;
        break;
    }
    case QEvent::Enter: {
        input.kind = ViewerGpuInputKind::Enter;
        auto* enter = static_cast<QEnterEvent*>(event);
        input.local = enter->position();
        input.global = enter->globalPosition();
        input.buttons = enter->buttons();
        updateLastPosition(lastLocal, lastGlobal, input.local, input.global);
        break;
    }
    case QEvent::Leave:
        input.kind = ViewerGpuInputKind::Leave;
        input.local = lastLocal;
        input.global = lastGlobal;
        break;
    case QEvent::FocusIn:
        input.kind = ViewerGpuInputKind::FocusIn;
        input.local = lastLocal;
        input.global = lastGlobal;
        break;
    case QEvent::FocusOut:
        input.kind = ViewerGpuInputKind::FocusOut;
        input.local = lastLocal;
        input.global = lastGlobal;
        break;
    case QEvent::InputMethod: {
        input.kind = ViewerGpuInputKind::InputMethod;
        input.text = static_cast<QInputMethodEvent*>(event)->commitString();
        input.local = lastLocal;
        input.global = lastGlobal;
        break;
    }
    case QEvent::GrabMouse:
    case QEvent::UngrabMouse:
    case QEvent::TouchCancel:
        input.kind = event->type() == QEvent::GrabMouse     ? ViewerGpuInputKind::GrabMouse
                     : event->type() == QEvent::UngrabMouse ? ViewerGpuInputKind::UngrabMouse
                                                            : ViewerGpuInputKind::Cancel;
        input.local = lastLocal;
        input.global = lastGlobal;
        break;
    default:
        return false;
    }
    if (callback) {
        callback(input);
    }
    return true;
}

} // namespace bloom::ui::detail
