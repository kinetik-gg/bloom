// The graph editor's pointer and keyboard: hit testing, the key and handle drags with their live
// preview, box select, zoom and pan. The canvas and its curves live in timeline_graph_view.cpp.

#include <bloom/ui/timeline_graph_view.hpp>

#include "timeline_graph_math.hpp"
#include "timeline_keyframe_time.hpp"

#include <bloom/commands/transaction.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QApplication>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace bloom::ui {
namespace {

// The handle edit for `key` inside `edits`, created on first use, so one gesture writes each key
// once even when it touches both of its sides.
commands::KeyframeHandleEdit& handleEditFor(std::vector<commands::KeyframeHandleEdit>& edits,
                                            const commands::KeyframeAddress& key) {
    const auto found = std::ranges::find(edits, key, &commands::KeyframeHandleEdit::key);
    if (found != edits.end())
        return *found;
    return edits.emplace_back(commands::KeyframeHandleEdit{key});
}

} // namespace

std::vector<GraphCurveId> TimelineGraphView::curvesByPriority() const {
    auto ordered = curves_;
    // The ACTIVE curve is tested first, which is exactly what "the active curve wins ties" means:
    // the first match in this order is the hit.
    if (const auto active = activeCurve(); active.has_value()) {
        const auto at = std::ranges::find(ordered, *active);
        if (at != ordered.end())
            std::rotate(ordered.begin(), at, at + 1);
    }
    return ordered;
}

std::optional<GraphCurveId> TimelineGraphView::curveOf(const KeyframeSelection& key) const {
    const GraphCurveId exact{key.curveId, key.component};
    if (std::ranges::find(curves_, exact) != curves_.end())
        return exact;
    return std::nullopt;
}

std::optional<GraphHit> TimelineGraphView::hitTest(const QPointF position) const {
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value() || position.x() < gutterWidth() ||
        position.y() < kit::px(kit::Size::HeaderRow))
        return std::nullopt;
    const double tolerance = kit::px(kit::Spacing::S);
    const auto& selected = session_.selection().keyframes;
    const auto ordered = curvesByPriority();

    // 1. The handles of SELECTED keys. They are small and they sit on top of everything else, so
    // testing them first is what makes them grabbable at all.
    for (const auto& curve : ordered) {
        const auto keys = keysFor(curve);
        for (std::size_t index = 0; index < keys.size(); ++index) {
            const KeyframeSelection address{curve.curveId, keys[index].id, curve.component};
            if (std::ranges::find(selected, address) == selected.end())
                continue;
            for (const bool outgoing : {true, false})
                if (const auto handle = handlePoint(curve, keys, index, outgoing);
                    handle.has_value() && QLineF(*handle, position).length() <= tolerance)
                    return GraphHit{GraphHit::Kind::Handle, curve, keys[index].id, outgoing,
                                    keys[index].time};
        }
    }

    // 2. The keys themselves.
    for (const auto& curve : ordered) {
        const auto keys = keysFor(curve);
        for (std::size_t index = 0; index < keys.size(); ++index)
            if (QLineF(keyPoint(curve, keys, index), position).length() <= tolerance)
                return GraphHit{GraphHit::Kind::Key, curve, keys[index].id, false,
                                keys[index].time};
    }

    // 3. The curve itself, through the SAMPLER rather than a second shape: what is hit is exactly
    // what is drawn.
    const double seconds = axis->secondsForPixel(position.x());
    const auto pointerTime = keyPointerOffset(seconds);
    if (!pointerTime.has_value())
        return std::nullopt;
    for (const auto& curve : ordered) {
        const auto keys = keysFor(curve);
        if (keys.size() < 2 || *pointerTime < keys.front().time || *pointerTime > keys.back().time)
            continue;
        if (std::abs(lensFor(curve).pixelFor(plottedAt(curve, seconds)) - position.y()) <=
            tolerance)
            return GraphHit{GraphHit::Kind::Segment, curve, keys.front().id, false, *pointerTime};
    }
    return std::nullopt;
}

void TimelineGraphView::cancelGesture() {
    gesture_ = Gesture::None;
    dragging_ = false;
    copying_ = false;
    pressed_.reset();
    gestureKeys_.clear();
    gestureData_.clear();
    pendingMoves_.clear();
    pendingValues_.clear();
    pendingHandles_.clear();
    frozenNormalization_.clear();
    pressedSpeeds_.clear();
    box_.reset();
    snapGuide_.reset();
    session_.clearKeyframePreview();
    update();
}

