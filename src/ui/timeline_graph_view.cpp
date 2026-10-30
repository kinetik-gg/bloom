#include <bloom/ui/timeline_graph_view.hpp>

#include "animation_components.hpp"
#include "timeline_graph_math.hpp"
#include "timeline_key_glyph.hpp"
#include "timeline_keyframe_time.hpp"

#include <bloom/ui/timeline_frame_math.hpp>

#include <bloom/document/project.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/curve_compilation.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>

#include <algorithm>
#include <array>
#include <cmath>
#include <variant>

namespace bloom::ui {
namespace {

// The component vocabulary of one record kind, in canonical order. Named per kind rather than
// derived from the enum, because X/Y/Z and R/G/B/A are different vocabularies that happen to share
// numbering.
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

// A component curve takes its axis colour, with R/G/B/A borrowing X/Y/Z/W unchanged so one hue
// always means "the first component" whatever the parameter is.
[[nodiscard]] kit::Color componentColor(const document::AnimationComponent component) noexcept {
    switch (component) {
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
    return kit::Color::ComponentW;
}

constexpr std::array<kit::Color, kit::kGraphCurvePaletteSize> kSingleValuePalette{
    kit::Color::GraphCurve1, kit::Color::GraphCurve2, kit::Color::GraphCurve3,
    kit::Color::GraphCurve4, kit::Color::GraphCurve5};

// The scalar keys of one curve record, addressed the way the view addresses curves: a whole-value
// record by no component, a vector or colour record by exactly one.
[[nodiscard]] std::vector<document::ScalarKeyframe>
keysOfRecord(const document::AnimationCurveRecord& record, const GraphCurveId& curve) {
    return std::visit(
        [&](const auto& held) -> std::vector<document::ScalarKeyframe> {
            using Curve = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Curve, document::PathAnimationCurve>) {
                return {}; // A Path curve has no value graph.
            } else if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                return curve.component.has_value() ? std::vector<document::ScalarKeyframe>{}
                                                   : held.keyframes;
            } else {
                if (!curve.component.has_value())
                    return {};
                const auto* lane = held.component(*curve.component);
                return lane == nullptr ? std::vector<document::ScalarKeyframe>{} : lane->keyframes;
            }
        },
        record);
}

[[nodiscard]] std::optional<runtime::CompiledScalarCurve>
compileRecord(const document::AnimationCurveRecord& record, const GraphCurveId& curve) {
    return std::visit(
        [&](const auto& held) -> std::optional<runtime::CompiledScalarCurve> {
            using Curve = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Curve, document::PathAnimationCurve>) {
                return std::nullopt;
            } else if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                if (curve.component.has_value())
                    return std::nullopt;
                return runtime::compileAnimationCurve(held);
            } else {
                if (!curve.component.has_value())
                    return std::nullopt;
                const auto compiled = runtime::compileAnimationCurve(held);
                const auto index = componentIndex(*curve.component);
                if (index >= compiled.components.size() || compiled.components[index].empty())
                    return std::nullopt;
                return runtime::CompiledScalarCurve{compiled.id, compiled.components[index]};
            }
        },
        record);
}

// Samples per segment when measuring a sampled extent (the Speed Graph, an overshooting easing
// equation) for auto-fit and Normalize.
constexpr int kSpeedSamplesPerSegment = 32;

} // namespace

TimelineGraphView::TimelineGraphView(CompositionSession& session, TimelineRuler& ruler,
                                     QWidget* parent)
    : QWidget(parent), session_(session), ruler_(ruler) {
    setObjectName(QStringLiteral("timelineGraphView"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(true);
    const QSettings settings;
    graphType_ = settings.value(QStringLiteral("timeline/graph-type")).toString() ==
                         QStringLiteral("speed")
                     ? GraphType::Speed
                     : GraphType::Value;
    normalized_ = settings.value(QStringLiteral("timeline/graph-normalize"), false).toBool();
    buildHeader();
    connect(&ruler_, &TimelineRuler::axisChanged, this, &TimelineGraphView::invalidateAll);
    // #358: a playhead move repaints only the strips it leaves and enters.
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &TimelineGraphView::invalidatePlayhead);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &TimelineGraphView::invalidateAll);
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        compiledValid_ = false;
        refreshCurves();
        update();
    });
    connect(&session_, &CompositionSession::compositionChanged, this, [this] {
        compiledValid_ = false;
        viewports_.clear();
        normalizedViewports_.clear();
        refreshCurves();
        update();
    });
    // The live drag's preview lands through the session's live-value channel: repaint only when
    // the preview itself moved, not for every unrelated value edit.
    connect(&session_, &CompositionSession::liveValueChanged, this, [this] {
        if (compiledPreview_ != session_.keyframePreviewGeneration())
            update();
    });
}

