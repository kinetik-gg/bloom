#include <bloom/ui/timeline_ruler.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_frame_math.hpp>

#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <bloom/document/animation.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <QApplication>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::ui {
namespace {

// Ruler/lane extents (task U7, issue #122, decisions 1/3): TimelineRow (34px) is the token every
// track row -- header AND lane -- now shares; the ruler keeps its own extent (no dedicated
// "ruler height" token exists) but is still resolved from Control (26px) rather than a bare
// literal, matching this file's own prior 26px value exactly.
const int kRulerHeight = kit::px(kit::Size::Control);
const int kKeyframeRowHeight = kit::px(kit::Size::TimelineRow);
// The honest work-area strip (decision 3): thin, using the smallest spacing token rather than an
// invented pixel gap.
const int kWorkAreaStripHeight = kit::px(kit::Spacing::XS);
constexpr qreal kKeyDiamondRadius = 4.5;
constexpr qreal kKeyHitToleranceLogicalPixels = 6.0;
// Minor ticks are a dense, purely visual grid (decision 3: "minors as subtle ticks"); majors are
// re-derived per paint from the axis's OWN font metrics so adjacent labels can never collide (see
// majorTickStepFrames() below) rather than reusing this fixed minor-tick pixel budget for labels
// the way the pre-restyle single-density ruler did.
constexpr qreal kMinimumPixelsPerMinorTick = 6.0;
// Extra breathing room between two adjacent major labels, beyond their own widest possible text
// width -- keeps the collision-avoidance math from packing labels edge-to-edge.
constexpr qreal kMajorLabelGapPixels = 10.0;
constexpr qreal kTickLabelInsetPixels = 3.0;
constexpr qreal kMinorTickHeight = 4.0;
constexpr qreal kMajorTickHeight = 8.0;
// "Accent 2px line" (decision 4), shared by the ruler's own playhead and every keyframe lane row's
// playhead segment so the line reads as one continuous stroke down through the whole panel.
constexpr qreal kPlayheadLineWidth = 2.0;

// Maps this widget's pixel-space x axis onto composition frame indices/time using
// bloom::ui::maxFrameIndex()/frameTimeForIndex() (src/ui/include/bloom/ui/timeline_frame_math.hpp),
// which delegate to bloom::core::FrameTimeMapping's checked, exact-rational contract math. Pixel
// coordinates are UI-space integers, not RationalTime values, so the reverse pixel -> frame-index
// direction below (used only for scrubbing) is a separate, deliberately exact integer mapping using
// the same tie-to-greater rule as the time-domain contract, kept local to this file since it is not
// part of that contract. The forward frame/time -> pixel direction (used only for painting) is
// ordinary presentational arithmetic, not a clamp/tie/mapping decision.
struct TimelineAxis final {
    document::FrameRate frameRate;
    core::RationalTime duration;
    int widthPixels = 0;
    std::uint64_t maxIndex = 0;

    [[nodiscard]] static std::optional<TimelineAxis>
    create(const document::Composition& composition, const int widthPixels) {
        const auto frameRate = composition.format().frameRate();
        const auto duration = composition.duration();
        const auto maxIndex = ui::maxFrameIndex(frameRate, duration);
        if (!maxIndex.has_value()) {
            return std::nullopt;
        }
        return TimelineAxis{frameRate, duration, widthPixels, *maxIndex};
    }