double TimelineGraphView::pressedSpeed(const KeyframeSelection& key, const bool outgoing) const {
    for (const auto& pressed : pressedSpeeds_)
        if (pressed.key == key && pressed.outgoing == outgoing)
            return pressed.speed;
    return 0.0;
}

void TimelineGraphView::previewGesture() {
    // The exact operations the release commits, so what the artist watches IS the edit.
    commands::Transaction transaction("Preview Keyframes", gestureRevision_);
    const auto composition = session_.compositionId();
    if (!pendingHandles_.empty()) {
        transaction.emplace<commands::SetKeyframeHandles>(composition, pendingHandles_);
    } else if (copying_) {
        auto data = gestureData_;
        for (std::size_t index = 0; index < data.size() && index < gestureKeys_.size(); ++index) {
            const auto& key = gestureKeys_[index];
            for (const auto& move : pendingMoves_)
                if (move.key.keyframeId == key.keyframeId && move.key.curveId == key.curveId)
                    data[index].time = move.time;
            for (const auto& edit : pendingValues_)
                if (edit.key.keyframeId == key.keyframeId && edit.key.curveId == key.curveId)
                    data[index].value = edit.value;
        }
        if (!pendingMoves_.empty() || !pendingValues_.empty())
            transaction.emplace<commands::PasteKeyframes>(composition, std::move(data));
    } else {
        if (!pendingMoves_.empty())
            transaction.emplace<commands::MoveKeyframes>(composition, pendingMoves_);
        if (!pendingValues_.empty())
            transaction.emplace<commands::SetKeyframeValues>(composition, pendingValues_);
    }
    if (transaction.empty())
        session_.clearKeyframePreview();
    else
        (void)session_.previewKeyframeTransaction(transaction);
    update();
}

