#include <bloom/ui/composition_preview_controller.hpp>

#include <algorithm>
#include <limits>

namespace bloom::ui {

void CompositionPreviewController::noteDroppedFrames(const std::uint64_t count) {
    if (!countingDroppedFrames_ || count == 0) {
        return;
    }
    droppedFrameCount_ +=
        std::min(count, std::numeric_limits<std::uint64_t>::max() - droppedFrameCount_);
    emit droppedFrameCountChanged();
}

void CompositionPreviewController::setPlaybackActive(const bool playing) {
    if (playbackActive_ == playing) {
        return;
    }
    playbackActive_ = playing;
    emit playbackActiveChanged(playing);
    if (!playing && active_.has_value() && active_->playbackOutstanding) {
        active_->handle.cancel();
        active_->playbackOutstanding = false;
        noteDroppedFrame();
    }
}

void CompositionPreviewController::presentPlaybackFrame(
    const std::chrono::nanoseconds untilNextTick) {
    if (shuttingDown_) {
        return;
    }
    playbackBudget_ = std::max(untilNextTick, std::chrono::nanoseconds::zero());
    requestPreview(false, PreviewRequestKind::Playback);
}

void CompositionPreviewController::recordPreparationDuration(
    const runtime::PreviewRequestIdentity& identity, const std::chrono::nanoseconds duration) {
    const auto key = cacheKeyForTime(identity.time);
    if (!key.has_value() || *key != PreviewFrameCacheKey::forIdentity(identity) ||
        duration < std::chrono::nanoseconds::zero()) {
        return;
    }
    preparationEstimate_ = std::max(preparationEstimate_.value_or(duration), duration);
}

} // namespace bloom::ui