    // Exact pixel -> frame index, clamped into the widget bounds, with an exact halfway pixel tie
    // going to the greater index (checked integer arithmetic). Falls back to a defensively clamped
    // floating approximation only if the exact product would overflow std::uint64_t -- unreachable
    // for any realistic composition duration/frame rate combined with a practical widget width, but
    // kept safe rather than UB, matching FrameTimeMapping's own defensive-clamp precedent.
    [[nodiscard]] std::uint64_t frameIndexForPixel(const int pixelX) const noexcept {
        if (widthPixels <= 1 || maxIndex == 0) {
            return 0;
        }
        const int clamped = std::clamp(pixelX, 0, widthPixels - 1);
        const auto span = static_cast<std::uint64_t>(widthPixels - 1);
        const auto position = static_cast<std::uint64_t>(clamped);
        if (position != 0 && maxIndex > std::numeric_limits<std::uint64_t>::max() / position) {
            const double fraction = static_cast<double>(clamped) / static_cast<double>(span);
            const double approximate = std::clamp(fraction * static_cast<double>(maxIndex), 0.0,
                                                  static_cast<double>(maxIndex));
            return static_cast<std::uint64_t>(approximate);
        }
        const auto product = position * maxIndex;
        const auto quotient = product / span;
        const auto remainder = product % span;
        const auto tiedUp = (remainder * 2 >= span) ? quotient + 1 : quotient;
        return std::min(tiedUp, maxIndex);
    }

    // Presentational time -> pixel (not a contract decision): clamps into [0, duration] so a key or
    // playhead fractionally outside the composition's range still paints at a visible edge.
    [[nodiscard]] qreal pixelForTime(const core::RationalTime time) const noexcept {
        if (widthPixels <= 1) {
            return 0.0;
        }
        const double durationSeconds = duration.toSeconds();
        if (!(durationSeconds > 0.0)) {
            return 0.0;
        }
        const double fraction = std::clamp(time.toSeconds() / durationSeconds, 0.0, 1.0);
        return static_cast<qreal>(fraction * static_cast<double>(widthPixels - 1));
    }
};

// The dense, unlabeled minor grid (decision 3: "minors as subtle ticks"): the smallest frame step
// whose pixel spacing is still at least kMinimumPixelsPerMinorTick apart.
[[nodiscard]] std::uint64_t minorTickStepFrames(const TimelineAxis& axis) {
    if (axis.maxIndex == 0 || axis.widthPixels <= 1) {
        return 1;
    }
    const double pixelsPerFrame =
        static_cast<double>(axis.widthPixels - 1) / static_cast<double>(axis.maxIndex);
    if (pixelsPerFrame >= kMinimumPixelsPerMinorTick) {
        return 1;
    }
    return static_cast<std::uint64_t>(std::ceil(kMinimumPixelsPerMinorTick / pixelsPerFrame));
}

// The labeled major grid (decision 3: "majors every N frames chosen from zoom/width so labels
// never collide"). `widestLabelPixels` is the ACTUAL rendered width of this axis's widest possible
// label (its own maxIndex, in the SAME font paintEvent uses) rather than a guessed pixel budget --
// see the caller. The step is kept an exact multiple of `minorStep` so every major tick lands on a
// minor tick rather than an off-grid position.
//
// Proof of disjointness (pinned by testRulerMajorTickLabelsNeverCollideAtTwoWidths): for two
// adjacent major indices i < j = i + majorStep, their left-aligned label rects start at
// pixelForTime(i) + inset and pixelForTime(j) + inset respectively, each at most widestLabelPixels
// wide (every in-range label's digit count is <= maxIndex's, and this font's digit glyphs are
// monospaced-within-a-weight so a shorter number never renders wider). The major step guarantees
// pixelForTime(j) - pixelForTime(i) >= widestLabelPixels + kMajorLabelGapPixels, so label i's
// right edge (start + width <= start + widestLabelPixels) sits strictly left of label j's left
// edge (start + widestLabelPixels + kMajorLabelGapPixels), for any pair of consecutive majors --
// and by induction, every non-adjacent pair too.
[[nodiscard]] std::uint64_t majorTickStepFrames(const TimelineAxis& axis,
                                                const qreal widestLabelPixels,
                                                const std::uint64_t minorStep) {
    const std::uint64_t flooredMinor = std::max<std::uint64_t>(minorStep, 1);
    if (axis.maxIndex == 0 || axis.widthPixels <= 1) {
        return flooredMinor;
    }
    const double pixelsPerFrame =
        static_cast<double>(axis.widthPixels - 1) / static_cast<double>(axis.maxIndex);
    const double neededPixels = static_cast<double>(widestLabelPixels) + kMajorLabelGapPixels;
    std::uint64_t step = flooredMinor;
    while (static_cast<double>(step) * pixelsPerFrame < neededPixels) {
        step += flooredMinor;
    }
    return step;
}

QString titleCase(const std::string_view role) {
    if (role.empty()) {
        return QStringLiteral("Parameter");
    }
    QString text = QString::fromUtf8(role.data(), static_cast<qsizetype>(role.size()));
    text.replace(0, 1, text.left(1).toUpper());
    return text;
}

struct KeyEntry final {
    document::KeyframeId id;
    core::RationalTime time;
};

// The Geist Mono tick/label font (decision 3), sized down slightly the same way the pre-restyle
// ruler already scaled its tick font -- ticks are secondary chrome, not a Value-role readout.
[[nodiscard]] QFont tickFont() {
    QFont font = kit::font(kit::TypeRole::Value);
    font.setPointSizeF(font.pointSizeF() * 0.9);
    return font;
}

struct MajorTickLabel final {
    std::uint64_t index = 0;
    QRectF rect;
};

// The single source of major-tick label geometry: paintEvent() and
// TimelineRuler::majorTickLabelRectsForTest() both call this, so a test can never observe a
// different collision-avoidance decision than what actually gets painted.
[[nodiscard]] std::vector<MajorTickLabel> computeMajorTickLabels(const TimelineAxis& axis,
                                                                 const qreal labelAreaHeight) {
    std::vector<MajorTickLabel> labels;
    const QFontMetrics metrics(tickFont());
    const qreal widestLabelPixels = metrics.horizontalAdvance(QString::number(axis.maxIndex));
    const auto minorStep = minorTickStepFrames(axis);
    const auto majorStep = majorTickStepFrames(axis, widestLabelPixels, minorStep);
    for (std::uint64_t index = 0; index <= axis.maxIndex; index += majorStep) {
        const auto time = frameTimeForIndex(axis.frameRate, axis.duration, index);
        if (!time.has_value()) {
            continue;
        }
        const qreal x = axis.pixelForTime(*time);
        const QString text = QString::number(index);
        const qreal textWidth = metrics.horizontalAdvance(text);
        labels.push_back(
            {index, QRectF(x + kTickLabelInsetPixels, 0.0, textWidth, labelAreaHeight)});
    }
    return labels;
}

} // namespace

