#include <bloom/ui/timeline_graph_view.hpp>

#include "timeline_graph_math.hpp"
#include "timeline_key_glyph.hpp"
#include "timeline_keyframe_time.hpp"

#include <bloom/ui/timeline_frame_math.hpp>

#include <bloom/document/project.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/curve_compilation.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <memory>
#include <variant>

namespace bloom::ui {
namespace {

// The stroke a curve takes. A scalar parameter keeps the gold Keyframe hue it has everywhere else
// in the timeline; a component takes its axis colour, with R/G/B/A borrowing X/Y/Z/W unchanged so
// one hue always means "the first component" whatever the parameter is.
[[nodiscard]] kit::Color
curveColor(const std::optional<document::AnimationComponent>& component) noexcept {
    if (!component.has_value())
        return kit::Color::Keyframe;
    switch (*component) {
    case document::AnimationComponent::X:
    case document::AnimationComponent::Red:
        return kit::Color::ComponentX;
    case document::AnimationComponent::Y:
    case document::AnimationComponent::Green:
        return kit::Color::ComponentY;
    case document::AnimationComponent::Z:
    case document::AnimationComponent::Blue:
        return kit::Color::ComponentZ;
    case document::AnimationComponent::Alpha:
        return kit::Color::ComponentW;
    }
    return kit::Color::Keyframe;
}

// The component vocabulary of one record kind, in canonical order. Named per kind rather than
// derived from the enum, because X/Y/Z and R/G/B/A are different vocabularies that happen to share
// numbering.
template <typename Curve> [[nodiscard]] std::vector<document::AnimationComponent> componentsOf() {
    if constexpr (std::is_same_v<Curve, document::Vec2AnimationCurve>)
        return {document::AnimationComponent::X, document::AnimationComponent::Y};
    else if constexpr (std::is_same_v<Curve, document::Vec3AnimationCurve>)
        return {document::AnimationComponent::X, document::AnimationComponent::Y,
                document::AnimationComponent::Z};
    else
        return {document::AnimationComponent::Red, document::AnimationComponent::Green,
                document::AnimationComponent::Blue, document::AnimationComponent::Alpha};
}

[[nodiscard]] std::size_t componentIndex(const document::AnimationComponent component) noexcept {
    switch (component) {
    case document::AnimationComponent::X:
    case document::AnimationComponent::Red:
        return 0;
    case document::AnimationComponent::Y:
    case document::AnimationComponent::Green:
        return 1;
    case document::AnimationComponent::Z:
    case document::AnimationComponent::Blue:
        return 2;
    case document::AnimationComponent::Alpha:
        return 3;
    }
    return 0;
}

} // namespace

TimelineGraphView::TimelineGraphView(CompositionSession& session, TimelineRuler& ruler,
                                     QWidget* parent)
    : QWidget(parent), session_(session), ruler_(ruler) {
    setObjectName(QStringLiteral("timelineGraphView"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(true);
    connect(&ruler_, &TimelineRuler::axisChanged, this, qOverload<>(&TimelineGraphView::update));
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            qOverload<>(&TimelineGraphView::update));
    connect(&session_, &CompositionSession::selectionChanged, this,
            qOverload<>(&TimelineGraphView::update));
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        compiledValid_ = false;
        refreshCurves();
        update();
    });
    connect(&session_, &CompositionSession::compositionChanged, this, [this] {
        compiledValid_ = false;
        viewports_.clear();
        refreshCurves();
        update();
    });
}

TimelineGraphView::~TimelineGraphView() = default;

int TimelineGraphView::gutterWidth() const noexcept { return kit::px(kit::Size::GraphValueAxis); }

double TimelineGraphView::bandTop() const noexcept { return kit::px(kit::Spacing::M); }

double TimelineGraphView::bandHeight() const noexcept {
    return std::max(
        1.0, static_cast<double>(height() - kit::px(kit::Spacing::M) - kit::px(kit::Spacing::M)));
}

