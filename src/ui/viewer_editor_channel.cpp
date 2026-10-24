#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QSignalBlocker>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/ui/kit/dropdown.hpp>

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

namespace bloom::ui {
namespace {
constexpr int kStandardChannelCount = 6;
} // namespace

void ViewerEditor::refreshChannelInspectionChoices() {
    std::optional<ChannelSetTarget> target;
    if (const auto* node = session_.selectedNode()) {
        if (const auto* definition =
                document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion)) {
            for (const auto& output : definition->outputs)
                if (output.valueKind == document::SocketValueKind::ChannelSet) {
                    target = ChannelSetTarget{node->id, output.name,
                                              session_.snapshot().revision()};
                    break;
                }
        }
    }
    // #364: a Cryptomatte offers its matte preview (the Mask output shown directly); there is no
    // channel set of its own to list.
    std::optional<ChannelSetTarget> matteTarget;
    if (const auto* node = session_.selectedNode();
        !target && node != nullptr && node->typeId == document::kCryptomatteNodeType)
        matteTarget = ChannelSetTarget{node->id, std::string(document::kCryptomatteMaskOutputPort),
                                       session_.snapshot().revision()};
    const auto effective = target ? target : matteTarget;
    if (effective == listingTarget_)
        return;
    listingTarget_ = effective;
    if (listingTask_) {
        listingTask_->cancel();
        listingTask_.reset();
        listingTaskTarget_.reset();
    }
    if (matteTarget) {
        inspectionChoices_ = {runtime::ChannelInspectionChoice{"Cryptomatte matte", {}}};
        choicesTarget_ = matteTarget;
        rebuildChannelDropdown();
        return;
    }
    if (!target) {
        // Keep the choices of an active inspection so its entry stays visible after deselection.
        if (!previewController_.viewerChannelInspection()) {
            inspectionChoices_.clear();
            choicesTarget_.reset();
            rebuildChannelDropdown();
        }
        return;
    }
    const auto submission = previewController_.submitChannelListing(target->node, target->port);
    if (submission.accepted()) {
        listingTask_ = submission.handle;
        listingTaskTarget_ = target;
    }
}

void ViewerEditor::consumeChannelListing() {
    if (!listingTask_)
        return;
    auto result = listingTask_->tryTakeResult();
    if (!result)
        return;
    const auto target = listingTaskTarget_;
    listingTask_.reset();
    listingTaskTarget_.reset();
    if (target != listingTarget_ || result->state() != runtime::TaskState::Succeeded ||
        !result->value() || !*result->value())
        return;
    inspectionChoices_ = (*result->value())->choices;
    choicesTarget_ = target;
    rebuildChannelDropdown();
}

void ViewerEditor::rebuildChannelDropdown() {
    if (channelDropdown_ == nullptr)
        return;
    const QSignalBlocker blocker(channelDropdown_);
    while (channelDropdown_->count() > kStandardChannelCount) {
        // KDropdown has no single-item removal; rebuild the whole list.
        channelDropdown_->clearItems();
        for (const auto* name : {"RGBA", "RGB", "R", "G", "B", "Alpha"})
            channelDropdown_->addItem(tr(name));
    }
    for (const auto& choice : inspectionChoices_)
        channelDropdown_->addItem(tr("Inspect %1").arg(QString::fromStdString(choice.label)));
    int current = static_cast<int>(channel_);
    if (const auto& inspection = previewController_.viewerChannelInspection();
        inspection && choicesTarget_ && inspection->node == choicesTarget_->node) {
        for (std::size_t index = 0; index < inspectionChoices_.size(); ++index)
            if (inspectionChoices_[index].channels == inspection->channels)
                current = kStandardChannelCount + static_cast<int>(index);
    }
    channelDropdown_->setCurrentIndex(current);
}

void ViewerEditor::chooseChannelDropdownIndex(const int index) {
    if (index < 0)
        return;
    if (index < kStandardChannelCount) {
        previewController_.setViewerChannelInspection(std::nullopt);
        setChannel(static_cast<ViewerChannel>(index));
        return;
    }
    const auto choice = static_cast<std::size_t>(index - kStandardChannelCount);
    if (!choicesTarget_ || choice >= inspectionChoices_.size())
        return;
    // A session display choice: a viewer-only plan shows the channels raw; the project, its
    // compiled plan and every render or export are untouched.
    previewController_.setViewerChannelInspection(runtime::ChannelInspection{
        choicesTarget_->node, choicesTarget_->port, inspectionChoices_[choice].channels});
    setChannel(ViewerChannel::Rgba);
    rebuildChannelDropdown();
}

} // namespace bloom::ui