TimelineGraphView::~TimelineGraphView() = default;

void TimelineGraphView::buildHeader() {
    // The graph header: the graph type and Normalize, as one kit chrome row over the canvas.
    header_ = new QWidget(this);
    header_->setObjectName(QStringLiteral("timelineGraphHeader"));
    auto* layout = new QHBoxLayout(header_);
    const int padding = kit::px(kit::Spacing::ChromePadding);
    layout->setContentsMargins(kit::px(kit::Spacing::S), padding, padding, padding);
    layout->setSpacing(kit::px(kit::Spacing::ChromeGroupGap));
    typeSwitch_ = new kit::KRadioGroup(header_);
    typeSwitch_->setObjectName(QStringLiteral("timelineGraphTypeSwitch"));
    typeSwitch_->setAccessibleName(tr("Graph type"));
    typeSwitch_->setPresentation(kit::KRadioGroup::Presentation::Segmented);
    typeSwitch_->addOption(tr("Value Graph"));
    typeSwitch_->addOption(tr("Speed Graph"));
    typeSwitch_->setCurrentIndex(graphType_ == GraphType::Speed ? 1 : 0);
    connect(typeSwitch_, &kit::KRadioGroup::currentIndexChanged, this, [this](const int index) {
        setGraphType(index == 1 ? GraphType::Speed : GraphType::Value);
    });
    normalizeToggle_ = new kit::KCheckBox(tr("Normalize"), header_);
    normalizeToggle_->setObjectName(QStringLiteral("timelineGraphNormalizeToggle"));
    normalizeToggle_->setToolTip(
        tr("Draw every curve scaled to -1...1; edits still write real values"));
    normalizeToggle_->setChecked(normalized_);
    connect(normalizeToggle_, &kit::KCheckBox::toggled, this,
            [this](const bool checked) { setNormalized(checked); });
    layout->addWidget(typeSwitch_);
    layout->addWidget(normalizeToggle_);
    layout->addStretch(1);
    header_->setFixedHeight(kit::px(kit::Size::HeaderRow));
    header_->setGeometry(0, 0, width(), kit::px(kit::Size::HeaderRow));
}

void TimelineGraphView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (header_ != nullptr)
        header_->setGeometry(0, 0, width(), kit::px(kit::Size::HeaderRow));
}

void TimelineGraphView::setGraphType(const GraphType type) {
    if (graphType_ == type)
        return;
    cancelGesture();
    graphType_ = type;
    QSettings().setValue(QStringLiteral("timeline/graph-type"),
                         type == GraphType::Speed ? QStringLiteral("speed")
                                                  : QStringLiteral("value"));
    if (typeSwitch_ != nullptr && typeSwitch_->currentIndex() != (type == GraphType::Speed ? 1 : 0))
        typeSwitch_->setCurrentIndex(type == GraphType::Speed ? 1 : 0);
    for (const auto& curve : curves_)
        if (!viewports_.contains({curve, graphType_}))
            viewports_.emplace(std::pair{curve, graphType_}, fittedViewport(curve));
    invalidateAll();
}

void TimelineGraphView::setNormalized(const bool normalized) {
    if (normalized_ == normalized)
        return;
    cancelGesture();
    normalized_ = normalized;
    QSettings().setValue(QStringLiteral("timeline/graph-normalize"), normalized);
    if (normalizeToggle_ != nullptr && normalizeToggle_->isChecked() != normalized)
        normalizeToggle_->setChecked(normalized);
    invalidateAll();
}