void TimelineGraphView::updateKeyDrag(const QMouseEvent& event) {
    const auto axis = ruler_.axisForWidth(width());
    const auto* composition = session_.composition();
    if (!axis.has_value() || composition == nullptr || gestureData_.empty() ||
        !pressed_.has_value())
        return;
    const auto pressedTime = pressed_->time;
    pendingMoves_.clear();
    pendingValues_.clear();
    pendingHandles_.clear();
    snapGuide_.reset();

    const double dx = event.position().x() - press_.x();
    const double dy = event.position().y() - press_.y();
    // Shift locks the dominant axis of the whole displacement. The Speed Graph always does: a
    // speed edit rewrites handles against the keys' own durations, so it never rides a time move.
    const auto axisLock =
        dragAxis(dx, dy,
                 event.modifiers().testFlag(Qt::ShiftModifier) || graphType_ == GraphType::Speed);
    const bool moveTime = axisLock != GraphDragAxis::ValueOnly;
    const bool moveValue = axisLock != GraphDragAxis::TimeOnly;
    if (graphType_ == GraphType::Speed && moveValue) {
        updateSpeedKeyDrag(dy);
        return;
    }

    // TIME. The same helpers, the same snap targets and the same clamp the lane drag uses -- a key
    // must not move differently because the artist is looking at a curve.
    double targetSeconds = pressedTime.toSeconds();
    if (moveTime) {
        targetSeconds +=
            axis->secondsForPixel(event.position().x()) - axis->secondsForPixel(press_.x());
        if (snapping_) {
            std::vector<core::RationalTime> targets{
                session_.currentTime(), session_.workArea().start, session_.workArea().end};
            for (const auto& curve : curves_)
                for (const auto& key : keysFor(curve, false))
                    if (std::ranges::find(gestureKeys_, KeyframeSelection{curve.curveId, key.id,
                                                                          curve.component}) ==
                        gestureKeys_.end())
                        targets.push_back(key.time);
            qreal distance = kit::px(kit::Spacing::S);
            for (const auto target : targets) {
                const auto delta =
                    std::abs(axis->pixelForSeconds(targetSeconds) - axis->pixelForTime(target));
                if (delta <= distance) {
                    distance = delta;
                    snapGuide_ = target;
                }
            }
            if (snapGuide_.has_value())
                targetSeconds = snapGuide_->toSeconds();
        }
    }
    const auto requestedTarget =
        snapGuide_.has_value()
            ? snapGuide_
            : (snapping_ && moveTime
                   ? frameTimeForIndex(axis->frameRate, axis->duration,
                                       axis->frameIndexForPixel(static_cast<int>(
                                           std::lround(axis->pixelForSeconds(targetSeconds)))))
                   : keyPointerOffset(targetSeconds));
    if (!requestedTarget.has_value())
        return;
    const auto [firstKey, lastKey] = std::minmax_element(
        gestureData_.begin(), gestureData_.end(),
        [](const auto& left, const auto& right) { return left.time < right.time; });
    const auto epsilon = core::RationalTime::create(-1, 1'000'000'000);
    const auto lastInstant =
        epsilon.has_value() ? offsetKeyTime(composition->duration(), *epsilon) : std::nullopt;
    const auto minimum = keyTimeDifference(core::RationalTime{}, firstKey->time);
    const auto maximum =
        lastInstant.has_value() ? keyTimeDifference(*lastInstant, lastKey->time) : std::nullopt;
    const auto requested = keyTimeDifference(*requestedTarget, pressedTime);
    if (!requested.has_value() || !minimum.has_value() || !maximum.has_value() ||
        *minimum > *maximum)
        return;
    const auto offset = moveTime ? std::clamp(*requested, *minimum, *maximum)
                                 : core::RationalTime{};
    if (moveTime && offset != *requested)
        snapGuide_.reset();

    for (std::size_t index = 0; index < gestureData_.size(); ++index) {
        const auto time = offsetKeyTime(gestureData_[index].time, offset);
        if (!time.has_value()) {
            pendingMoves_.clear();
            pendingValues_.clear();
            previewGesture();
            return;
        }
        const auto& key = gestureKeys_[index];
        if (moveTime && *time != gestureData_[index].time)
            pendingMoves_.push_back({{key.curveId, key.keyframeId, key.component}, *time});
        if (!moveValue)
            continue;
        const auto curve = curveOf(key);
        const auto* base = std::get_if<double>(&gestureData_[index].value);
        if (!curve.has_value() || base == nullptr)
            continue;
        // A pixel up adds the lens's units-per-pixel, normalized or not: Normalize changes what is
        // drawn, never the unit an edit is written in.
        const double moved = *base - dy * lensFor(*curve).quantityPerPixel();
        // A value viewport that spans an overflowed range makes `moved` an infinity. Queuing it
        // would cost the whole drag at commit, so a non-representable value is simply not offered.
        if (std::isfinite(moved) && moved != *base)
            pendingValues_.push_back({{key.curveId, key.keyframeId, key.component}, moved});
    }
    previewGesture();
}

void TimelineGraphView::updateSpeedKeyDrag(const double dy) {
    // A Speed Graph key moved vertically changes the speed on both of its sides by the pointer's
    // travel, at each side's own influence. A side whose segment is not yet a Bezier becomes one,
    // and the segment's far end keeps its straight-line tangent so only this key's speed changes.
    for (const auto& key : gestureKeys_) {
        const auto curve = curveOf(key);
        if (!curve.has_value())
            continue;
        const auto keys = keysFor(*curve, false);
        const auto at = std::ranges::find(keys, key.keyframeId, &document::ScalarKeyframe::id);
        if (at == keys.end())
            continue;
        const auto index = static_cast<std::size_t>(at - keys.begin());
        const double delta = -dy * lensFor(*curve).quantityPerPixel();
        const commands::KeyframeAddress address{key.curveId, key.keyframeId, key.component};
        for (const bool outgoing : {true, false}) {
            if (outgoing ? index + 1 >= keys.size() : index == 0)
                continue;
            const auto& left = outgoing ? keys[index] : keys[index - 1];
            if (left.outgoingInterpolation == document::KeyframeInterpolation::Hold)
                continue;
            const auto& neighbour = outgoing ? keys[index + 1] : keys[index - 1];
            const double duration = std::abs(neighbour.time.toSeconds() - at->time.toSeconds());
            const bool eased =
                left.outgoingInterpolation == document::KeyframeInterpolation::EaseInOut;
            const double influence = eased ? (outgoing ? at->outgoingHandle.time
                                                       : at->incomingHandle.time)
                                           : document::kDefaultKeyframeHandleTime;
            const double speed = pressedSpeed(key, outgoing) + delta;
            const double value = handleValueForSpeed(speed, influence, duration, outgoing);
            if (!std::isfinite(value))
                continue;
            auto& edit = handleEditFor(pendingHandles_, address);
            (outgoing ? edit.outgoing : edit.incoming) = document::KeyframeHandle{influence, value};
            const KeyframeSelection far{key.curveId, neighbour.id, key.component};
            if (!eased && std::ranges::find(gestureKeys_, far) == gestureKeys_.end()) {
                const auto& start = outgoing ? *at : neighbour;
                const auto& end = outgoing ? neighbour : *at;
                auto& farEdit = handleEditFor(
                    pendingHandles_, {key.curveId, neighbour.id, key.component});
                (outgoing ? farEdit.incoming : farEdit.outgoing) =
                    linearTangentHandle(start.value, end.value, !outgoing);
            }
        }
    }
    previewGesture();
}

void TimelineGraphView::updateHandleDrag(const QMouseEvent& event) {
    if (graphType_ == GraphType::Speed) {
        updateSpeedHandleDrag(event);
        return;
    }
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value() || !pressed_.has_value())
        return;
    pendingHandles_.clear();
    const auto& curve = pressed_->curve;
    const auto keys = keysFor(curve, false);
    const auto at = std::ranges::find(keys, pressed_->keyframeId, &document::ScalarKeyframe::id);
    if (at == keys.end())
        return;
    const auto index = static_cast<std::size_t>(at - keys.begin());
    const bool outgoing = pressed_->outgoing;
    if ((outgoing && index + 1 >= keys.size()) || (!outgoing && index == 0))
        return;
    const auto& neighbour = outgoing ? keys[index + 1] : keys[index - 1];
    const double segment = std::abs(neighbour.time.toSeconds() - at->time.toSeconds());
    if (!(segment > 0.0))
        return;

    const auto lens = lensFor(curve);
    QPointF pointer = event.position();
    if (event.modifiers().testFlag(Qt::ShiftModifier)) {
        // Shift snaps the handle's on-screen angle to 45 degree steps, keeping its length along
        // the snapped direction: the angle the artist sees is the one that snaps.
        const QPointF key(axis->pixelForTime(at->time), lens.pixelFor(at->value));
        const auto snapped = snapHandleAngle({pointer.x() - key.x(), pointer.y() - key.y()},
                                             outgoing ? 1.0 : -1.0);
        pointer = key + QPointF(snapped.dx, snapped.dy);
    }
    const double pointerSeconds = axis->secondsForPixel(pointer.x());
    const double pointerValue = lens.quantityFor(pointer.y());
    const double deltaTime =
        outgoing ? pointerSeconds - at->time.toSeconds() : at->time.toSeconds() - pointerSeconds;
    const document::KeyframeHandle dragged{std::clamp(deltaTime / segment, 0.0, 1.0),
                                           pointerValue - at->value};
    if (!std::isfinite(dragged.time) || !std::isfinite(dragged.value))
        return;

    commands::KeyframeHandleEdit edit{{curve.curveId, at->id, curve.component}};
    if (outgoing)
        edit.outgoing = dragged;
    else
        edit.incoming = dragged;

    // Mirrored slope by default: the key keeps ONE tangent, so the opposite control point stays
    // collinear with the key and the dragged one while keeping its own time length. Alt breaks the
    // pair, which is the only way to author a corner.
    const double draggedRun = (outgoing ? 1.0 : -1.0) * dragged.time * segment;
    const bool oppositeEased =
        outgoing ? (index > 0 && keys[index - 1].outgoingInterpolation ==
                                     document::KeyframeInterpolation::EaseInOut)
                 : (index + 1 < keys.size() &&
                    at->outgoingInterpolation == document::KeyframeInterpolation::EaseInOut);
    if (!event.modifiers().testFlag(Qt::AltModifier) && oppositeEased && draggedRun != 0.0) {
        const auto& oppositeNeighbour = outgoing ? keys[index - 1] : keys[index + 1];
        const double oppositeSegment =
            std::abs(oppositeNeighbour.time.toSeconds() - at->time.toSeconds());
        const double oppositeTime = outgoing ? at->incomingHandle.time : at->outgoingHandle.time;
        const double oppositeRun = (outgoing ? -1.0 : 1.0) * oppositeTime * oppositeSegment;
        const document::KeyframeHandle mirrored{oppositeTime,
                                                (dragged.value / draggedRun) * oppositeRun};
        // `draggedRun != 0.0` above is a presence test, not a magnitude one: a handle time just
        // off zero makes the run denormal and the mirrored offset overflow. The mirror is applied
        // only when it is representable; otherwise the dragged handle stands on its own.
        if (std::isfinite(mirrored.value)) {
            if (outgoing)
                edit.incoming = mirrored;
            else
                edit.outgoing = mirrored;
        }
    }
    pendingHandles_.push_back(edit);
    previewGesture();
}