void TimelineGraphView::setEntries(const std::vector<TimelineLayerEntry>& entries) {
    parameters_.clear();
    for (const auto& entry : entries) {
        const auto lane = std::pair{entry.parameterId, entry.component};
        if ((entry.rowKind == TimelineLayerEntry::Kind::Parameter ||
             entry.rowKind == TimelineLayerEntry::Kind::Component) &&
            std::ranges::find(parameters_, lane) == parameters_.end())
            parameters_.push_back(lane);
    }
    compiledValid_ = false;
    refreshCurves();
    update();
}

void TimelineGraphView::refreshCurves() {
    std::vector<GraphCurveId> curves;
    owners_.clear();
    const auto* composition = session_.composition();
    if (composition != nullptr) {
        for (const auto& [parameterId, selectedComponent] : parameters_) {
            const auto* parameter = composition->parameters().find(parameterId);
            const auto* source =
                parameter != nullptr
                    ? std::get_if<document::AnimationCurveSource>(&parameter->source)
                    : nullptr;
            const auto* record =
                source != nullptr ? composition->animationCurves().find(source->curveId) : nullptr;
            if (record == nullptr)
                continue;
            owners_.emplace(source->curveId, parameterId);
            std::visit(
                [&](const auto& curve) {
                    using Curve = std::decay_t<decltype(curve)>;
                    if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                        const GraphCurveId id{curve.id, std::nullopt};
                        if (!selectedComponent && std::ranges::find(curves, id) == curves.end())
                            curves.push_back(id);
                    } else {
                        // Vector and colour keys are read from components[] ALWAYS. The legacy
                        // whole-value projection cannot answer "which axis is this key on", which
                        // is the only question a curve view asks.
                        for (const auto component : componentsOf<Curve>())
                            if (const auto* lane = curve.component(component);
                                lane != nullptr && !lane->keyframes.empty() &&
                                (!selectedComponent || selectedComponent == component)) {
                                const GraphCurveId id{curve.id, component};
                                if (std::ranges::find(curves, id) == curves.end())
                                    curves.push_back(id);
                            }
                    }
                },
                *record);
        }
    }
    if (curves == curves_)
        return;
    curves_ = std::move(curves);
    for (const auto& curve : curves_)
        ensureViewport(curve);
}

std::vector<document::ScalarKeyframe> TimelineGraphView::keysFor(const GraphCurveId& curve) const {
    const auto* composition = session_.composition();
    const auto* record =
        composition != nullptr ? composition->animationCurves().find(curve.curveId) : nullptr;
    if (record == nullptr)
        return {};
    return std::visit(
        [&](const auto& record2) -> std::vector<document::ScalarKeyframe> {
            using Curve = std::decay_t<decltype(record2)>;
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                return curve.component.has_value() ? std::vector<document::ScalarKeyframe>{}
                                                   : record2.keyframes;
            } else {
                if (!curve.component.has_value())
                    return {};
                const auto* lane = record2.component(*curve.component);
                return lane == nullptr ? std::vector<document::ScalarKeyframe>{} : lane->keyframes;
            }
        },
        *record);
}

const runtime::CompiledScalarCurve*
TimelineGraphView::compiledFor(const GraphCurveId& curve) const {
    const auto revision = session_.snapshot().revision().value();
    if (!compiledValid_ || compiledRevision_ != revision) {
        compiled_.clear();
        compiledRevision_ = revision;
        compiledValid_ = true;
    }
    if (const auto existing = compiled_.find(curve); existing != compiled_.end())
        return &existing->second;
    const auto* composition = session_.composition();
    const auto* record =
        composition != nullptr ? composition->animationCurves().find(curve.curveId) : nullptr;
    if (record == nullptr)
        return nullptr;
    auto built = std::visit(
        [&](const auto& record2) -> std::optional<runtime::CompiledScalarCurve> {
            using Curve = std::decay_t<decltype(record2)>;
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                if (curve.component.has_value())
                    return std::nullopt;
                return runtime::compileAnimationCurve(record2);
            } else {
                if (!curve.component.has_value())
                    return std::nullopt;
                const auto compiled = runtime::compileAnimationCurve(record2);
                const auto index = componentIndex(*curve.component);
                if (index >= compiled.components.size() || compiled.components[index].empty())
                    return std::nullopt;
                return runtime::CompiledScalarCurve{compiled.id, compiled.components[index]};
            }
        },
        *record);
    if (!built.has_value())
        return nullptr;
    return &compiled_.emplace(curve, std::move(*built)).first->second;
}