void TimelineGraphView::invalidateAll() {
    ++work_.fullInvalidations;
    update();
}

void TimelineGraphView::invalidatePlayhead() {
    const auto axis = ruler_.axisForWidth(width());
    if (!axis || !paintedPlayheadX_ || dragging_) {
        invalidateAll();
        return;
    }
    ++work_.playheadInvalidations;
    const int margin = kit::px(kit::Spacing::XXS) + 1;
    const int next = static_cast<int>(std::floor(axis->pixelForTime(session_.currentTime())));
    const int band = margin + kit::px(kit::Size::Hairline) + margin;
    const int top = kit::px(kit::Size::HeaderRow);
    for (const int x : {*paintedPlayheadX_, next})
        update(QRect(x - margin, top, band, height() - top));
}

const QPainterPath& TimelineGraphView::pathFor(const GraphCurveId& curve,
                                               const runtime::CompiledScalarCurve& compiled) const {
    const auto axis = ruler_.axisForWidth(width());
    const auto lens = lensFor(curve);
    auto& cached = paths_[curve];
    if (axis && cached.generation == compiledGeneration_ && cached.t0 == axis->t0 &&
        cached.t1 == axis->t1 && cached.width == width() && cached.top == lens.top &&
        cached.band == lens.height && cached.minimum == lens.viewport.minimum &&
        cached.maximum == lens.viewport.maximum && cached.center == lens.normalization.center &&
        cached.halfSpan == lens.normalization.halfSpan && cached.type == graphType_ &&
        !cached.path.isEmpty())
        return cached.path;
    QPainterPath path;
    if (axis) {
        const auto keys = keysFor(curve);
        std::vector<double> times;
        std::vector<document::KeyframeInterpolation> modes;
        for (const auto& key : keys) {
            times.push_back(key.time.toSeconds());
            modes.push_back(key.outgoingInterpolation);
        }
        const auto sample = [&](const double seconds) {
            const auto offset = keyPointerOffset(seconds);
            const auto sampled = offset ? runtime::sampleAnimationCurve(compiled, *offset)
                                        : runtime::AnimationSampleResult<double>{};
            return sampled.value.value_or(0.0);
        };
        bool started = false;
        for (int x = gutterWidth(); x < width(); ++x) {
            const double seconds = axis->secondsForPixel(x);
            const auto offset = keyPointerOffset(seconds);
            if (!offset.has_value())
                continue;
            ++work_.curveSamples;
            double quantity = 0.0;
            if (graphType_ == GraphType::Speed) {
                quantity = curveSpeed(sample, times, modes, seconds);
            } else {
                const auto sampled = runtime::sampleAnimationCurve(compiled, *offset);
                if (!sampled.value.has_value())
                    continue;
                quantity = *sampled.value;
            }
            const QPointF point(x, lens.pixelFor(quantity));
            if (!started) {
                path.moveTo(point);
                started = true;
            } else {
                path.lineTo(point);
            }
        }
        cached = {compiledGeneration_,     axis->t0,  axis->t1,   width(),
                  lens.top,                lens.height, lens.viewport.minimum,
                  lens.viewport.maximum,   lens.normalization.center,
                  lens.normalization.halfSpan, graphType_, path};
    }
    return cached.path;
}

int TimelineGraphView::gutterWidth() const noexcept { return kit::px(kit::Size::GraphValueAxis); }

double TimelineGraphView::bandTop() const noexcept {
    return kit::px(kit::Size::HeaderRow) + kit::px(kit::Spacing::M);
}

double TimelineGraphView::bandHeight() const noexcept {
    return std::max(1.0, static_cast<double>(height()) - bandTop() - kit::px(kit::Spacing::M));
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
                    if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                        const GraphCurveId id{curve.id, std::nullopt};
                        if (!selectedComponent && std::ranges::find(curves, id) == curves.end())
                            curves.push_back(id);
                    } else {
                        // Vector and colour keys are read from components[]: "which axis is
                        // this key on" is the only question a curve view asks, and a component
                        // curve is the one thing that answers it.
                        for (const auto component : animationComponentsOf<Curve>())
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
        if (!viewports_.contains({curve, graphType_}))
            viewports_.emplace(std::pair{curve, graphType_}, fittedViewport(curve));
}