void TimelineGraphView::updateSpeedHandleDrag(const QMouseEvent& event) {
    // The Speed Graph's handle is After Effects' influence/speed handle: horizontal travel is
    // influence, vertical travel is speed, both written back as the same Bezier handle the Value
    // Graph edits. Shift locks to whichever of the two dominates.
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value() || !pressed_.has_value())
        return;
    pendingHandles_.clear();
    const auto& curve = pressed_->curve;
    const auto keys = keysFor(curve, false);
    const auto at = std::ranges::find(keys, pressed_->keyframeId, &document::ScalarKeyframe::id);
    if (at == keys.end())
        return;
    const auto index = static_cast<std::size_t>(at - keys.begin());
    const bool outgoing = pressed_->outgoing;
    if ((outgoing && index + 1 >= keys.size()) || (!outgoing && index == 0))
        return;
    const auto& neighbour = outgoing ? keys[index + 1] : keys[index - 1];
    const double duration = std::abs(neighbour.time.toSeconds() - at->time.toSeconds());
    if (!(duration > 0.0))
        return;
    const auto base = outgoing ? at->outgoingHandle : at->incomingHandle;
    const double baseSpeed = handleSpeed(base, duration, outgoing).value_or(0.0);
    const auto lock = dragAxis(event.position().x() - press_.x(), event.position().y() - press_.y(),
                               event.modifiers().testFlag(Qt::ShiftModifier));
    const double pointerSeconds = axis->secondsForPixel(event.position().x());
    const double reach =
        outgoing ? pointerSeconds - at->time.toSeconds() : at->time.toSeconds() - pointerSeconds;
    const double influence = lock == GraphDragAxis::ValueOnly
                                 ? base.time
                                 : std::clamp(reach / duration, kMinimumHandleInfluence, 1.0);
    const double speed = lock == GraphDragAxis::TimeOnly
                             ? baseSpeed
                             : lensFor(curve).quantityFor(event.position().y());
    const document::KeyframeHandle dragged{
        influence, handleValueForSpeed(speed, influence, duration, outgoing)};
    if (!std::isfinite(dragged.time) || !std::isfinite(dragged.value))
        return;
    commands::KeyframeHandleEdit edit{{curve.curveId, at->id, curve.component}};
    (outgoing ? edit.outgoing : edit.incoming) = dragged;
    // Continuous by default, as in the Value Graph: the other side takes the same speed at its own
    // influence. Alt breaks the pair.
    const bool oppositeEased =
        outgoing ? (index > 0 && keys[index - 1].outgoingInterpolation ==
                                     document::KeyframeInterpolation::EaseInOut)
                 : (index + 1 < keys.size() &&
                    at->outgoingInterpolation == document::KeyframeInterpolation::EaseInOut);
    if (!event.modifiers().testFlag(Qt::AltModifier) && oppositeEased) {
        const auto& opposite = outgoing ? keys[index - 1] : keys[index + 1];
        const double oppositeDuration = std::abs(opposite.time.toSeconds() - at->time.toSeconds());
        const auto oppositeHandle = outgoing ? at->incomingHandle : at->outgoingHandle;
        const document::KeyframeHandle mirrored{
            oppositeHandle.time,
            handleValueForSpeed(speed, oppositeHandle.time, oppositeDuration, !outgoing)};
        if (std::isfinite(mirrored.value))
            (outgoing ? edit.incoming : edit.outgoing) = mirrored;
    }
    pendingHandles_.push_back(edit);
    previewGesture();
}