// A single animated-parameter row: name label plus key lane. Deliberately not exposed via the
// header -- TimelineKeyframePanel owns it exclusively and forward-declares it (`class
// TimelineKeyframeRow*` in the FILE_SET header) purely to type its row list, so this definition
// must live directly in bloom::ui rather than in an anonymous namespace (which would make it a
// distinct, unrelated type from the header's forward declaration). It does not declare Q_OBJECT (no
// signals are needed; selection is reported by calling straight into CompositionSession, which the
// row already holds a reference to), keeping it free of moc.
//
// Press-drag-release gesture (issue #84, decision 3): press selects (existing); once the pointer
// moves past QApplication::startDragDistance() from the press point, drag mode arms -- a
// presentation-only ghost diamond at the snapped target frame (this row's own TimelineAxis, the
// SAME pixel<->frame mapping the ruler scrub uses) with NO document mutation until release, which
// executes exactly one CompositionSession::moveSelectedKeyframe() transaction. Escape mid-drag
// cancels with no transaction and clears the ghost; grabKeyboard()/releaseKeyboard() bracket the
// drag so a real Escape key press reaches this row regardless of focus.
class TimelineKeyframeRow final : public QWidget {
  public:
    TimelineKeyframeRow(CompositionSession& session, QString label,
                        const document::AnimationCurveId curveId, const bool isVec2,
                        QWidget* parent)
        : QWidget(parent), session_(session), label_(std::move(label)), curveId_(curveId),
          isVec2_(isVec2) {
        setFixedHeight(kKeyframeRowHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(&session_, &CompositionSession::currentTimeChanged, this, [this] { update(); });
        // The row set is memoized (TimelineKeyframePanel::rebuild()), so this row instance
        // typically survives a selection change or a document edit that leaves its own curve
        // intact -- it must repaint itself on both rather than rely on being recreated.
        connect(&session_, &CompositionSession::selectionChanged, this, [this] { update(); });
        connect(&session_, &CompositionSession::snapshotChanged, this, [this] { update(); });
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // One step up the surface ladder from the ruler's own Surface (decision 1's "row striping
        // via surface ladder" carried down into the lane rows, which sit directly beneath it).
        painter.fillRect(rect(), kit::color(kit::surfaceStep(kit::Color::Surface, 1)));
        kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
        painter.drawLine(QPointF(0.0, height() - 0.5), QPointF(width(), height() - 0.5));

        const auto* composition = session_.composition();
        if (composition == nullptr) {
            return;
        }
        const auto axis = TimelineAxis::create(*composition, width());
        if (!axis.has_value()) {
            return;
        }

        // Decision 4: the SAME Accent 2px line the ruler paints, continuing this row's own segment
        // of it (no head marker here -- that lives once, in the ruler).
        const qreal playheadX = axis->pixelForTime(session_.currentTime());
        painter.setPen(QPen(kit::color(kit::Color::Accent), kPlayheadLineWidth));
        painter.drawLine(QPointF(playheadX, 0.0), QPointF(playheadX, height()));

        const auto* keySelection = std::get_if<KeyframeSelection>(&session_.selection().primary);
        const qreal centerY = height() / 2.0;
        for (const auto& key : collectKeys()) {
            const qreal x = axis->pixelForTime(key.time);
            const bool selected = keySelection != nullptr && keySelection->curveId == curveId_ &&
                                  keySelection->keyframeId == key.id;
            // Decision 2: "gold diamonds, Accent selection" -- Keyframe is the token every other
            // keyframe indicator in the interface already uses (PropertiesEditor's own
            // updateKeyframeIndicator()) for exactly this "gold" meaning.
            const QColor fill =
                selected ? kit::color(kit::Color::Accent) : kit::color(kit::Color::Keyframe);
            painter.setPen(QPen(fill.darker(140), 1.0));
            painter.setBrush(fill);
            QPolygonF diamond;
            diamond << QPointF(x, centerY - kKeyDiamondRadius)
                    << QPointF(x + kKeyDiamondRadius, centerY)
                    << QPointF(x, centerY + kKeyDiamondRadius)
                    << QPointF(x - kKeyDiamondRadius, centerY);
            painter.drawPolygon(diamond);
        }

        if (dragging_ && ghostTime_.has_value()) {
            // Presentation-only: paint the snapped drag target, never mutate the document mid-drag.
            const qreal x = axis->pixelForTime(*ghostTime_);
            QColor ghostColor = kit::color(kit::Color::Accent);
            ghostColor.setAlpha(150);
            painter.setPen(QPen(ghostColor.darker(120), 1.5, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            QPolygonF diamond;
            diamond << QPointF(x, centerY - kKeyDiamondRadius)
                    << QPointF(x + kKeyDiamondRadius, centerY)
                    << QPointF(x, centerY + kKeyDiamondRadius)
                    << QPointF(x - kKeyDiamondRadius, centerY);
            painter.drawPolygon(diamond);
        }

        painter.setFont(kit::font(kit::TypeRole::UiSmall));
        const QFontMetrics metrics = painter.fontMetrics();
        const QRectF chip(kit::px(kit::Spacing::XXS), kit::px(kit::Spacing::XXS),
                          metrics.horizontalAdvance(label_) + kit::px(kit::Spacing::S),
                          height() - 2.0 * kit::px(kit::Spacing::XXS));
        kit::fillRoundedSurface(painter, chip, kit::color(kit::Color::SurfaceRaised), QColor(),
                                kit::Radius::Small);
        painter.setPen(kit::color(kit::Color::Muted));
        painter.drawText(chip, Qt::AlignCenter, label_);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        const auto* composition = session_.composition();
        if (composition == nullptr) {
            return;
        }
        const auto axis = TimelineAxis::create(*composition, width());
        if (!axis.has_value()) {
            return;
        }

        const qreal clickX = event->position().x();
        const auto closest = hitTestKey(*axis, clickX);
        if (!closest.has_value()) {
            return;
        }
        session_.selectKeyframe(curveId_, *closest);
        if (auto* panel = parentWidget()) {
            // TimelineKeyframePanel accepts focus so a real Delete/Backspace press reaches it after
            // a click selects a key.
            panel->setFocus(Qt::MouseFocusReason);
        }
        pressedKeyId_ = closest;
        pressPos_ = event->position();
        dragging_ = false;
        ghostTime_.reset();
    }

    // Insert gesture (issue #86, task E1; decision 4): double-clicking the row BACKGROUND (never an
    // existing key -- same hit-test tolerance/idiom as press-select above) inserts a new key at the
    // clicked frame-snapped time, using the SAME TimelineAxis pixel<->frame mapping the drag
    // gesture already snaps to (frameIndexForPixel() + frameTimeForIndex()). CompositionSession::
    // insertKeyframeAtTime() owns the command construction, value sampling, occupied-time refusal,
    // and the one-truth selection swap on success; this handler is pure hit-testing/dispatch.
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QWidget::mouseDoubleClickEvent(event);
            return;
        }
        const auto* composition = session_.composition();
        if (composition == nullptr) {
            return;
        }
        const auto axis = TimelineAxis::create(*composition, width());
        if (!axis.has_value()) {
            return;
        }
        const qreal clickX = event->position().x();
        if (hitTestKey(*axis, clickX).has_value()) {
            // Landed on an existing key, not the lane background -- no insert gesture.
            return;
        }
        const auto index = axis->frameIndexForPixel(static_cast<int>(clickX));
        const auto snappedTime = frameTimeForIndex(axis->frameRate, axis->duration, index);
        if (!snappedTime.has_value()) {
            return;
        }
        if (session_.insertKeyframeAtTime(curveId_, *snappedTime)) {
            if (auto* panel = parentWidget()) {
                panel->setFocus(Qt::MouseFocusReason);
            }
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!pressedKeyId_.has_value()) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        const QPointF pos = event->position();
        if (!dragging_) {
            const QPointF delta = pos - pressPos_;
            const int manhattan =
                static_cast<int>(std::abs(delta.x())) + static_cast<int>(std::abs(delta.y()));
            if (manhattan < QApplication::startDragDistance()) {
                return;
            }
            dragging_ = true;
            grabKeyboard();
        }
        const auto* composition = session_.composition();
        if (composition == nullptr) {
            return;
        }
        const auto axis = TimelineAxis::create(*composition, width());
        if (!axis.has_value()) {
            return;
        }
        const auto index = axis->frameIndexForPixel(static_cast<int>(pos.x()));
        ghostTime_ = frameTimeForIndex(axis->frameRate, axis->duration, index);
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !pressedKeyId_.has_value()) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        if (dragging_) {
            releaseKeyboard();
            if (ghostTime_.has_value()) {
                // Refusal (duplicate time / model rejection) commits nothing and keeps the
                // selection; the ghost is cleared unconditionally below either way.
                (void)session_.moveSelectedKeyframe(*ghostTime_);
            }
        }
        endDrag();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (dragging_ && event->key() == Qt::Key_Escape) {
            releaseKeyboard();
            endDrag();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

  private:
    void endDrag() {
        dragging_ = false;
        pressedKeyId_.reset();
        ghostTime_.reset();
        update();
    }

    // Shared hit-test (press-select and double-click-insert both use it, same tolerance): the
    // closest key within kKeyHitToleranceLogicalPixels of `pixelX`, or nullopt if none is close
    // enough -- the latter is what marks a click as landing on the lane BACKGROUND.
    [[nodiscard]] std::optional<document::KeyframeId> hitTestKey(const TimelineAxis& axis,
                                                                 const qreal pixelX) const {
        std::optional<document::KeyframeId> closest;
        qreal closestDistance = std::numeric_limits<qreal>::max();
        for (const auto& key : collectKeys()) {
            const qreal distance = std::abs(axis.pixelForTime(key.time) - pixelX);
            if (distance <= kKeyHitToleranceLogicalPixels && distance < closestDistance) {
                closest = key.id;
                closestDistance = distance;
            }
        }
        return closest;
    }

    [[nodiscard]] std::vector<KeyEntry> collectKeys() const {
        std::vector<KeyEntry> entries;
        const auto* composition = session_.composition();
        if (composition == nullptr) {
            return entries;
        }
        if (isVec2_) {
            const auto* curve = composition->animationCurves().findVec2(curveId_);
            if (curve == nullptr) {
                return entries;
            }
            entries.reserve(curve->keyframes.size());
            for (const auto& key : curve->keyframes) {
                entries.push_back({key.id, key.time});
            }
        } else {
            const auto* curve = composition->animationCurves().findScalar(curveId_);
            if (curve == nullptr) {
                return entries;
            }
            entries.reserve(curve->keyframes.size());
            for (const auto& key : curve->keyframes) {
                entries.push_back({key.id, key.time});
            }
        }
        return entries;
    }

    CompositionSession& session_;
    QString label_;
    document::AnimationCurveId curveId_;
    bool isVec2_;
    bool dragging_ = false;
    std::optional<document::KeyframeId> pressedKeyId_;
    QPointF pressPos_;
    std::optional<core::RationalTime> ghostTime_;
};

namespace {

struct AnimatedParameterRow final {
    QString label;
    document::AnimationCurveId curveId;
    bool isVec2 = false;
};

[[nodiscard]] std::vector<AnimatedParameterRow>
collectAnimatedParameters(const CompositionSession& session) {
    std::vector<AnimatedParameterRow> rows;
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return rows;
    }

    std::optional<document::LayerId> layerId = session.selection().contextualLayer;
    if (!layerId.has_value()) {
        if (const auto* direct = std::get_if<document::LayerId>(&session.selection().primary)) {
            layerId = *direct;
        }
    }
    if (!layerId.has_value()) {
        return rows;
    }

    const auto boundaryNodeId = session.boundaryNodeForLayer(*layerId);
    if (!boundaryNodeId.has_value()) {
        return rows;
    }
    const auto* node = composition->graph().findNode(*boundaryNodeId);
    if (node == nullptr) {
        return rows;
    }

    for (const auto& binding : node->parameters) {
        const auto* parameter = composition->parameters().find(binding.parameterId);
        if (parameter == nullptr) {
            continue;
        }
        const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
        if (source == nullptr) {
            continue;
        }
        const bool isVec2 = composition->animationCurves().findVec2(source->curveId) != nullptr;
        rows.push_back({titleCase(binding.role), source->curveId, isVec2});
    }
    return rows;
}

} // namespace

TimelineRuler::TimelineRuler(CompositionSession& session,
                             CompositionPreviewController& previewController, QWidget* parent)
    : QWidget(parent), session_(session), previewController_(previewController) {
    setObjectName("timelineRuler");
    setAccessibleName(tr("Scrub ruler"));
    setFixedHeight(kRulerHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            qOverload<>(&TimelineRuler::update));
    connect(&session_, &CompositionSession::compositionChanged, this,
            qOverload<>(&TimelineRuler::update));
    connect(&session_, &CompositionSession::snapshotChanged, this,
            qOverload<>(&TimelineRuler::update));
}

void TimelineRuler::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    // Decision 3: "Surface bg, hairline base."
    painter.fillRect(rect(), kit::color(kit::Color::Surface));
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.drawLine(QPointF(0.0, height() - 0.5), QPointF(width(), height() - 0.5));

    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    const auto axis = TimelineAxis::create(*composition, width());
    if (!axis.has_value()) {
        return;
    }

