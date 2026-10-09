#include <bloom/ui/composition_preview_controller.hpp>

namespace bloom::ui {
void CompositionPreviewController::releasePublishedFramesForShutdown() {
    Q_ASSERT(shuttingDown_);
    if (!shuttingDown_)
        return;
    state_.frame.reset();
    state_.freshness = FrameFreshness::None;
    frameCache_->releaseForShutdown();
}
} // namespace bloom::ui