void TimelineGraphView::ensureViewport(const GraphCurveId& curve) {
    if (viewports_.contains(curve))
        return;
    viewports_.emplace(curve, viewportFor(curve));
}

GraphValueViewport TimelineGraphView::viewportFor(const GraphCurveId& curve) const {
    if (const auto stored = viewports_.find(curve); stored != viewports_.end())
        return stored->second;
    // Auto-fit over the keys AND their control points, so a handle pulled beyond its keys still
    // has somewhere on the canvas to be.
    const auto keys = keysFor(curve);
    std::vector<double> values;
    values.reserve(keys.size() * 3);
    for (std::size_t index = 0; index < keys.size(); ++index) {
        values.push_back(keys[index].value);
        if (index + 1 < keys.size() &&
            keys[index].outgoingInterpolation == document::KeyframeInterpolation::EaseInOut) {
            values.push_back(keys[index].value + keys[index].outgoingHandle.value);
            values.push_back(keys[index + 1].value + keys[index + 1].incomingHandle.value);
        }
    }
    return fitValueViewport(values);
}

void TimelineGraphView::fitCurves() {
    viewports_.clear();
    for (const auto& curve : curves_)
        ensureViewport(curve);
    update();
}

std::vector<QPointF> TimelineGraphView::polylineForTest(const GraphCurveId& curve) const {
    std::vector<QPointF> points;
    const auto axis = ruler_.axisForWidth(width());
    const auto* compiled = compiledFor(curve);
    if (!axis.has_value() || compiled == nullptr)
        return points;
    const auto viewport = viewportFor(curve);
    const double top = bandTop();
    const double band = bandHeight();
    points.reserve(static_cast<std::size_t>(std::max(0, width())));
    for (int x = 0; x < width(); ++x) {
        const auto offset = keyPointerOffset(axis->secondsForPixel(x));
        if (!offset.has_value())
            continue;
        const auto sampled = runtime::sampleAnimationCurve(*compiled, *offset);
        if (!sampled.value.has_value())
            continue;
        points.emplace_back(x, pixelForValue(viewport, *sampled.value, top, band));
    }
    return points;
}

std::optional<GraphCurveId> TimelineGraphView::activeCurve() const {
    if (curves_.empty())
        return std::nullopt;
    const auto& selection = session_.selection();
    if (const auto* key = std::get_if<KeyframeSelection>(&selection.primary)) {
        const GraphCurveId target{key->curveId, key->component};
        if (std::ranges::find(curves_, target) != curves_.end())
            return target;
        for (const auto& curve : curves_)
            if (curve.curveId == key->curveId)
                return curve;
    }
    // The row the selection is on: a parameter row selects its own parameter, and that parameter
    // resolves to exactly the curves this view derived from it.
    if (const auto* composition = session_.composition(); composition != nullptr)
        if (const auto* parameterId = std::get_if<document::ParameterId>(&selection.primary))
            if (const auto* parameter = composition->parameters().find(*parameterId))
                if (const auto* source =
                        std::get_if<document::AnimationCurveSource>(&parameter->source))
                    for (const auto& curve : curves_)
                        if (curve.curveId == source->curveId)
                            return curve;
    return curves_.front();
}

