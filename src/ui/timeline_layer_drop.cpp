// #380: where an Assets or file drop on the Timeline lands, as in After Effects. Over the layer
// stack a drop inserts at the row under the pointer and starts at the playhead; over the lanes it
// inserts at that row and starts at the frame under the pointer; anywhere else in the panel it
// goes on top at the playhead.
#include "layer_drop.hpp"
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>

namespace bloom::ui {

LayerDropHit TimelineEditor::layerDropHit(const QPoint position) const {
    LayerDropHit hit;
    const auto* composition = session_.composition();
    if (composition == nullptr)
        return hit;
    const auto inStack = stack_->rect().contains(stack_->mapFrom(this, position));
    const auto lanePosition = lanes_->mapFrom(this, position);
    const auto inLanes = lanes_->isVisible() && lanes_->rect().contains(lanePosition);
    if (!inStack && !inLanes)
        return hit;
    // Both halves share one row grid; the stack's coordinates name the row either way.
    hit.row = stack_->insertionRowAt(stack_->mapFrom(this, position).y());
    const auto& entries = stack_->entries();
    if (hit.row < static_cast<int>(entries.size())) {
        hit.stack = LayerDropPlacement::Stack::Before;
        hit.before = entries[static_cast<std::size_t>(hit.row)].slotId;
    } else {
        hit.stack = LayerDropPlacement::Stack::Bottom;
    }
    if (inLanes)
        if (const auto axis = ruler_->axisForWidth(lanes_->width()))
            hit.time = frameTimeForIndex(composition->format().frameRate(), composition->duration(),
                                         axis->frameIndexForPixel(lanePosition.x()));
    return hit;
}

void TimelineEditor::indicateLayerDrop(const std::optional<LayerDropHit>& hit) {
    const auto row = hit ? hit->row : -1;
    stack_->showInsertionIndicator(row);
    lanes_->setDropMarker(hit ? hit->time : std::nullopt, row);
}

} // namespace bloom::ui