    painter.setFont(tickFont());
    const auto labelAreaHeight = static_cast<qreal>(height()) - kMajorTickHeight;
    const auto majorLabels = computeMajorTickLabels(*axis, labelAreaHeight);
    const auto minorStep = minorTickStepFrames(*axis);

    // Minor grid first (decision 3: "minors as subtle ticks"), so a coincident major tick paints
    // on top of it below rather than the other way around.
    kit::applyHairlinePen(painter, kit::color(kit::Color::Faint));
    for (std::uint64_t index = 0; index <= axis->maxIndex; index += minorStep) {
        const auto time = frameTimeForIndex(axis->frameRate, axis->duration, index);
        if (!time.has_value()) {
            continue;
        }
        const qreal x = axis->pixelForTime(*time);
        painter.drawLine(QPointF(x, static_cast<qreal>(height()) - kMinorTickHeight),
                         QPointF(x, static_cast<qreal>(height()) - 1.0));
    }

    // Major grid: taller ticks plus a Geist Mono label (decision 3), density-adaptive so labels
    // never collide -- see computeMajorTickLabels()/majorTickStepFrames().
    for (const auto& label : majorLabels) {
        const auto time = frameTimeForIndex(axis->frameRate, axis->duration, label.index);
        if (!time.has_value()) {
            continue;
        }
        const qreal x = axis->pixelForTime(*time);
        kit::applyHairlinePen(painter, kit::color(kit::Color::Muted));
        painter.drawLine(QPointF(x, static_cast<qreal>(height()) - kMajorTickHeight),
                         QPointF(x, static_cast<qreal>(height()) - 1.0));
        painter.setPen(kit::color(kit::Color::Muted));
        painter.drawText(label.rect, Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(label.index));
    }

