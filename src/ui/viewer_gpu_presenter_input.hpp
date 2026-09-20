#pragma once

// Private input-translation helper for ViewerGpuPresenter. It turns one actual Qt event into a
// ViewerGpuInputEvent and invokes the callback once. It never reposts an event, so it cannot create
// a recursive forwarding loop, and it performs no layout math (the outer host applies the
// container's origin once). Position state is carried in `lastLocal`/`lastGlobal` across calls.

#include <bloom/ui/viewer_gpu_presenter.hpp>

#include <QPointF>

#include <functional>

class QEvent;

namespace bloom::ui::detail {

// Returns true when the event was one this adapter forwards; false otherwise.
[[nodiscard]] bool
forwardViewerGpuInput(QEvent* event, QPointF& lastLocal, QPointF& lastGlobal,
                      const std::function<void(const ViewerGpuInputEvent&)>& callback);

} // namespace bloom::ui::detail