void TimelineGraphView::commitGesture() {
    const auto revision = gestureRevision_;
    auto moves = pendingMoves_;
    auto values = pendingValues_;
    auto handles = pendingHandles_;
    auto data = gestureData_;
    auto keys = gestureKeys_;
    const bool copy = copying_;
    // The preview ends before the commit, so the one transaction below is the only change the
    // document and the history ever see.
    cancelGesture();
    if (!handles.empty()) {
        (void)session_.setKeyframeHandles(std::move(handles), revision);
        return;
    }
    if (moves.empty() && values.empty())
        return;
    if (copy) {
        // A copy-drag leaves the originals alone and pastes the gesture's own data -- values,
        // interpolation and ease handles included -- at the times and values the drag landed on.
        for (std::size_t index = 0; index < data.size() && index < keys.size(); ++index) {
            for (const auto& move : moves)
                if (move.key.keyframeId == keys[index].keyframeId &&
                    move.key.curveId == keys[index].curveId)
                    data[index].time = move.time;
            for (const auto& edit : values)
                if (edit.key.keyframeId == keys[index].keyframeId &&
                    edit.key.curveId == keys[index].curveId)
                    data[index].value = edit.value;
        }
        (void)session_.pasteKeyframes(data, revision);
        return;
    }
    // ONE transaction: a diagonal drag is one gesture and therefore one undo entry. A domain
    // refusal publishes nothing and leaves the selection exactly where it was.
    (void)session_.moveKeyframesAndValues(std::move(moves), std::move(values), revision);
}