    // Playhead (decision 4): an Accent 2px line with a head marker, shared visually with every
    // keyframe lane row's own playhead segment below (TimelineKeyframeRow::paintEvent) so the line
    // reads as one continuous stroke down through the whole panel.
    const qreal playheadX = axis->pixelForTime(session_.currentTime());
    const QColor playheadColor = kit::color(kit::Color::Accent);
    painter.setPen(QPen(playheadColor, kPlayheadLineWidth));
    painter.drawLine(QPointF(playheadX, 0.0), QPointF(playheadX, height()));
    painter.setPen(Qt::NoPen);
    painter.setBrush(playheadColor);
    QPolygonF marker;
    marker << QPointF(playheadX - 5.0, 0.0) << QPointF(playheadX + 5.0, 0.0)
           << QPointF(playheadX, 6.0);
    painter.drawPolygon(marker);
}

std::vector<QRectF> TimelineRuler::majorTickLabelRectsForTest() const {
    std::vector<QRectF> rects;
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return rects;
    }
    const auto axis = TimelineAxis::create(*composition, width());
    if (!axis.has_value()) {
        return rects;
    }
    const auto labelAreaHeight = static_cast<qreal>(height()) - kMajorTickHeight;
    for (const auto& label : computeMajorTickLabels(*axis, labelAreaHeight)) {
        rects.push_back(label.rect);
    }
    return rects;
}