std::vector<document::ScalarKeyframe> TimelineGraphView::keysFor(const GraphCurveId& curve,
                                                                 const bool preview) const {
    if (preview)
        if (const auto* previewed = session_.keyframePreviewCurve(curve.curveId))
            return keysOfRecord(*previewed, curve);
    const auto* composition = session_.composition();
    const auto* record =
        composition != nullptr ? composition->animationCurves().find(curve.curveId) : nullptr;
    return record == nullptr ? std::vector<document::ScalarKeyframe>{}
                             : keysOfRecord(*record, curve);
}

const runtime::CompiledScalarCurve*
TimelineGraphView::compiledFor(const GraphCurveId& curve) const {
    const auto revision = session_.snapshot().revision().value();
    const auto preview = session_.keyframePreviewGeneration();
    if (!compiledValid_ || compiledRevision_ != revision || compiledPreview_ != preview) {
        compiled_.clear();
        ++compiledGeneration_;
        compiledRevision_ = revision;
        compiledPreview_ = preview;
        compiledValid_ = true;
    }
    if (const auto existing = compiled_.find(curve); existing != compiled_.end())
        return &existing->second;
    const auto* record = session_.keyframePreviewCurve(curve.curveId);
    if (record == nullptr) {
        const auto* composition = session_.composition();
        record =
            composition != nullptr ? composition->animationCurves().find(curve.curveId) : nullptr;
    }
    if (record == nullptr)
        return nullptr;
    auto built = compileRecord(*record, curve);
    if (!built.has_value())
        return nullptr;
    return &compiled_.emplace(curve, std::move(*built)).first->second;
}

double TimelineGraphView::plottedAt(const GraphCurveId& curve, const double seconds,
                                    const bool outgoing) const {
    const auto* compiled = compiledFor(curve);
    if (compiled == nullptr)
        return 0.0;
    const auto sample = [compiled](const double at) {
        const auto offset = keyPointerOffset(at);
        const auto sampled = offset ? runtime::sampleAnimationCurve(*compiled, *offset)
                                    : runtime::AnimationSampleResult<double>{};
        return sampled.value.value_or(0.0);
    };
    if (graphType_ == GraphType::Value)
        return sample(seconds);
    std::vector<double> times;
    std::vector<document::KeyframeInterpolation> modes;
    for (const auto& key : keysFor(curve)) {
        times.push_back(key.time.toSeconds());
        modes.push_back(key.outgoingInterpolation);
    }
    return curveSpeed(sample, times, modes, seconds, outgoing);
}

std::vector<double> TimelineGraphView::plottedExtent(const GraphCurveId& curve) const {
    const auto keys = keysFor(curve);
    std::vector<double> values;
    if (graphType_ == GraphType::Value) {
        // The keys AND their control points, so a handle pulled beyond its keys still has
        // somewhere on the canvas to be.
        values.reserve(keys.size() * 3);
        for (std::size_t index = 0; index < keys.size(); ++index) {
            values.push_back(keys[index].value);
            if (index + 1 >= keys.size())
                continue;
            if (keys[index].outgoingInterpolation == document::KeyframeInterpolation::EaseInOut) {
                values.push_back(keys[index].value + keys[index].outgoingHandle.value);
                values.push_back(keys[index + 1].value + keys[index + 1].incomingHandle.value);
            } else if (document::isEasingInterpolation(keys[index].outgoingInterpolation)) {
                // Back and Elastic overshoot their keys: the sampled curve is the extent.
                const double start = keys[index].time.toSeconds();
                const double end = keys[index + 1].time.toSeconds();
                for (int step = 1; step < kSpeedSamplesPerSegment; ++step)
                    values.push_back(plottedAt(curve, start + (end - start) * step /
                                                              kSpeedSamplesPerSegment));
            }
        }
        return values;
    }
    // The Speed Graph's extent is the speed curve's own: both sides of every key and a run of
    // samples across every segment.
    for (std::size_t index = 0; index + 1 < keys.size(); ++index) {
        const double start = keys[index].time.toSeconds();
        const double end = keys[index + 1].time.toSeconds();
        values.push_back(plottedAt(curve, start, true));
        values.push_back(plottedAt(curve, end, false));
        for (int step = 1; step < kSpeedSamplesPerSegment; ++step)
            values.push_back(plottedAt(curve, start + (end - start) * step / kSpeedSamplesPerSegment));
    }
    // Before the first key and after the last the curve holds, so zero speed is always on the
    // canvas too and a curve never drops out of view where it stops moving.
    values.push_back(0.0);
    return values;
}