void TimelineGraphView::zoomValues(const double factor, const double anchorPixelY) {
    if (normalized_) {
        // One shared -1...1 window: zoom it once, about the display value under the pointer.
        const auto viewport = curves_.empty() ? normalizedViewport() : viewportFor(curves_.front());
        normalizedViewports_[graphType_] = zoomValueViewport(
            viewport, factor, valueForPixel(viewport, anchorPixelY, bandTop(), bandHeight()));
    } else {
        for (const auto& curve : curves_) {
            const auto viewport = viewportFor(curve);
            setViewport(curve, zoomValueViewport(viewport, factor,
                                                 valueForPixel(viewport, anchorPixelY, bandTop(),
                                                               bandHeight())));
        }
    }
    update();
}

void TimelineGraphView::panValues(const double pixels) {
    if (normalized_) {
        const auto viewport = curves_.empty() ? normalizedViewport() : viewportFor(curves_.front());
        normalizedViewports_[graphType_] =
            panValueViewport(viewport, pixels * viewport.span() / bandHeight());
    } else {
        for (const auto& curve : curves_) {
            const auto viewport = viewportFor(curve);
            setViewport(curve, panValueViewport(viewport, pixels * viewport.span() / bandHeight()));
        }
    }
    update();
}

void TimelineGraphView::wheelEvent(QWheelEvent* event) {
    // Ctrl, Shift and a horizontal wheel are the TIME axis, and the ruler owns that -- there is no
    // second copy of the time zoom here. A plain vertical wheel is the one gesture the graph adds.
    if (ruler_.handleWheel(event))
        return;
    const int angle = event->angleDelta().y();
    if (angle == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    zoomValues(std::pow(1.25, -angle / 120.0), event->position().y());
    event->accept();
}

void TimelineGraphView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        panOrigin_ = event->position();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    cancelGesture();
    setFocus(Qt::MouseFocusReason);
    press_ = event->position();
    gestureRevision_ = session_.snapshot().revision();
    pressed_ = hitTest(press_);
    // Normalize holds still for the whole drag: rescaling under the pointer as the curve's own
    // extent changes would make the key run away from it.
    if (normalized_)
        for (const auto& curve : curves_)
            frozenNormalization_.emplace(curve, normalizationOf(curve));
    const bool extend = event->modifiers().testFlag(Qt::ShiftModifier);
    if (pressed_.has_value() && pressed_->kind == GraphHit::Kind::Handle) {
        gesture_ = Gesture::Handle;
        event->accept();
        return;
    }
    if (pressed_.has_value() && pressed_->kind == GraphHit::Kind::Key) {
        const KeyframeSelection address{pressed_->curve.curveId, pressed_->keyframeId,
                                        pressed_->curve.component};
        const auto& selected = session_.selection().keyframes;
        if (extend || std::ranges::find(selected, address) == selected.end()) {
            // The component overloads, always: naming the component is half the address, and
            // this view never has a key in hand whose component it does not already know.
            if (address.component.has_value())
                session_.selectKeyframe(address.curveId, *address.component, address.keyframeId,
                                        extend);
            else
                session_.selectKeyframe(address.curveId, address.keyframeId, extend);
        }
        gesture_ = Gesture::Keys;
        gestureKeys_ = session_.selection().keyframes;
        gestureData_ = session_.selectedKeyframeData();
        if (graphType_ == GraphType::Speed)
            for (std::size_t index = 0; index < gestureKeys_.size() && index < gestureData_.size();
                 ++index)
                if (const auto curve = curveOf(gestureKeys_[index]))
                    for (const bool outgoing : {true, false})
                        pressedSpeeds_.push_back(
                            {gestureKeys_[index], outgoing,
                             plottedAt(*curve, gestureData_[index].time.toSeconds(), outgoing)});
        copying_ = event->modifiers().testFlag(Qt::AltModifier);
        event->accept();
        return;
    }
    gesture_ = Gesture::Box;
    gestureKeys_ = extend ? session_.selection().keyframes : std::vector<KeyframeSelection>{};
    box_ = QRectF(press_, press_);
    event->accept();
}