void TimelineRuler::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    scrubbing_ = true;
    previewController_.beginInteractiveScrub();
    scrubToPixel(static_cast<int>(event->position().x()));
}

void TimelineRuler::mouseMoveEvent(QMouseEvent* event) {
    if (!scrubbing_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    scrubToPixel(static_cast<int>(event->position().x()));
}

void TimelineRuler::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !scrubbing_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    scrubbing_ = false;
    scrubToPixel(static_cast<int>(event->position().x()));
    previewController_.notifyScrubEnded();
}

void TimelineRuler::scrubToPixel(const int pixelX) {
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    const auto axis = TimelineAxis::create(*composition, width());
    if (!axis.has_value()) {
        return;
    }
    const auto index = axis->frameIndexForPixel(pixelX);
    const auto exactTime = frameTimeForIndex(axis->frameRate, axis->duration, index);
    if (!exactTime.has_value()) {
        return;
    }
    (void)session_.setCurrentTime(*exactTime);
}

TimelineWorkAreaStrip::TimelineWorkAreaStrip(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("timelineWorkAreaStrip");
    setAccessibleName(tr("Work area"));
    setFixedHeight(kWorkAreaStripHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(&session_, &CompositionSession::compositionChanged, this,
            qOverload<>(&TimelineWorkAreaStrip::update));
    connect(&session_, &CompositionSession::snapshotChanged, this,
            qOverload<>(&TimelineWorkAreaStrip::update));
}

void TimelineWorkAreaStrip::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    // Background chrome (Surface) shows through when no composition is active -- an honest empty
    // strip, not a stale/leftover band.
    painter.fillRect(rect(), kit::color(kit::Color::Surface));
    if (session_.composition() == nullptr) {
        return;
    }
    // The honest current work area is the WHOLE [0, duration) range (decision 3: no range-editing
    // feature exists, so there is no separate in/out point to visualize) -- the band therefore
    // always spans this widget's full width by construction, never a fake partial trim. Reuses
    // kDisabledOpacity as the "dim" fraction (the same "dimmed ink and disabled ink are the same
    // fade recipe" precedent PropertiesEditor's updateKeyframeIndicator() already uses) rather than
    // inventing a new opacity literal.
    painter.fillRect(rect(), kit::withOpacity(kit::color(kit::Color::Accent), kit::kDisabledOpacity));
}

