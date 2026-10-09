#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <limits>

namespace bloom::ui {
void ViewerEditor::resetChannelDisplay() {
    ++channelEpoch_;
    if (channelTask_)
        channelTask_->cancel();
    channelTaskCancelled_ = channelTask_.has_value();
    channelView_.reset();
    channelWantedFrame_.reset();
    channelFailure_.clear();
    scheduleDisplayedStateRefresh();
}

void ViewerEditor::startChannelDisplay() {
    if (channelTask_ || !channelWantedFrame_ || channelWantedChannel_ == ViewerChannel::Rgba)
        return;
    // One completed derivative and one worker allocation may overlap. Reserve their source
    // storage as well, counting restamped shared pixels only once. No replacement is admitted
    // until the cancelled worker has drained; borrowed upstream sources are never copied here.
    auto retainedBytes = viewerPackedSourceByteCost(channelWantedFrame_);
    const auto add = [&](const std::size_t bytes) {
        retainedBytes = bytes > std::numeric_limits<std::size_t>::max() - retainedBytes
                            ? std::numeric_limits<std::size_t>::max()
                            : retainedBytes + bytes;
    };
    if (channelView_) {
        add(channelView_->byteCost());
        if (!sameViewerPackedSource(channelView_->frame, channelWantedFrame_))
            add(viewerPackedSourceByteCost(channelView_->frame));
    }
    const auto submission = previewController_.submitViewerChannelDisplay(
        channelWantedFrame_, channelWantedChannel_, retainedBytes);
    if (!submission.accepted()) {
        channelFailure_ = tr("Viewer channel preparation could not be admitted");
        scheduleDisplayedStateRefresh();
        return;
    }
    channelTask_ = submission.handle;
    channelTaskFrame_ = channelWantedFrame_;
    channelTaskChannel_ = channelWantedChannel_;
    channelTaskEpoch_ = channelEpoch_;
    channelTaskCancelled_ = false;
    scheduleDisplayedStateRefresh();
}

void ViewerEditor::consumeChannelDisplay() {
    if (!channelTask_)
        return;
    auto result = channelTask_->tryTakeResult();
    if (!result)
        return;
    const bool current = channelTaskEpoch_ == channelEpoch_ &&
                         channelTaskChannel_ == channelWantedChannel_ &&
                         sameViewerPackedSource(channelTaskFrame_, channelWantedFrame_) &&
                         frameInCurrentDomain(channelWantedFrame_);
    channelTask_.reset();
    channelTaskFrame_.reset();
    channelTaskCancelled_ = false;
    if (current && result->state() == runtime::TaskState::Succeeded && result->value() &&
        *result->value()) {
        const auto& completed = *result->value();
        if (completed->channel == channelWantedChannel_ &&
            sameViewerPackedSource(completed->frame, channelWantedFrame_)) {
            channelView_ = completed;
            channelFailure_.clear();
        }
    } else if (current && result->state() == runtime::TaskState::Failed) {
        channelFailure_ = tr("Viewer channel preparation failed");
    }
    if (channelWantedFrame_ && channelFailure_.isEmpty() &&
        (!channelView_ || channelView_->channel != channelWantedChannel_ ||
         !sameViewerPackedSource(channelView_->frame, channelWantedFrame_)))
        startChannelDisplay();
    scheduleDisplayedStateRefresh();
    update();
}

ViewerEditor::PackedPaintDisplay
ViewerEditor::choosePackedPaintDisplay(PreparedPreviewFrameHandle frame) {
    if (!frame)
        return {};
    const auto view = frame->displayBufferView();
    if (!view)
        return {};
    if (channel_ == ViewerChannel::Rgba) {
        const bool changed = channelWantedFrame_ || channelView_ || !channelFailure_.isEmpty();
        channelWantedFrame_.reset();
        channelView_.reset();
        channelFailure_.clear();
        if (channelTask_ && !channelTaskCancelled_) {
            channelTask_->cancel();
            channelTaskCancelled_ = true;
        }
        if (changed)
            scheduleDisplayedStateRefresh();
        return {std::move(frame), view->pixels, ViewerChannel::Rgba};
    }
    const bool changed =
        channelWantedChannel_ != channel_ || !sameViewerPackedSource(channelWantedFrame_, frame);
    channelWantedFrame_ = frame;
    channelWantedChannel_ = channel_;
    if (changed) {
        channelFailure_.clear();
        if (channelTask_ && !channelTaskCancelled_ &&
            (channelTaskChannel_ != channel_ ||
             !sameViewerPackedSource(channelTaskFrame_, frame))) {
            channelTask_->cancel();
            channelTaskCancelled_ = true;
        }
        scheduleDisplayedStateRefresh();
    }
    if (channelView_ && channelView_->channel == channel_ &&
        sameViewerPackedSource(channelView_->frame, frame))
        return {std::move(frame), channelView_->pixels, channel_};
    if (channelFailure_.isEmpty())
        startChannelDisplay();
    // A complete older image/channel remains describable as Stale; its own frame supplies the
    // matching bounds. If no derived channel exists, show real RGBA and report that actual channel.
    if (channelView_ && frameInCurrentDomain(channelView_->frame))
        return {channelView_->frame, channelView_->pixels, channelView_->channel};
    return {std::move(frame), view->pixels, ViewerChannel::Rgba};
}
} // namespace bloom::ui