GraphNormalization TimelineGraphView::normalizationOf(const GraphCurveId& curve) const {
    if (const auto frozen = frozenNormalization_.find(curve); frozen != frozenNormalization_.end())
        return frozen->second;
    return normalizationFor(plottedExtent(curve));
}

GraphValueViewport TimelineGraphView::fittedViewport(const GraphCurveId& curve) const {
    return fitValueViewport(plottedExtent(curve));
}

GraphValueViewport TimelineGraphView::viewportFor(const GraphCurveId& curve) const {
    if (normalized_) {
        const auto stored = normalizedViewports_.find(graphType_);
        return stored != normalizedViewports_.end() ? stored->second : normalizedViewport();
    }
    if (const auto stored = viewports_.find({curve, graphType_}); stored != viewports_.end())
        return stored->second;
    return fittedViewport(curve);
}

void TimelineGraphView::setViewport(const GraphCurveId& curve, const GraphValueViewport& viewport) {
    if (normalized_)
        normalizedViewports_[graphType_] = viewport;
    else
        viewports_[{curve, graphType_}] = viewport;
}

GraphLens TimelineGraphView::lensFor(const GraphCurveId& curve) const {
    return {viewportFor(curve), normalized_ ? normalizationOf(curve) : GraphNormalization{},
            bandTop(), bandHeight()};
}

double TimelineGraphView::minimumFitSeconds() const {
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value())
        return 0.0;
    return kGraphFitMinimumFrames * static_cast<double>(axis->frameRate.denominator()) /
           static_cast<double>(axis->frameRate.numerator());
}

void TimelineGraphView::frameTimes(const double t0, const double t1) {
    ruler_.zoomToRange(t0, t1);
}

void TimelineGraphView::fitCurves() {
    std::vector<GraphPoint> points;
    for (const auto& curve : curves_) {
        viewports_[{curve, graphType_}] = fittedViewport(curve);
        for (const auto& key : keysFor(curve))
            points.push_back({key.time.toSeconds(), 0.0});
    }
    normalizedViewports_.erase(graphType_);
    if (const auto frame = frameFor(points, minimumFitSeconds()))
        frameTimes(frame->t0, frame->t1);
    update();
}

void TimelineGraphView::fitSelection() {
    const auto& selected = session_.selection().keyframes;
    std::map<GraphCurveId, std::vector<GraphPoint>> byCurve;
    std::vector<GraphPoint> all;
    for (const auto& curve : curves_) {
        const auto keys = keysFor(curve);
        for (std::size_t index = 0; index < keys.size(); ++index) {
            if (std::ranges::find(selected, KeyframeSelection{curve.curveId, keys[index].id,
                                                              curve.component}) == selected.end())
                continue;
            const double seconds = keys[index].time.toSeconds();
            const double plotted = graphType_ == GraphType::Value
                                       ? keys[index].value
                                       : plottedAt(curve, seconds, index + 1 < keys.size());
            byCurve[curve].push_back({seconds, plotted});
            all.push_back({seconds, plotted});
        }
    }
    if (all.empty()) {
        fitCurves();
        return;
    }
    const double minimumSeconds = minimumFitSeconds();
    for (const auto& [curve, points] : byCurve) {
        if (const auto frame = frameFor(points, minimumSeconds)) {
            if (normalized_) {
                const auto normalization = normalizationOf(curve);
                std::vector<double> display;
                for (const auto& point : points)
                    display.push_back(normalization.toDisplay(point.value));
                normalizedViewports_[graphType_] = fitValueViewport(display);
            } else {
                viewports_[{curve, graphType_}] = frame->values;
            }
        }
    }
    if (const auto frame = frameFor(all, minimumSeconds))
        frameTimes(frame->t0, frame->t1);
    update();
}

