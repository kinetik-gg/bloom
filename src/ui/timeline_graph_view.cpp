#include <bloom/ui/timeline_graph_view.hpp>

#include "timeline_graph_math.hpp"
#include "timeline_key_glyph.hpp"
#include "timeline_keyframe_time.hpp"

#include <bloom/document/project.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/curve_compilation.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
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
    for (const auto& entry : entries)
        if (entry.rowKind == TimelineLayerEntry::Kind::Parameter)
            parameters_.push_back(entry.parameterId);
    compiledValid_ = false;
    refreshCurves();
    update();
}

void TimelineGraphView::refreshCurves() {
    std::vector<GraphCurveId> curves;
    const auto* composition = session_.composition();
    if (composition != nullptr) {
        for (const auto parameterId : parameters_) {
            const auto* parameter = composition->parameters().find(parameterId);
            const auto* source =
                parameter != nullptr
                    ? std::get_if<document::AnimationCurveSource>(&parameter->source)
                    : nullptr;
            const auto* record =
                source != nullptr ? composition->animationCurves().find(source->curveId) : nullptr;
            if (record == nullptr)
                continue;
            std::visit(
                [&](const auto& curve) {
                    using Curve = std::decay_t<decltype(curve)>;
                    if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                        curves.push_back({curve.id, std::nullopt});
                    } else {
                        // Vector and colour keys are read from components[] ALWAYS. The legacy
                        // whole-value projection cannot answer "which axis is this key on", which
                        // is the only question a curve view asks.
                        for (const auto component : componentsOf<Curve>())
                            if (const auto* lane = curve.component(component);
                                lane != nullptr && !lane->keyframes.empty())
                                curves.push_back({curve.id, component});
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
    QWidget::mousePressEvent(event);
}

void TimelineGraphView::mouseMoveEvent(QMouseEvent* event) {
    if (panOrigin_.has_value()) {
        const double delta = event->position().y() - panOrigin_->y();
        const double band = bandHeight();
        for (const auto& curve : curves_) {
            const auto viewport = viewportFor(curve);
            viewports_[curve] = panValueViewport(viewport, delta * viewport.span() / band);
        }
        panOrigin_ = event->position();
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TimelineGraphView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton && panOrigin_.has_value()) {
        panOrigin_.reset();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TimelineGraphView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->position().x() < gutterWidth()) {
        fitCurves();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TimelineGraphView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_F) {
        fitCurves();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace bloom::ui
