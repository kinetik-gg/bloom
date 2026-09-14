#include "timeline_keyframe_time.hpp"
#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <bloom/document/project.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>
#include <cmath>
#include <memory>

namespace bloom::ui {
std::vector<TimelineKeyframePanel::LaneKey> TimelineKeyframePanel::laneKeys() const {
    std::vector<LaneKey> keys;
    const auto* composition = session_.composition();
    if (!composition)
        return keys;
    for (std::size_t i = 0; i < gridParameters_.size(); ++i) {
        const auto* parameter = composition->parameters().find(gridParameters_[i]);
        const auto* source =
            parameter ? std::get_if<document::AnimationCurveSource>(&parameter->source) : nullptr;
        const auto* record =
            source ? composition->animationCurves().find(source->curveId) : nullptr;
        if (!record)
            continue;
        std::visit(
            [&](const auto& curve) {
                for (const auto& key : curve.keyframes)
                    keys.push_back(
                        {{curve.id, key.id}, key.time, key.outgoingInterpolation, gridRows_[i]});
            },
            *record);
    }
    return keys;
}
std::optional<TimelineKeyframePanel::LaneKey>
TimelineKeyframePanel::hitKey(QPointF position) const {
    const auto axis = ruler_ ? ruler_->axisForWidth(width()) : std::nullopt;
    if (!axis)
        return std::nullopt;
    const int row = static_cast<int>(position.y() + gridScroll_) / kTimelineRowHeight;
    std::optional<LaneKey> closest;
    qreal distance = kit::px(kit::Spacing::S);
    for (const auto& key : laneKeys()) {
        if (key.row != row || key.time.toSeconds() < axis->t0 || key.time.toSeconds() >= axis->t1)
            continue;
        const auto dx = std::abs(axis->pixelForTime(key.time) - position.x());
        if (dx <= distance) {
            closest = key;
            distance = dx;
        }
    }
    return closest;
}
bool TimelineKeyframePanel::event(QEvent* event) {
    if (gridMode_ && event->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace ||
            key->key() == Qt::Key_Escape || key->matches(QKeySequence::Copy) ||
            key->matches(QKeySequence::Paste)) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}
bool TimelineKeyframePanel::eventFilter(QObject* watched, QEvent* event) {
    if (!gridMode_)
        return QWidget::eventFilter(watched, event);
    auto* row = qobject_cast<QWidget*>(watched);
    if (!row)
        return false;
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonDblClick) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        QMouseEvent mapped(mouse->type(), row->mapTo(this, mouse->position()),
                           mouse->globalPosition(), mouse->button(), mouse->buttons(),
                           mouse->modifiers());
        QApplication::sendEvent(this, &mapped);
        return true;
    }
    if (event->type() == QEvent::ContextMenu) {
        const auto* context = static_cast<QContextMenuEvent*>(event);
        QContextMenuEvent mapped(context->reason(), row->mapTo(this, context->pos()),
                                 context->globalPos(), context->modifiers());
        contextMenuEvent(&mapped);
        return true;
    }
    if (event->type() == QEvent::Wheel) {
        wheelEvent(static_cast<QWheelEvent*>(event));
        return event->isAccepted();
    }
    return QWidget::eventFilter(watched, event);
}
void TimelineKeyframePanel::mousePressEvent(QMouseEvent* event) {
    if (!gridMode_ || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    cancelGesture();
    setFocus(Qt::MouseFocusReason);
    press_ = event->position();
    pressed_ = hitKey(press_);
    gestureRevision_ = session_.snapshot().revision();
    if (pressed_) {
        const auto selected = session_.selection().keyframes;
        if (event->modifiers().testFlag(Qt::ShiftModifier) ||
            std::ranges::find(selected, pressed_->selection) == selected.end())
            session_.selectKeyframe(pressed_->selection.curveId, pressed_->selection.keyframeId,
                                    event->modifiers().testFlag(Qt::ShiftModifier));
        gestureKeys_ = session_.selection().keyframes;
        gestureData_ = session_.selectedKeyframeData();
        copying_ = event->modifiers().testFlag(Qt::AltModifier) && gestureKeys_.size() == 1;
        if (event->modifiers().testFlag(Qt::AltModifier) && gestureData_.size() >= 2 &&
            std::ranges::all_of(gestureData_, [&](const auto& key) {
                return key.parameterId == gestureData_.front().parameterId;
            })) {
            const auto [first, last] =
                std::minmax_element(gestureData_.begin(), gestureData_.end(),
                                    [](const auto& a, const auto& b) { return a.time < b.time; });
            if (pressed_->time == first->time)
                stretchAnchor_ = last->time;
            else if (pressed_->time == last->time)
                stretchAnchor_ = first->time;
        }
    } else {
        boxing_ = true;
        gestureKeys_ = event->modifiers().testFlag(Qt::ShiftModifier)
                           ? session_.selection().keyframes
                           : std::vector<KeyframeSelection>{};
        box_ = QRectF(press_, press_);
    }
    event->accept();
}
void TimelineKeyframePanel::mouseMoveEvent(QMouseEvent* event) {
    if (!gridMode_ || (!pressed_ && !boxing_)) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    if (session_.snapshot().revision() != gestureRevision_) {
        cancelGesture();
        return;
    }
    if (!dragging_ &&
        (event->position() - press_).manhattanLength() < QApplication::startDragDistance())
        return;
    dragging_ = true;
    const auto axis = ruler_ ? ruler_->axisForWidth(width()) : std::nullopt;
    if (!axis)
        return;
    if (boxing_) {
        box_ = QRectF(press_, event->position()).normalized();
        updateRows();
        return;
    }
    if (gestureData_.size() != gestureKeys_.size() || gestureData_.empty())
        return;
    const auto rate = axis->frameRate;
    const auto duration = axis->duration;
    const auto lead = nearestFrameIndexForTime(rate, duration, pressed_->time);
    if (!lead)
        return;
    double targetSeconds = pressed_->time.toSeconds() +
                           axis->secondsForPixel(event->position().x()) -
                           axis->secondsForPixel(press_.x());
    snapGuide_.reset();
    if (!event->modifiers().testFlag(Qt::ShiftModifier)) {
        auto targets = std::vector<core::RationalTime>{
            session_.currentTime(), session_.workArea().start, session_.workArea().end};
        if (const auto* composition = session_.composition())
            for (const auto& record : composition->animationCurves().records())
                std::visit(
                    [&](const auto& curve) {
                        for (const auto& key : curve.keyframes)
                            if (std::ranges::find(gestureKeys_,
                                                  KeyframeSelection{curve.id, key.id}) ==
                                gestureKeys_.end())
                                targets.push_back(key.time);
                    },
                    record);
        qreal distance = kit::px(kit::Spacing::S);
        for (const auto target : targets) {
            const auto dx =
                std::abs(axis->pixelForSeconds(targetSeconds) - axis->pixelForTime(target));
            if (dx <= distance) {
                distance = dx;
                snapGuide_ = target;
            }
        }
        if (snapGuide_)
            targetSeconds = snapGuide_->toSeconds();
    }
    if (event->modifiers().testFlag(Qt::ShiftModifier) && !stretchAnchor_) {
        const auto [first, last] =
            std::minmax_element(gestureData_.begin(), gestureData_.end(),
                                [](const auto& a, const auto& b) { return a.time < b.time; });
        const double deltaSeconds =
            std::clamp(targetSeconds - pressed_->time.toSeconds(), -first->time.toSeconds(),
                       std::max(0.0, duration.toSeconds() - last->time.toSeconds() - 1e-9));
        const auto offset = keyPointerOffset(deltaSeconds);
        moves_.clear();
        if (!offset)
            return;
        for (std::size_t i = 0; i < gestureData_.size(); ++i) {
            const auto time = offsetKeyTime(gestureData_[i].time, *offset);
            if (!time) {
                moves_.clear();
                return;
            }
            moves_.push_back({{gestureKeys_[i].curveId, gestureKeys_[i].keyframeId}, *time});
        }
        updateRows();
        event->accept();
        return;
    }
    const auto targetIndex = axis->frameIndexForPixel(
        static_cast<int>(std::lround(axis->pixelForSeconds(targetSeconds))));
    if (!stretchAnchor_) {
        const auto target =
            snapGuide_ ? snapGuide_ : frameTimeForIndex(rate, duration, targetIndex);
        const auto [firstKey, lastKey] =
            std::minmax_element(gestureData_.begin(), gestureData_.end(),
                                [](const auto& a, const auto& b) { return a.time < b.time; });
        const auto epsilon = core::RationalTime::create(-1, 1'000'000'000);
        const auto lastInstant = epsilon ? offsetKeyTime(duration, *epsilon) : std::nullopt;
        const auto minimum = keyTimeDifference(core::RationalTime{}, firstKey->time);
        const auto maximum =
            lastInstant ? keyTimeDifference(*lastInstant, lastKey->time) : std::nullopt;
        const auto requested = target ? keyTimeDifference(*target, pressed_->time) : std::nullopt;
        moves_.clear();
        if (!requested || !minimum || !maximum || *minimum > *maximum)
            return;
        const auto offset = std::clamp(*requested, *minimum, *maximum);
        if (offset != *requested)
            snapGuide_.reset();
        for (std::size_t i = 0; i < gestureData_.size(); ++i) {
            const auto time = offsetKeyTime(gestureData_[i].time, offset);
            if (!time) {
                moves_.clear();
                return;
            }
            moves_.push_back({{gestureKeys_[i].curveId, gestureKeys_[i].keyframeId}, *time});
        }
        updateRows();
        event->accept();
        return;
    }
    long double delta = static_cast<long double>(targetIndex) - static_cast<long double>(*lead);
    std::vector<std::uint64_t> frames;
    frames.reserve(gestureData_.size());
    for (const auto& key : gestureData_) {
        const auto frame = nearestFrameIndexForTime(rate, duration, key.time);
        if (!frame)
            return;
        frames.push_back(*frame);
    }
    const auto [first, last] = std::minmax_element(frames.begin(), frames.end());
    delta = std::clamp(delta, -static_cast<long double>(*first),
                       static_cast<long double>(axis->maxIndex - *last));
    moves_.clear();
    const auto framePosition = [rate](core::RationalTime time) {
        return (static_cast<long double>(time.numerator()) /
                static_cast<long double>(time.denominator())) *
               static_cast<long double>(rate.numerator()) /
               static_cast<long double>(rate.denominator());
    };
    for (std::size_t i = 0; i < frames.size(); ++i) {
        long double frame = static_cast<long double>(frames[i]) + delta;
        if (stretchAnchor_) {
            if (gestureData_[i].time == *stretchAnchor_) {
                moves_.push_back(
                    {{gestureKeys_[i].curveId, gestureKeys_[i].keyframeId}, *stretchAnchor_});
                continue;
            }
            const long double fixed = framePosition(*stretchAnchor_);
            const long double oldSpan = framePosition(pressed_->time) - fixed;
            if (oldSpan == 0.0L) {
                moves_.clear();
                return;
            }
            // Preserve the fixed endpoint's exact time, including subframes. Only the moved
            // times snap to frames. A rounded collision is still rejected by MoveKeyframes.
            const long double newSpan =
                oldSpan > 0 ? std::max(1.0L, static_cast<long double>(targetIndex) - fixed)
                            : std::min(-1.0L, static_cast<long double>(targetIndex) - fixed);
            frame = std::round(fixed +
                               (framePosition(gestureData_[i].time) - fixed) * newSpan / oldSpan);
        }
        const auto time =
            frameTimeForIndex(rate, duration,
                              static_cast<std::uint64_t>(std::clamp(
                                  frame, 0.0L, static_cast<long double>(axis->maxIndex))));
        if (!time) {
            moves_.clear();
            return;
        }
        moves_.push_back({{gestureKeys_[i].curveId, gestureKeys_[i].keyframeId}, *time});
    }
    updateRows();
    event->accept();
}
void TimelineKeyframePanel::mouseReleaseEvent(QMouseEvent* event) {
    if (!gridMode_ || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (session_.snapshot().revision() != gestureRevision_) {
        cancelGesture();
        return;
    }
    if (boxing_) {
        auto keys = gestureKeys_;
        const auto axis = ruler_ ? ruler_->axisForWidth(width()) : std::nullopt;
        if (dragging_ && box_ && axis)
            for (const auto& key : laneKeys()) {
                const QPointF point(axis->pixelForTime(key.time), key.row * kTimelineRowHeight -
                                                                      gridScroll_ +
                                                                      kTimelineRowHeight / 2.0);
                if (box_->contains(point) && std::ranges::find(keys, key.selection) == keys.end())
                    keys.push_back(key.selection);
            }
        session_.selectKeyframes(keys);
    } else if (pressed_ && !dragging_ && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        session_.selectKeyframe(pressed_->selection.curveId, pressed_->selection.keyframeId);
    } else if (dragging_ && !moves_.empty()) {
        const auto moves = moves_;
        auto data = gestureData_;
        const auto revision = gestureRevision_;
        const bool copy = copying_;
        cancelGesture();
        if (copy) {
            for (std::size_t i = 0; i < data.size(); ++i)
                data[i].time = moves[i].time;
            (void)session_.pasteKeyframes(data, revision);
        } else
            (void)session_.moveKeyframes(moves, revision);
    }
    cancelGesture();
    event->accept();
}
void TimelineKeyframePanel::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!gridMode_ || event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    const auto hit = hitKey(event->position());
    cancelGesture();
    if (hit) {
        (void)session_.setCurrentTime(hit->time);
        event->accept();
        return;
    }
    const auto axis = ruler_ ? ruler_->axisForWidth(width()) : std::nullopt;
    const auto* composition = session_.composition();
    if (!axis || !composition)
        return;
    const int row = static_cast<int>(event->position().y() + gridScroll_) / kTimelineRowHeight;
    const auto found = std::ranges::find(gridRows_, row);
    if (found == gridRows_.end())
        return;
    const auto parameterId = gridParameters_[static_cast<std::size_t>(found - gridRows_.begin())];
    const auto* parameter = composition->parameters().find(parameterId);
    const auto* source =
        parameter ? std::get_if<document::AnimationCurveSource>(&parameter->source) : nullptr;
    const auto time =
        frameTimeForIndex(axis->frameRate, axis->duration,
                          axis->frameIndexForPixel(static_cast<int>(event->position().x())));
    if (source && time)
        (void)session_.insertKeyframeAtTime(source->curveId, *time);
}
void TimelineKeyframePanel::contextMenuEvent(QContextMenuEvent* event) {
    if (!gridMode_) {
        QWidget::contextMenuEvent(event);
        return;
    }
    const auto hit = hitKey(event->pos());
    if (!hit)
        return;
    if (std::ranges::find(session_.selection().keyframes, hit->selection) ==
        session_.selection().keyframes.end())
        session_.selectKeyframe(hit->selection.curveId, hit->selection.keyframeId);
    cancelGesture();
    std::unique_ptr<QMenu> menuOwner(kit::makeMenu(this));
    auto& menu = *menuOwner;
    for (const auto mode :
         {document::KeyframeInterpolation::Hold, document::KeyframeInterpolation::Linear,
          document::KeyframeInterpolation::EaseInOut}) {
        auto* action =
            menu.addAction(mode == document::KeyframeInterpolation::Hold     ? tr("Hold")
                           : mode == document::KeyframeInterpolation::Linear ? tr("Linear")
                                                                             : tr("Ease In-Out"));
        action->setObjectName("keyframeInterpolationAction");
        connect(action, &QAction::triggered, this,
                [this, mode] { (void)session_.setSelectedKeyframesInterpolation(mode); });
    }
    menu.addSeparator();
    auto* remove = menu.addAction(tr("Delete Keyframes"));
    remove->setObjectName("keyframeDeleteAction");
    connect(remove, &QAction::triggered, this,
            [this] { (void)session_.deleteSelectedKeyframes(); });
    menu.exec(event->globalPos());
    event->accept();
}
void TimelineKeyframePanel::wheelEvent(QWheelEvent* event) {
    if (!ruler_ || !ruler_->handleWheel(event)) {
        event->ignore();
        QWidget::wheelEvent(event);
    }
}
void TimelineKeyframePanel::cancelGesture() {
    stretchAnchor_.reset();
    pressed_.reset();
    dragging_ = false;
    boxing_ = false;
    copying_ = false;
    moves_.clear();
    gestureKeys_.clear();
    gestureData_.clear();
    box_.reset();
    snapGuide_.reset();
    updateRows();
}
void TimelineKeyframePanel::updateRows() {
    for (auto* row : findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
        row->update();
    update();
}
void TimelineKeyframePanel::paintGridOverlay(QPainter& painter, const QWidget& row) const {
    const auto axis = ruler_ ? ruler_->axisForWidth(width()) : std::nullopt;
    if (!axis)
        return;
    painter.save();
    painter.translate(0, -row.y());
    if (box_) {
        painter.setPen(kit::color(kit::Color::Accent));
        auto fill = kit::color(kit::Color::Accent);
        fill.setAlphaF(0.15F);
        painter.setBrush(fill);
        painter.drawRect(*box_);
    }
    painter.setPen(kit::color(kit::Color::Foreground));
    painter.setBrush(Qt::NoBrush);
    for (const auto& move : moves_)
        for (const auto& key : laneKeys()) {
            if (key.selection != KeyframeSelection{move.key.curveId, move.key.keyframeId})
                continue;
            const qreal x = axis->pixelForTime(move.time),
                        y = key.row * kTimelineRowHeight - gridScroll_ + kTimelineRowHeight / 2.0;
            const qreal r = kit::px(kit::Spacing::XS);
            painter.drawPolygon(QPolygonF{QPointF(x, y - r), QPointF(x + r, y), QPointF(x, y + r),
                                          QPointF(x - r, y)});
        }
    if (snapGuide_) {
        const auto x = axis->pixelForTime(*snapGuide_);
        painter.drawLine(QPointF(x, 0), QPointF(x, height()));
    }
    painter.restore();
}
} // namespace bloom::ui