std::vector<QPointF> TimelineGraphView::polylineForTest(const GraphCurveId& curve) const {
    std::vector<QPointF> points;
    const auto axis = ruler_.axisForWidth(width());
    const auto* compiled = compiledFor(curve);
    if (!axis.has_value() || compiled == nullptr)
        return points;
    const auto lens = lensFor(curve);
    points.reserve(static_cast<std::size_t>(std::max(0, width())));
    for (int x = 0; x < width(); ++x) {
        const double seconds = axis->secondsForPixel(x);
        if (!keyPointerOffset(seconds).has_value())
            continue;
        points.emplace_back(x, lens.pixelFor(plottedAt(curve, seconds)));
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

std::vector<GraphCurveId> TimelineGraphView::selectedCurves() const {
    std::vector<GraphCurveId> selected;
    for (const auto& key : session_.selection().keyframes) {
        const GraphCurveId curve{key.curveId, key.component};
        if (std::ranges::find(curves_, curve) != curves_.end() &&
            std::ranges::find(selected, curve) == selected.end())
            selected.push_back(curve);
    }
    if (!selected.empty())
        return selected;
    if (const auto* composition = session_.composition(); composition != nullptr)
        if (const auto* parameterId =
                std::get_if<document::ParameterId>(&session_.selection().primary))
            if (const auto* parameter = composition->parameters().find(*parameterId))
                if (const auto* source =
                        std::get_if<document::AnimationCurveSource>(&parameter->source))
                    for (const auto& curve : curves_)
                        if (curve.curveId == source->curveId)
                            selected.push_back(curve);
    return selected;
}

QColor TimelineGraphView::curveColor(const GraphCurveId& curve, const bool selected) const {
    kit::Color token = kit::Color::GraphCurve1;
    if (curve.component.has_value()) {
        token = componentColor(*curve.component);
    } else {
        // Single-value curves take the palette in curve order, so two on one canvas never share
        // a hue until the palette is exhausted.
        std::size_t index = 0;
        for (const auto& other : curves_) {
            if (other == curve)
                break;
            if (!other.component.has_value())
                ++index;
        }
        token = kSingleValuePalette[index % kSingleValuePalette.size()];
    }
    const auto resting = kit::color(token);
    return selected ? kit::emphasizedStrokeFor(resting) : resting;
}

QPointF TimelineGraphView::keyPoint(const GraphCurveId& curve,
                                    const std::vector<document::ScalarKeyframe>& keys,
                                    const std::size_t index) const {
    const auto axis = ruler_.axisForWidth(width());
    const auto& key = keys[index];
    const double x = axis.has_value() ? axis->pixelForTime(key.time) : 0.0;
    const auto lens = lensFor(curve);
    if (graphType_ == GraphType::Value)
        return {x, lens.pixelFor(key.value)};
    // A Speed Graph key sits at the speed LEAVING it; the final key at the speed arriving.
    return {x, lens.pixelFor(plottedAt(curve, key.time.toSeconds(), index + 1 < keys.size()))};
}

std::optional<QPointF>
TimelineGraphView::handlePoint(const GraphCurveId& curve,
                               const std::vector<document::ScalarKeyframe>& keys,
                               const std::size_t index, const bool outgoing) const {
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value())
        return std::nullopt;
    if (outgoing ? (index + 1 >= keys.size() ||
                    keys[index].outgoingInterpolation != document::KeyframeInterpolation::EaseInOut)
                 : (index == 0 || keys[index - 1].outgoingInterpolation !=
                                      document::KeyframeInterpolation::EaseInOut))
        return std::nullopt;
    const auto& key = keys[index];
    const auto& neighbour = outgoing ? keys[index + 1] : keys[index - 1];
    const auto lens = lensFor(curve);
    if (graphType_ == GraphType::Value) {
        const auto& start = outgoing ? key : neighbour;
        const auto& end = outgoing ? neighbour : key;
        const auto controls =
            bezierControls({start.time.toSeconds(), start.value}, {end.time.toSeconds(), end.value},
                           start.outgoingHandle, end.incomingHandle);
        const auto& control = outgoing ? controls.startHandle : controls.endHandle;
        return QPointF(axis->pixelForSeconds(control.time), lens.pixelFor(control.value));
    }
    // The Speed Graph's handle: horizontal at the key's speed on that side, as long as its
    // influence -- After Effects' influence/speed handle, exact in the same Bezier.
    const double duration = std::abs(neighbour.time.toSeconds() - key.time.toSeconds());
    const auto handle = outgoing ? key.outgoingHandle : key.incomingHandle;
    const double speed = handleSpeed(handle, duration, outgoing).value_or(0.0);
    const double seconds =
        key.time.toSeconds() + (outgoing ? 1.0 : -1.0) * handle.time * duration;
    return QPointF(axis->pixelForSeconds(seconds), lens.pixelFor(speed));
}

void TimelineGraphView::paintGrid(QPainter& painter,
                                  const std::optional<GraphCurveId>& active) const {
    // After Effects' grid: horizontal value lines at the gutter's own ticks, vertical frame lines
    // at the ruler's own labelled ticks, so both line up with the numbers beside them.
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    const double top = kit::px(kit::Size::HeaderRow);
    if (active.has_value()) {
        const auto lens = lensFor(*active);
        const auto step = niceTickStep(
            lens.viewport.span(), std::max(2, static_cast<int>(lens.height / kit::px(
                                                                    kit::Size::HeaderRow))));
        for (const double value : valueTicks(lens.viewport, step)) {
            const double y = std::round(pixelForValue(lens.viewport, value, lens.top, lens.height));
            painter.drawLine(QPointF(gutterWidth(), y + 0.5), QPointF(width(), y + 0.5));
        }
    }
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value())
        return;
    for (const auto time : ruler_.majorTickTimes(width())) {
        const double x = std::round(axis->pixelForTime(time));
        if (x < gutterWidth())
            continue;
        painter.drawLine(QPointF(x + 0.5, top), QPointF(x + 0.5, height()));
    }
}