void TimelineGraphView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), kit::color(kit::Color::Surface));
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value())
        return;
    const double top = bandTop();
    const double band = bandHeight();
    const auto active = activeCurve();

    for (const auto& curve : curves_) {
        const auto* compiled = compiledFor(curve);
        if (compiled == nullptr)
            continue;
        const auto viewport = viewportFor(curve);
        const auto tint = kit::color(curveColor(curve.component));
        QPainterPath path;
        bool started = false;
        for (int x = gutterWidth(); x < width(); ++x) {
            const auto offset = keyPointerOffset(axis->secondsForPixel(x));
            if (!offset.has_value())
                continue;
            const auto sampled = runtime::sampleAnimationCurve(*compiled, *offset);
            if (!sampled.value.has_value())
                continue;
            const QPointF point(x, pixelForValue(viewport, *sampled.value, top, band));
            if (!started) {
                path.moveTo(point);
                started = true;
            } else {
                path.lineTo(point);
            }
        }
        painter.setPen(QPen(tint, kit::kHairlineWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);

        const auto keys = keysFor(curve);
        for (std::size_t index = 0; index < keys.size(); ++index) {
            const auto& key = keys[index];
            const KeyframeSelection address{curve.curveId, key.id, curve.component};
            const bool selected = std::ranges::find(session_.selection().keyframes, address) !=
                                  session_.selection().keyframes.end();
            const QPointF center(axis->pixelForTime(key.time),
                                 pixelForValue(viewport, key.value, top, band));
            if (center.x() < gutterWidth())
                continue;
            if (selected) {
                // Handles belong to a SELECTED key on an eased segment, and only there: an
                // unselected key would otherwise fill the canvas with tangents nobody asked for.
                painter.setPen(QPen(kit::color(kit::Color::Muted), kit::kHairlineWidth));
                painter.setBrush(kit::color(kit::Color::Accent));
                const auto dot = kit::px(kit::Size::GraphHandleDot) / 2.0;
                if (index + 1 < keys.size() &&
                    key.outgoingInterpolation == document::KeyframeInterpolation::EaseInOut) {
                    const auto controls =
                        bezierControls({key.time.toSeconds(), key.value},
                                       {keys[index + 1].time.toSeconds(), keys[index + 1].value},
                                       key.outgoingHandle, keys[index + 1].incomingHandle);
                    const QPointF handle(
                        axis->pixelForSeconds(controls.startHandle.time),
                        pixelForValue(viewport, controls.startHandle.value, top, band));
                    painter.drawLine(center, handle);
                    painter.drawEllipse(handle, dot, dot);
                }
                if (index > 0 && keys[index - 1].outgoingInterpolation ==
                                     document::KeyframeInterpolation::EaseInOut) {
                    const auto controls =
                        bezierControls({keys[index - 1].time.toSeconds(), keys[index - 1].value},
                                       {key.time.toSeconds(), key.value},
                                       keys[index - 1].outgoingHandle, key.incomingHandle);
                    const QPointF handle(
                        axis->pixelForSeconds(controls.endHandle.time),
                        pixelForValue(viewport, controls.endHandle.value, top, band));
                    painter.drawLine(center, handle);
                    painter.drawEllipse(handle, dot, dot);
                }
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(selected ? kit::color(kit::Color::Accent) : tint);
            paintKeyGlyph(painter, center, key.outgoingInterpolation);
        }
    }

    // Ghosts: the drag's own pending numbers, drawn where the keys WILL land. They are read from
    // the same pendingMoves_/pendingValues_ the release transaction carries, so the preview and
    // the commit cannot describe different edits.
    if (dragging_ && gesture_ == Gesture::Keys) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(kit::color(kit::Color::Muted));
        for (std::size_t index = 0; index < gestureKeys_.size(); ++index) {
            const auto curve = curveOf(gestureKeys_[index]);
            if (!curve.has_value() || index >= gestureData_.size())
                continue;
            auto time = gestureData_[index].time;
            const auto* base = std::get_if<double>(&gestureData_[index].value);
            double value = base != nullptr ? *base : 0.0;
            for (const auto& move : pendingMoves_)
                if (move.key.keyframeId == gestureKeys_[index].keyframeId &&
                    move.key.curveId == gestureKeys_[index].curveId)
                    time = move.time;
            for (const auto& edit : pendingValues_)
                if (edit.key.keyframeId == gestureKeys_[index].keyframeId &&
                    edit.key.curveId == gestureKeys_[index].curveId)
                    value = edit.value;
            paintKeyGlyph(painter,
                          QPointF(axis->pixelForTime(time),
                                  pixelForValue(viewportFor(*curve), value, top, band)),
                          gestureData_[index].interpolation);
        }
    }
    if (gesture_ == Gesture::Box && box_.has_value()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(kit::color(kit::Color::Accent), kit::kHairlineWidth, Qt::DashLine));
        painter.drawRect(*box_);
    }
    if (snapGuide_.has_value()) {
        // The same dashed guide the lane drag shows, for the same reason: a snap the artist
        // cannot see is a snap they cannot predict.
        painter.setPen(QPen(kit::color(kit::Color::Foreground), kit::kHairlineWidth, Qt::DashLine));
        const auto x = axis->pixelForTime(*snapGuide_);
        painter.drawLine(QPointF(x, 0.0), QPointF(x, height()));
    }

    paintPlayheadLine(painter, *axis, session_.currentTime(), height());

    // The gutter is painted LAST, over the curves, so a curve that runs off the left edge slides
    // under the axis instead of colliding with its labels.
    const QRectF gutter(0, 0, gutterWidth(), height());
    painter.setPen(Qt::NoPen);
    painter.fillRect(gutter, kit::color(kit::Color::Surface));
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.drawLine(QPointF(gutter.right(), 0.0), QPointF(gutter.right(), height()));
    if (active.has_value()) {
        const auto viewport = viewportFor(*active);
        const auto step = niceTickStep(viewport.span(), 4);
        painter.setFont(kit::font(kit::TypeRole::UiSmall));
        painter.setPen(kit::color(kit::Color::Muted));
        for (const double value : valueTicks(viewport, step)) {
            const double y = pixelForValue(viewport, value, top, band);
            const QRectF label(0, y - kit::px(kit::Size::TimelineRow) / 2.0,
                               gutter.width() - kit::px(kit::Spacing::XS),
                               kit::px(kit::Size::TimelineRow));
            painter.drawText(label, Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(value, 'g', 4));
        }
    }
}