TimelineKeyframePanel::TimelineKeyframePanel(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("timelineKeyframePanel");
    setAccessibleName(tr("Animated parameter keys"));
    // Accepts focus so a click-selected key (TimelineKeyframeRow::mousePressEvent focuses this
    // panel) can be deleted with Delete/Backspace (issue #84, decision 2).
    setFocusPolicy(Qt::StrongFocus);
    rowsLayout_ = new QVBoxLayout(this);
    rowsLayout_->setContentsMargins(0, 0, 0, 0);
    rowsLayout_->setSpacing(0);
    connect(&session_, &CompositionSession::snapshotChanged, this, &TimelineKeyframePanel::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &TimelineKeyframePanel::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &TimelineKeyframePanel::rebuild);
    rebuild();
}

void TimelineKeyframePanel::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        // deleteSelectedKeyframe() is a no-op false (no transaction, selection intact) when
        // nothing is selected or the command layer refuses (e.g. the curve's last key) -- the
        // widget has nothing further to do beyond the existing projection updates either way.
        (void)session_.deleteSelectedKeyframe();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TimelineKeyframePanel::rebuild() {
    const auto specs = collectAnimatedParameters(session_);
    std::vector<document::AnimationCurveId> currentCurveIds;
    currentCurveIds.reserve(specs.size());
    for (const auto& spec : specs) {
        currentCurveIds.push_back(spec.curveId);
    }
    if (currentCurveIds == lastCurveIds_) {
        // The row SET is unchanged (e.g. a keyframe selection/move/delete that leaves the same
        // curves in place) -- existing rows already repaint themselves off selectionChanged/
        // snapshotChanged, so skip tearing them down. This also sidesteps a real hazard: a row's
        // own click emits selectionChanged synchronously (CompositionSession::selectKeyframe()),
        // and destroying that same row from inside its own mousePressEvent would be a use-after-
        // free.
        setVisible(!specs.empty());
        return;
    }
    lastCurveIds_ = currentCurveIds;

    QLayoutItem* item = nullptr;
    while ((item = rowsLayout_->takeAt(0)) != nullptr) {
        if (auto* widget = item->widget()) {
            // setParent(nullptr) first: takeAt() only detaches the item from the LAYOUT, the
            // widget stays a child of this panel (and so still visible to findChildren()) until
            // reparented. deleteLater(), not delete, for the actual destruction: rebuild() can
            // itself be reached (via selectionChanged) from a row's own mouse event handler when
            // the row SET does change (e.g. selecting a keyframe on a newly-selected layer's first
            // row); deferring destruction keeps that call stack safe even in that case.
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
    rows_.clear();

    rows_.reserve(specs.size());
    for (const auto& spec : specs) {
        auto* row = new TimelineKeyframeRow(session_, spec.label, spec.curveId, spec.isVec2, this);
        rowsLayout_->addWidget(row);
        rows_.push_back(row);
    }

    setVisible(!specs.empty());
}

} // namespace bloom::ui