void TimelineGraphView::paintCurve(QPainter& painter, const GraphCurveId& curve,
                                   const bool selected) const {
    const auto* compiled = compiledFor(curve);
    if (compiled == nullptr)
        return;
    const auto tint = curveColor(curve, selected);
    painter.setPen(QPen(tint, kit::kHairlineWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(pathFor(curve, *compiled));

    const auto& selection = session_.selection().keyframes;
    const auto keys = keysFor(curve);
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const auto& key = keys[index];
        const KeyframeSelection address{curve.curveId, key.id, curve.component};
        const bool keySelected = std::ranges::find(selection, address) != selection.end();
        const QPointF center = keyPoint(curve, keys, index);
        if (center.x() < gutterWidth())
            continue;
        if (keySelected) {
            // Handles belong to a SELECTED key on an eased segment, and only there: an
            // unselected key would otherwise fill the canvas with tangents nobody asked for.
            // The arm takes the curve's own ink so it reads as part of that curve; the dot is
            // the selection colour with an edge, so it stays findable over any line.
            const auto dot = kit::px(kit::Size::GraphHandleDot) / 2.0;
            for (const bool outgoing : {false, true}) {
                const auto handle = handlePoint(curve, keys, index, outgoing);
                if (!handle.has_value())
                    continue;
                painter.setPen(QPen(tint, kit::kHairlineWidth));
                painter.setBrush(Qt::NoBrush);
                if (graphType_ == GraphType::Speed)
                    painter.drawLine(QPointF(center.x(), handle->y()), *handle);
                else
                    painter.drawLine(center, *handle);
                painter.setPen(QPen(kit::edgeInkFor(kit::color(kit::Color::Accent)),
                                    kit::kHairlineWidth));
                painter.setBrush(kit::color(kit::Color::Accent));
                painter.drawEllipse(*handle, dot, dot);
            }
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(keySelected ? kit::color(kit::Color::Accent) : tint);
        paintKeyGlyph(painter, center, key.outgoingInterpolation);
    }
}

void TimelineGraphView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), kit::color(kit::Color::Surface));
    const double headerBottom = kit::px(kit::Size::HeaderRow);
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.drawLine(QPointF(0.0, headerBottom - 0.5), QPointF(width(), headerBottom - 0.5));
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value())
        return;
    painter.save();
    painter.setClipRect(QRectF(0.0, headerBottom, width(), height() - headerBottom));
    const auto active = activeCurve();
    paintGrid(painter, active);
    // Unselected curves first, so the brighter selected ones are never painted under them.
    const auto selected = selectedCurves();
    for (const bool emphasised : {false, true})
        for (const auto& curve : curves_)
            if ((std::ranges::find(selected, curve) != selected.end()) == emphasised)
                paintCurve(painter, curve, emphasised);

    if (gesture_ == Gesture::Box && box_.has_value()) {
        painter.setPen(QPen(kit::color(kit::Color::Accent), kit::kHairlineWidth, Qt::DashLine));
        painter.setBrush(
            kit::withOpacity(kit::color(kit::Color::Accent), kit::kMarqueeFillOpacity));
        painter.drawRect(*box_);
    }
    if (snapGuide_.has_value()) {
        // The same dashed guide the lane drag shows, for the same reason: a snap the artist
        // cannot see is a snap they cannot predict.
        painter.setPen(QPen(kit::color(kit::Color::Foreground), kit::kHairlineWidth, Qt::DashLine));
        const auto x = axis->pixelForTime(*snapGuide_);
        painter.drawLine(QPointF(x, headerBottom), QPointF(x, height()));
    }

    paintPlayheadLine(painter, *axis, session_.currentTime(), height());
    paintedPlayheadX_ = static_cast<int>(std::floor(axis->pixelForTime(session_.currentTime())));

    // The gutter is painted LAST, over the curves, so a curve that runs off the left edge slides
    // under the axis instead of colliding with its labels. The time axis keeps the gutter's width
    // clear before frame 0 (TimelineRuler::setLeadingInset), so no key is ever under it.
    const QRectF gutter(0, headerBottom, gutterWidth(), height() - headerBottom);
    painter.setPen(Qt::NoPen);
    painter.fillRect(gutter, kit::color(kit::Color::Surface));
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.drawLine(QPointF(gutter.right() - 0.5, headerBottom),
                     QPointF(gutter.right() - 0.5, height()));
    if (active.has_value()) {
        const auto lens = lensFor(*active);
        const auto step = niceTickStep(
            lens.viewport.span(), std::max(2, static_cast<int>(lens.height / kit::px(
                                                                    kit::Size::HeaderRow))));
        painter.setFont(kit::font(kit::TypeRole::UiSmall));
        painter.setPen(kit::color(kit::Color::Muted));
        for (const double value : valueTicks(lens.viewport, step)) {
            const double y = pixelForValue(lens.viewport, value, lens.top, lens.height);
            const QRectF label(0, y - kit::px(kit::Size::ListRow) / 2.0,
                               gutter.width() - kit::px(kit::Spacing::XS),
                               kit::px(kit::Size::ListRow));
            painter.drawText(label, Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(std::abs(value) < step * 1e-9 ? 0.0 : value, 'g', 4));
        }
    }
    painter.restore();
}

std::optional<QPointF> TimelineGraphView::keyCenter(const GraphCurveId& curve,
                                                    const document::KeyframeId keyframeId) const {
    if (!ruler_.axisForWidth(width()).has_value())
        return std::nullopt;
    const auto keys = keysFor(curve);
    const auto at = std::ranges::find(keys, keyframeId, &document::ScalarKeyframe::id);
    if (at == keys.end())
        return std::nullopt;
    return keyPoint(curve, keys, static_cast<std::size_t>(at - keys.begin()));
}

std::optional<QPointF> TimelineGraphView::handleCenter(const GraphCurveId& curve,
                                                       const document::KeyframeId keyframeId,
                                                       const bool outgoing) const {
    const auto keys = keysFor(curve);
    const auto at = std::ranges::find(keys, keyframeId, &document::ScalarKeyframe::id);
    if (at == keys.end())
        return std::nullopt;
    return handlePoint(curve, keys, static_cast<std::size_t>(at - keys.begin()), outgoing);
}

} // namespace bloom::ui