void TimelineGraphView::mouseMoveEvent(QMouseEvent* event) {
    if (panOrigin_.has_value()) {
        // The hand pans both axes at once: time through the ruler's own range, values here.
        const auto delta = event->position() - *panOrigin_;
        if (const auto axis = ruler_.axisForWidth(width()); axis.has_value() && delta.x() != 0.0) {
            const double shift = delta.x() * (axis->t1 - axis->t0) / axis->pixelSpan();
            frameTimes(axis->t0 - shift, axis->t1 - shift);
        }
        panValues(delta.y());
        panOrigin_ = event->position();
        event->accept();
        return;
    }
    if (gesture_ == Gesture::None) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    if (!dragging_ &&
        (event->position() - press_).manhattanLength() < QApplication::startDragDistance())
        return;
    dragging_ = true;
    switch (gesture_) {
    case Gesture::Box:
        box_ = QRectF(press_, event->position()).normalized();
        update();
        break;
    case Gesture::Keys:
        updateKeyDrag(*event);
        break;
    case Gesture::Handle:
        updateHandleDrag(*event);
        break;
    case Gesture::None:
        break;
    }
    event->accept();
}

void TimelineGraphView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton && panOrigin_.has_value()) {
        panOrigin_.reset();
        event->accept();
        return;
    }
    if (gesture_ == Gesture::None || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (session_.snapshot().revision() != gestureRevision_) {
        cancelGesture();
        event->accept();
        return;
    }
    if (gesture_ == Gesture::Box) {
        auto keys = gestureKeys_;
        if (dragging_ && box_.has_value())
            for (const auto& curve : curves_) {
                const auto curveKeys = keysFor(curve);
                for (std::size_t index = 0; index < curveKeys.size(); ++index) {
                    const KeyframeSelection address{curve.curveId, curveKeys[index].id,
                                                    curve.component};
                    if (box_->contains(keyPoint(curve, curveKeys, index)) &&
                        std::ranges::find(keys, address) == keys.end())
                        keys.push_back(address);
                }
            }
        cancelGesture();
        session_.selectKeyframes(keys);
        event->accept();
        return;
    }
    if (!dragging_) {
        cancelGesture();
        event->accept();
        return;
    }
    commitGesture();
    event->accept();
}

void TimelineGraphView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->position().x() < gutterWidth()) {
        fitCurves();
        event->accept();
        return;
    }
    const auto hit = hitTest(event->position());
    cancelGesture();
    if (!hit.has_value() || hit->kind != GraphHit::Kind::Segment) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    // A key inserted on a segment is valued at the curve's OWN sampled value there, so the picture
    // does not move -- the same promise the diamond gesture makes.
    const auto axis = ruler_.axisForWidth(width());
    const auto* composition = session_.composition();
    if (!axis.has_value() || composition == nullptr)
        return;
    const auto snapped =
        snapping_ ? frameTimeForIndex(axis->frameRate, axis->duration,
                                      axis->frameIndexForPixel(
                                          static_cast<int>(std::lround(event->position().x()))))
                  : keyPointerOffset(axis->secondsForPixel(event->position().x()));
    if (!snapped.has_value())
        return;
    if (hit->curve.component.has_value()) {
        const auto owner = owners_.find(hit->curve.curveId);
        if (owner != owners_.end())
            (void)session_.toggleKeyframe(owner->second, *hit->curve.component, *snapped);
    } else {
        (void)session_.insertKeyframeAtTime(hit->curve.curveId, *snapped);
    }
    event->accept();
}

void TimelineGraphView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_F) {
        fitSelection();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        cancelGesture();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Copy)) {
        session_.copySelectedKeyframes();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Paste)) {
        (void)session_.pasteCopiedKeyframes();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        (void)session_.deleteSelectedKeyframes();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace bloom::ui