std::optional<QPointF> TimelineGraphView::keyCenter(const GraphCurveId& curve,
                                                    const document::KeyframeId keyframeId) const {
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value())
        return std::nullopt;
    const auto keys = keysFor(curve);
    const auto at = std::ranges::find(keys, keyframeId, &document::ScalarKeyframe::id);
    if (at == keys.end())
        return std::nullopt;
    return QPointF(axis->pixelForTime(at->time),
                   pixelForValue(viewportFor(curve), at->value, bandTop(), bandHeight()));
}

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

double TimelineGraphView::valueAt(const GraphCurveId& curve, const core::RationalTime time) const {
    const auto* compiled = compiledFor(curve);
    if (compiled == nullptr)
        return 0.0;
    const auto sampled = runtime::sampleAnimationCurve(*compiled, time);
    return sampled.value.value_or(0.0);
}

std::optional<GraphHit> TimelineGraphView::hitTest(const QPointF position) const {
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value() || position.x() < gutterWidth())
        return std::nullopt;
    const double tolerance = kit::px(kit::Spacing::S);
    const double top = bandTop();
    const double band = bandHeight();
    const auto& selected = session_.selection().keyframes;
    const auto ordered = curvesByPriority();

    // 1. The ease handles of SELECTED keys. They are small and they sit on top of everything else,
    // so testing them first is what makes them grabbable at all.
    for (const auto& curve : ordered) {
        const auto viewport = viewportFor(curve);
        const auto keys = keysFor(curve);
        for (std::size_t index = 0; index < keys.size(); ++index) {
            const KeyframeSelection address{curve.curveId, keys[index].id, curve.component};
            if (std::ranges::find(selected, address) == selected.end())
                continue;
            const auto probe = [&](const GraphPoint& control, const bool outgoing) {
                const QPointF point(axis->pixelForSeconds(control.time),
                                    pixelForValue(viewport, control.value, top, band));
                return QLineF(point, position).length() <= tolerance
                           ? std::optional<GraphHit>{{GraphHit::Kind::Handle, curve, keys[index].id,
                                                      outgoing, keys[index].time}}
                           : std::nullopt;
            };
            if (index + 1 < keys.size() &&
                keys[index].outgoingInterpolation == document::KeyframeInterpolation::EaseInOut) {
                const auto controls =
                    bezierControls({keys[index].time.toSeconds(), keys[index].value},
                                   {keys[index + 1].time.toSeconds(), keys[index + 1].value},
                                   keys[index].outgoingHandle, keys[index + 1].incomingHandle);
                if (const auto hit = probe(controls.startHandle, true))
                    return hit;
            }
            if (index > 0 && keys[index - 1].outgoingInterpolation ==
                                 document::KeyframeInterpolation::EaseInOut) {
                const auto controls =
                    bezierControls({keys[index - 1].time.toSeconds(), keys[index - 1].value},
                                   {keys[index].time.toSeconds(), keys[index].value},
                                   keys[index - 1].outgoingHandle, keys[index].incomingHandle);
                if (const auto hit = probe(controls.endHandle, false))
                    return hit;
            }
        }
    }

    // 2. The keys themselves.
    for (const auto& curve : ordered) {
        const auto viewport = viewportFor(curve);
        for (const auto& key : keysFor(curve)) {
            const QPointF center(axis->pixelForTime(key.time),
                                 pixelForValue(viewport, key.value, top, band));
            if (QLineF(center, position).length() <= tolerance)
                return GraphHit{GraphHit::Kind::Key, curve, key.id, false, key.time};
        }
    }

    // 3. The curve itself, through the SAMPLER rather than a second shape: what is hit is exactly
    // what is drawn.
    const auto pointerTime = keyPointerOffset(axis->secondsForPixel(position.x()));
    if (!pointerTime.has_value())
        return std::nullopt;
    for (const auto& curve : ordered) {
        const auto keys = keysFor(curve);
        if (keys.size() < 2 || *pointerTime < keys.front().time || *pointerTime > keys.back().time)
            continue;
        const double y = pixelForValue(viewportFor(curve), valueAt(curve, *pointerTime), top, band);
        if (std::abs(y - position.y()) <= tolerance)
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
    box_.reset();
    snapGuide_.reset();
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
    snapGuide_.reset();

    const double dx = event.position().x() - press_.x();
    const double dy = event.position().y() - press_.y();
    // Ctrl constrains to the DOMINANT axis, decided once from the whole displacement rather than
    // per move event, so a constrained drag cannot flip axis half way through.
    const bool constrained = event.modifiers().testFlag(Qt::ControlModifier);
    const bool timeOnly = !constrained || std::abs(dx) >= std::abs(dy);
    const bool valueOnly = !constrained || std::abs(dy) > std::abs(dx);

    // TIME. The same helpers, the same snap targets and the same clamp the lane drag uses -- a key
    // must not move differently because the artist is looking at a curve.
    double targetSeconds = pressedTime.toSeconds();
    if (timeOnly) {
        targetSeconds +=
            axis->secondsForPixel(event.position().x()) - axis->secondsForPixel(press_.x());
        const bool snapping = snapping_ && !event.modifiers().testFlag(Qt::ShiftModifier);
        if (snapping) {
            std::vector<core::RationalTime> targets{
                session_.currentTime(), session_.workArea().start, session_.workArea().end};
            for (const auto& curve : curves_)
                for (const auto& key : keysFor(curve))
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
            : (snapping_ && !event.modifiers().testFlag(Qt::ShiftModifier) && timeOnly
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
    const auto offset = std::clamp(*requested, *minimum, *maximum);
    if (offset != *requested)
        snapGuide_.reset();

    for (std::size_t index = 0; index < gestureData_.size(); ++index) {
        const auto time = offsetKeyTime(gestureData_[index].time, offset);
        if (!time.has_value()) {
            pendingMoves_.clear();
            pendingValues_.clear();
            return;
        }
        const auto& key = gestureKeys_[index];
        if (timeOnly && *time != gestureData_[index].time)
            pendingMoves_.push_back({{key.curveId, key.keyframeId, key.component}, *time});
        if (!valueOnly)
            continue;
        const auto curve = curveOf(key);
        if (!curve.has_value())
            continue;
        const auto viewport = viewportFor(*curve);
        const auto* base = std::get_if<double>(&gestureData_[index].value);
        if (base == nullptr)
            continue;
        const double moved = *base - dy * viewport.span() / bandHeight();
        if (moved != *base)
            pendingValues_.push_back({{key.curveId, key.keyframeId, key.component}, moved});
    }
    update();
}

void TimelineGraphView::updateHandleDrag(const QMouseEvent& event) {
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value() || !pressed_.has_value())
        return;
    pendingHandles_.clear();
    const auto& curve = pressed_->curve;
    const auto keys = keysFor(curve);
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

    const auto viewport = viewportFor(curve);
    const double pointerSeconds = axis->secondsForPixel(event.position().x());
    const double pointerValue =
        valueForPixel(viewport, event.position().y(), bandTop(), bandHeight());
    const double deltaTime =
        outgoing ? pointerSeconds - at->time.toSeconds() : at->time.toSeconds() - pointerSeconds;
    const document::KeyframeHandle dragged{std::clamp(deltaTime / segment, 0.0, 1.0),
                                           pointerValue - at->value};

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
        if (outgoing)
            edit.incoming = mirrored;
        else
            edit.outgoing = mirrored;
    }
    pendingHandles_.push_back(edit);
    update();
}

void TimelineGraphView::commitGesture() {
    const auto revision = gestureRevision_;
    if (gesture_ == Gesture::Handle) {
        auto edits = pendingHandles_;
        cancelGesture();
        if (!edits.empty())
            (void)session_.setKeyframeHandles(std::move(edits), revision);
        return;
    }
    auto moves = pendingMoves_;
    auto values = pendingValues_;
    auto data = gestureData_;
    const bool copy = copying_;
    cancelGesture();
    if (moves.empty() && values.empty())
        return;
    if (copy) {
        // A copy-drag leaves the originals alone and pastes the gesture's own data -- values,
        // interpolation and ease handles included -- at the times and values the drag landed on.
        for (std::size_t index = 0; index < data.size(); ++index) {
            if (index < moves.size())
                data[index].time = moves[index].time;
            if (index < values.size())
                data[index].value = values[index].value;
        }
        (void)session_.pasteKeyframes(data, revision);
        return;
    }
    // ONE transaction: a diagonal drag is one gesture and therefore one undo entry. A domain
    // refusal publishes nothing and leaves the selection exactly where it was.
    (void)session_.moveKeyframesAndValues(std::move(moves), std::move(values), revision);
}

void TimelineGraphView::contextMenuEvent(QContextMenuEvent* event) {
    const auto hit = hitTest(event->pos());
    if (hit.has_value() && hit->kind != GraphHit::Kind::Segment) {
        const KeyframeSelection address{hit->curve.curveId, hit->keyframeId, hit->curve.component};
        if (std::ranges::find(session_.selection().keyframes, address) ==
            session_.selection().keyframes.end()) {
            if (address.component.has_value())
                session_.selectKeyframe(address.curveId, *address.component, address.keyframeId);
            else
                session_.selectKeyframe(address.curveId, address.keyframeId);
        }
    }
    cancelGesture();
    const std::unique_ptr<QMenu> owner(kit::makeMenu(this));
    auto& menu = *owner;
    const bool hasKeys = !session_.selection().keyframes.empty();
    const bool finalKey = session_.selectedKeyframeIsFinal();
    for (const auto mode :
         {document::KeyframeInterpolation::Hold, document::KeyframeInterpolation::Linear,
          document::KeyframeInterpolation::EaseInOut}) {
        auto* action =
            menu.addAction(mode == document::KeyframeInterpolation::Hold     ? tr("Hold")
                           : mode == document::KeyframeInterpolation::Linear ? tr("Linear")
                                                                             : tr("Ease In-Out"));
        action->setObjectName("graphInterpolationAction");
        // The final key's mode is canonical Linear, so the two it cannot take are disabled rather
        // than offered and refused.
        action->setEnabled(hasKeys &&
                           (!finalKey || mode == document::KeyframeInterpolation::Linear));
        connect(action, &QAction::triggered, this,
                [this, mode] { (void)session_.setSelectedKeyframesInterpolation(mode); });
    }
    auto* reset = menu.addAction(tr("Reset Handles"));
    reset->setObjectName("graphResetHandlesAction");
    reset->setEnabled(hasKeys);
    connect(reset, &QAction::triggered, this,
            [this] { (void)session_.resetSelectedKeyframeHandles(); });
    menu.addSeparator();
    auto* remove = menu.addAction(tr("Delete Keyframes"));
    remove->setObjectName("graphDeleteAction");
    remove->setEnabled(hasKeys);
    connect(remove, &QAction::triggered, this,
            [this] { (void)session_.deleteSelectedKeyframes(); });
    auto* fit = menu.addAction(tr("Fit Curves"));
    fit->setObjectName("graphFitAction");
    connect(fit, &QAction::triggered, this, [this] { fitCurves(); });
    menu.exec(event->globalPos());
    event->accept();
}

void TimelineGraphView::zoomValues(const double factor, const double anchorPixelY) {
    const double top = bandTop();
    const double band = bandHeight();
    for (const auto& curve : curves_) {
        const auto viewport = viewportFor(curve);
        viewports_[curve] =
            zoomValueViewport(viewport, factor, valueForPixel(viewport, anchorPixelY, top, band));
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
            // The component overloads, always: a component key selected without its component
            // would address the whole-value projection this view never reads.
            if (address.component.has_value())
                session_.selectKeyframe(address.curveId, *address.component, address.keyframeId,
                                        extend);
            else
                session_.selectKeyframe(address.curveId, address.keyframeId, extend);
        }
        gesture_ = Gesture::Keys;
        gestureKeys_ = session_.selection().keyframes;
        gestureData_ = session_.selectedKeyframeData();
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
        const double delta = event->position().y() - panOrigin_->y();
        for (const auto& curve : curves_) {
            const auto viewport = viewportFor(curve);
            viewports_[curve] = panValueViewport(viewport, delta * viewport.span() / bandHeight());
        }
        panOrigin_ = event->position();
        update();
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
        const auto axis = ruler_.axisForWidth(width());
        if (dragging_ && box_.has_value() && axis.has_value())
            for (const auto& curve : curves_) {
                const auto viewport = viewportFor(curve);
                for (const auto& key : keysFor(curve)) {
                    const QPointF point(
                        axis->pixelForTime(key.time),
                        pixelForValue(viewport, key.value, bandTop(), bandHeight()));
                    const KeyframeSelection address{curve.curveId, key.id, curve.component};
                    if (box_->contains(point) && std::ranges::find(keys, address) == keys.end())
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
        snapping_ && !event->modifiers().testFlag(Qt::ShiftModifier)
            ? frameTimeForIndex(
                  axis->frameRate, axis->duration,
                  axis->frameIndexForPixel(static_cast<int>(std::lround(event->position().x()))))
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
        fitCurves();
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
