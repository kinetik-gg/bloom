#include <QRegion>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_frame_math.hpp>

#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QWheelEvent>

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

void paintPlayheadLine(QPainter& painter, const TimelineAxis& axis, const core::RationalTime time,
                       const qreal heightPixels) {
    if (time.toSeconds() < axis.t0 || time.toSeconds() >= axis.t1) {
        return;
    }
    // Snapped to the centre of one whole pixel column: a 1px pen on an integer x straddles the
    // boundary between two columns at some device pixel ratios and reads as two half-lit columns,
    // which is exactly the lie docs/ux/visual-language.md's Motion section forbids for a playhead.
    const qreal x = std::floor(axis.pixelForTime(time)) + 0.5;
    const bool wasAntialiased = painter.renderHints().testFlag(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(kit::color(kit::Color::Accent), kPlayheadLineWidth));
    painter.drawLine(QPointF(x, 0.0), QPointF(x, heightPixels));
    painter.setRenderHint(QPainter::Antialiasing, wasAntialiased);
}

namespace {

// Ruler/lane extents (task TL-C1): TimelineRow (32px) is the token every
// track row -- header AND lane -- now shares; the ruler keeps its own extent (no dedicated
// "ruler height" token exists) but is still resolved from Control (26px) rather than a bare
// literal, matching this file's own prior 26px value exactly.
const int kRulerHeight = kit::px(kit::Size::Control);
// Task T1: the key lanes step by the panel's shared row pitch, so a key row lines up with the layer
// rows and clip lanes above it instead of being two pixels taller than all of them.
const int kKeyframeRowHeight = kTimelineRowHeight;
// The honest work-area strip (decision 3): thin, using the smallest spacing token rather than an
// invented pixel gap.
const int kWorkAreaStripHeight = kit::px(kit::Size::TimelineWorkArea);
constexpr qreal kKeyDiamondRadius = 4.5;
constexpr qreal kKeyHitToleranceLogicalPixels = 6.0;
// Minor ticks are a dense, purely visual grid (decision 3: "minors as subtle ticks"); majors are
// re-derived per paint from the axis's OWN font metrics so adjacent labels can never collide (see
// majorTickStepFrames() below) rather than reusing this fixed minor-tick pixel budget for labels
// the way the pre-restyle single-density ruler did.
constexpr qreal kMinimumPixelsPerMinorTick = 8.0;
constexpr qreal kMinimumPixelsPerMajorTick = 40.0;
// Extra breathing room between two adjacent major labels, beyond their own widest possible text
// width -- keeps the collision-avoidance math from packing labels edge-to-edge.
constexpr qreal kMajorLabelGapPixels = 10.0;
constexpr qreal kTickLabelInsetPixels = 3.0;
constexpr qreal kMinorTickHeight = 4.0;
constexpr qreal kMajorTickHeight = 8.0;

// One minor per frame while frames have room to breathe; otherwise keep the ruler legible with
// five-frame minors.
[[nodiscard]] std::uint64_t minorTickStepFrames(const TimelineAxis& axis) {
    return axis.pixelsPerFrame() >= kMinimumPixelsPerMinorTick ? 1 : 5;
}

// Choose a readable frame cadence using the actual label font. Geometry is checked again when
// admitting labels, including timecode field-width changes and narrow right-edge clipping.
[[nodiscard]] std::uint64_t majorTickStepFrames(const TimelineAxis& axis,
                                                const qreal widestLabelPixels,
                                                const std::uint64_t minorStep) {
    const std::uint64_t flooredMinor = std::max<std::uint64_t>(minorStep, 1);
    if (axis.maxIndex == 0 || axis.widthPixels <= 1) {
        return flooredMinor;
    }
    const double pixelsPerFrame = axis.pixelsPerFrame();
    const double neededFrames =
        std::max(kMinimumPixelsPerMajorTick, widestLabelPixels + kMajorLabelGapPixels) /
        pixelsPerFrame;
    for (const std::uint64_t step : {1ULL, 2ULL, 5ULL, 10ULL, 24ULL, 48ULL}) {
        if (static_cast<double>(step) >= neededFrames) {
            return step;
        }
    }
    std::uint64_t step = 48;
    while (static_cast<double>(step) < neededFrames &&
           step <= std::numeric_limits<std::uint64_t>::max() / 2) {
        step *= 2;
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
    // Task S5, item 2: the key's OUTGOING interpolation, so the lane can give each mode its own
    // glyph instead of one diamond for everything.
    document::KeyframeInterpolation outgoingInterpolation = document::KeyframeInterpolation::Linear;
};

// The glyph for one interpolation mode (task S5, item 2: "timeline keyframe glyph differs per
// interpolation"). The shapes are the standard timeline vocabulary, and each one says what the
// SEGMENT LEAVING the key does:
//
//   Linear     a diamond  -- the shape every key had before this task; a straight ramp out
//   Hold       a square   -- a held step out, drawn with the same corner-to-corner extent
//   EaseInOut  a circle   -- a rounded departure, matching the rounded ease it names
//
// Shape, not colour, carries the mode: the gold/Accent colour pair is already spoken for by
// unselected/selected, and overloading it would make a selected Hold key indistinguishable from an
// unselected eased one.
void paintKeyGlyph(QPainter& painter, const QPointF center,
                   const document::KeyframeInterpolation interpolation) {
    switch (interpolation) {
    case document::KeyframeInterpolation::Hold: {
        const qreal half = kKeyDiamondRadius * 0.78;
        painter.drawRect(QRectF(center.x() - half, center.y() - half, 2.0 * half, 2.0 * half));
        return;
    }
    case document::KeyframeInterpolation::EaseInOut:
        painter.drawEllipse(center, kKeyDiamondRadius * 0.92, kKeyDiamondRadius * 0.92);
        return;
    case document::KeyframeInterpolation::Linear:
        break;
    }
    QPolygonF diamond;
    diamond << QPointF(center.x(), center.y() - kKeyDiamondRadius)
            << QPointF(center.x() + kKeyDiamondRadius, center.y())
            << QPointF(center.x(), center.y() + kKeyDiamondRadius)
            << QPointF(center.x() - kKeyDiamondRadius, center.y());
    painter.drawPolygon(diamond);
}

[[nodiscard]] QString
interpolationDisplayName(const document::KeyframeInterpolation interpolation) {
    switch (interpolation) {
    case document::KeyframeInterpolation::Hold:
        return QStringLiteral("Hold");
    case document::KeyframeInterpolation::Linear:
        return QStringLiteral("Linear");
    case document::KeyframeInterpolation::EaseInOut:
        return QStringLiteral("Ease In-Out");
    }
    return QStringLiteral("Linear");
}

// Timeline labels are Value-role readouts. Keep the kit font at its declared size so density is
// controlled by cadence, never by silently shrinking the numbers.
[[nodiscard]] QFont tickFont() {
    return kit::font(kit::TypeRole::Value);
}

struct MajorTickLabel final {
    std::uint64_t index = 0;
    QRectF rect;
};

// The single source of major-tick label geometry: paintEvent() and
// TimelineRuler::majorTickLabelRectsForTest() both call this, so a test can never observe a
// different collision-avoidance decision than what actually gets painted.
[[nodiscard]] std::vector<MajorTickLabel>
computeMajorTickLabels(const TimelineAxis& axis, const qreal labelAreaHeight, const bool timecode) {
    std::vector<MajorTickLabel> labels;
    const QFontMetrics metrics(tickFont());
    const qreal widestLabelPixels = metrics.horizontalAdvance(
        formatTimelineFrameLabel(axis.maxIndex, axis.frameRate, timecode));
    const auto minorStep = minorTickStepFrames(axis);
    const auto majorStep = majorTickStepFrames(axis, widestLabelPixels, minorStep);
    for (std::uint64_t index = (axis.frameIndexForPixel(0) / majorStep) * majorStep;
         index <= axis.maxIndex; index += majorStep) {
        const auto time = frameTimeForIndex(axis.frameRate, axis.duration, index);
        if (!time.has_value()) {
            continue;
        }
        const qreal x = axis.pixelForTime(*time);
        if (x > axis.widthPixels) {
            break;
        }
        if (x < 0) {
            continue;
        }
        const QString text = formatTimelineFrameLabel(index, axis.frameRate, timecode);
        const qreal textWidth = metrics.horizontalAdvance(text);
        if (x + kTickLabelInsetPixels + textWidth <= axis.widthPixels &&
            (labels.empty() ||
             x + kTickLabelInsetPixels > labels.back().rect.right() + kMajorLabelGapPixels)) {
            labels.push_back(
                {index, QRectF(x + kTickLabelInsetPixels, 0.0, textWidth, labelAreaHeight)});
        }
        if (axis.maxIndex - index < majorStep) {
            break;
        }
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
                        const document::AnimationCurveId curveId, QWidget* parent)
        : QWidget(parent), session_(session), label_(std::move(label)), curveId_(curveId) {
        setFixedHeight(kKeyframeRowHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(&session_, &CompositionSession::currentTimeChanged, this, [this] { update(); });
        // The row set is memoized (TimelineKeyframePanel::rebuild()), so this row instance
        // typically survives a selection change or a document edit that leaves its own curve
        // intact -- it must repaint itself on both rather than rely on being recreated.
        connect(&session_, &CompositionSession::selectionChanged, this, [this] { update(); });
        connect(&session_, &CompositionSession::snapshotChanged, this, [this] { update(); });
    }

    void setRuler(TimelineRuler& ruler) {
        ruler_ = &ruler;
        connect(&ruler, &TimelineRuler::axisChanged, this, [this] { update(); });
        update();
    }

  protected:
    void wheelEvent(QWheelEvent* event) override {
        if (ruler_ == nullptr || !ruler_->handleWheel(event)) {
            QWidget::wheelEvent(event);
        }
    }
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
        const auto axis = ruler_ != nullptr ? ruler_->axisForWidth(width())
                                            : TimelineAxis::create(*composition, width());
        if (!axis.has_value()) {
            return;
        }

        // The SAME 1px Accent stroke the ruler and the lane region paint, continuing this row's own
        // segment of it (no head marker here -- that lives once, in the work-area header row).
        paintPlayheadLine(painter, *axis, session_.currentTime(), height());

        const qreal centerY = height() / 2.0;
        for (const auto& key : collectKeys()) {
            if (key.time.toSeconds() < axis->t0 || key.time.toSeconds() >= axis->t1) {
                continue;
            }
            const qreal x = axis->pixelForTime(key.time);
            const bool selected = std::ranges::find(session_.selection().keyframes,
                                                    KeyframeSelection{curveId_, key.id}) !=
                                  session_.selection().keyframes.end();
            // Decision 2: "gold diamonds, Accent selection" -- Keyframe is the token every other
            // keyframe indicator in the interface already uses (PropertiesEditor's own
            // updateKeyframeIndicator()) for exactly this "gold" meaning.
            const QColor fill =
                selected ? kit::color(kit::Color::Accent) : kit::color(kit::Color::Keyframe);
            painter.setPen(QPen(fill.darker(140), 1.0));
            painter.setBrush(fill);
            paintKeyGlyph(painter, QPointF(x, centerY), key.outgoingInterpolation);
        }

        if (dragging_ && ghostTime_.has_value()) {
            // Presentation-only: paint the snapped drag target, never mutate the document mid-drag.
            const qreal x = axis->pixelForTime(*ghostTime_);
            QColor ghostColor = kit::color(kit::Color::Accent);
            ghostColor.setAlpha(150);
            painter.setPen(QPen(ghostColor.darker(120), 1.5, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            // The ghost wears the DRAGGED key's own glyph, so a Hold key being moved still reads as
            // a Hold key rather than turning into a diamond for the duration of the gesture.
            paintKeyGlyph(painter, QPointF(x, centerY), ghostInterpolation_);
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
        if (auto* panel = qobject_cast<TimelineKeyframePanel*>(parentWidget());
            panel && panel->gridMode())
            panel->paintGridOverlay(painter, *this);
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
        const auto axis = ruler_ != nullptr ? ruler_->axisForWidth(width())
                                            : TimelineAxis::create(*composition, width());
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
        ghostInterpolation_ = interpolationOf(*closest);
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
        const auto axis = ruler_ != nullptr ? ruler_->axisForWidth(width())
                                            : TimelineAxis::create(*composition, width());
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

    // The key context menu (task S5, item 2): right-clicking a key selects it, then offers its
    // three outgoing interpolations and Delete. Command construction lives in CompositionSession,
    // exactly as it does for every other gesture on this row; this handler is hit-testing and
    // dispatch only.
    //
    // Hold/Ease In-Out are DISABLED on a curve's final key rather than offered and refused: that
    // key's interpolation is canonical Linear (docs/architecture/animation-and-time.md), so the
    // command layer would reject the pick and the menu would have promised something it cannot do.
    // Delete is likewise disabled on a single-key curve, whose last key DeleteKeyframe refuses --
    // removing a parameter's whole animation is the diamond's job, not this menu's.
    void contextMenuEvent(QContextMenuEvent* event) override {
        const auto* composition = session_.composition();
        if (composition == nullptr) {
            return;
        }
        const auto axis = ruler_ != nullptr ? ruler_->axisForWidth(width())
                                            : TimelineAxis::create(*composition, width());
        if (!axis.has_value()) {
            return;
        }
        const auto hit = hitTestKey(*axis, static_cast<qreal>(event->pos().x()));
        if (!hit.has_value()) {
            return;
        }
        session_.selectKeyframe(curveId_, *hit);
        const auto current = session_.selectedKeyframeInterpolation();
        const bool isFinal = session_.selectedKeyframeIsFinal();
        const auto keys = collectKeys();

        QMenu menu(this);
        auto* group = new QActionGroup(&menu);
        group->setExclusive(true);
        for (const auto mode :
             {document::KeyframeInterpolation::Hold, document::KeyframeInterpolation::Linear,
              document::KeyframeInterpolation::EaseInOut}) {
            auto* action = menu.addAction(interpolationDisplayName(mode));
            action->setObjectName(QStringLiteral("keyframeInterpolationAction"));
            action->setCheckable(true);
            action->setChecked(current.has_value() && *current == mode);
            action->setEnabled(!isFinal || mode == document::KeyframeInterpolation::Linear);
            group->addAction(action);
            connect(action, &QAction::triggered, this,
                    [this, mode] { (void)session_.setSelectedKeyframeInterpolation(mode); });
        }
        menu.addSeparator();
        auto* remove = menu.addAction(QStringLiteral("Delete Keyframe"));
        remove->setObjectName(QStringLiteral("keyframeDeleteAction"));
        remove->setEnabled(keys.size() > 1);
        connect(remove, &QAction::triggered, this,
                [this] { (void)session_.deleteSelectedKeyframe(); });
        menu.exec(event->globalPos());
        event->accept();
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
        const auto axis = ruler_ != nullptr ? ruler_->axisForWidth(width())
                                            : TimelineAxis::create(*composition, width());
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
            if (key.time.toSeconds() < axis.t0 || key.time.toSeconds() >= axis.t1) {
                continue;
            }
            const qreal distance = std::abs(axis.pixelForTime(key.time) - pixelX);
            if (distance <= kKeyHitToleranceLogicalPixels && distance < closestDistance) {
                closest = key.id;
                closestDistance = distance;
            }
        }
        return closest;
    }

    // The outgoing interpolation of one key on this row's curve, or Linear when it no longer
    // resolves. Only the drag ghost needs it, and only at press time.
    [[nodiscard]] document::KeyframeInterpolation
    interpolationOf(const document::KeyframeId keyframeId) const {
        for (const auto& key : collectKeys()) {
            if (key.id == keyframeId) {
                return key.outgoingInterpolation;
            }
        }
        return document::KeyframeInterpolation::Linear;
    }

    [[nodiscard]] std::vector<KeyEntry> collectKeys() const {
        std::vector<KeyEntry> entries;
        const auto* composition = session_.composition();
        if (composition == nullptr) {
            return entries;
        }
        // One visit over the curve-kind variant, so a colour curve's lane (task S5, item 1) needs
        // no third copy of this loop -- and so the `isVec2_` flag no longer has to enumerate kinds.
        const auto* record = composition->animationCurves().find(curveId_);
        if (record == nullptr) {
            return entries;
        }
        std::visit(
            [&entries](const auto& curve) {
                entries.reserve(curve.keyframes.size());
                for (const auto& key : curve.keyframes) {
                    entries.push_back({key.id, key.time, key.outgoingInterpolation});
                }
            },
            *record);
        return entries;
    }

    TimelineRuler* ruler_ = nullptr;
    CompositionSession& session_;
    QString label_;
    document::AnimationCurveId curveId_;
    bool dragging_ = false;
    std::optional<document::KeyframeId> pressedKeyId_;
    QPointF pressPos_;
    std::optional<core::RationalTime> ghostTime_;
    // The pressed key's own outgoing interpolation, captured at press so the drag ghost keeps its
    // shape without re-walking the curve on every mouse move.
    document::KeyframeInterpolation ghostInterpolation_ = document::KeyframeInterpolation::Linear;
};

namespace {

struct AnimatedParameterRow final {
    QString label;
    document::AnimationCurveId curveId;
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

    // BOTH of the layer's nodes, in evaluation order: its Layer Output boundary (the transform and
    // opacity) and the source node feeding it (task S5, item 1 made a solid's colour and a text
    // layer's size and colour animatable, and those parameters live on the SOURCE node -- a lane
    // set that only walked the boundary would silently hide every key the new diamonds create).
    // Enumerated from each node's own bindings rather than from a list kept here, so this panel
    // still cannot carry a narrower idea of what is animatable than the schema does.
    std::vector<document::NodeId> nodeIds;
    if (const auto boundaryNodeId = session.boundaryNodeForLayer(*layerId)) {
        nodeIds.push_back(*boundaryNodeId);
    }
    if (const auto sourceNodeId = session.directSourceNodeForLayer(*layerId)) {
        nodeIds.push_back(*sourceNodeId);
    }

    for (const auto nodeId : nodeIds) {
        const auto* node = composition->graph().findNode(nodeId);
        if (node == nullptr) {
            continue;
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
            rows.push_back({titleCase(binding.role), source->curveId});
        }
    }
    return rows;
}

} // namespace

TimelineRuler::TimelineRuler(CompositionSession& session,
                             CompositionPreviewController& previewController, QWidget* parent)
    : QWidget(parent), session_(session), previewController_(previewController) {
    connect(&previewController_.frameCache(), &PreviewFrameCache::contentsChanged, this,
            qOverload<>(&TimelineRuler::update));
    connect(&previewController_, &CompositionPreviewController::resolutionChanged, this,
            qOverload<>(&TimelineRuler::update));
    setObjectName("timelineRuler");
    setAccessibleName(tr("Scrub ruler"));
    setFocusPolicy(Qt::StrongFocus);
    setFixedHeight(kRulerHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            qOverload<>(&TimelineRuler::update));
    connect(&session_, &CompositionSession::compositionChanged, this, &TimelineRuler::zoomToFit);
    connect(this, &TimelineRuler::axisChanged, this, qOverload<>(&TimelineRuler::update));
    connect(&session_, &CompositionSession::snapshotChanged, this, &TimelineRuler::axisChanged);
    connect(&session_, &CompositionSession::snapshotChanged, this,
            qOverload<>(&TimelineRuler::update));
}

void TimelineRuler::setTimecodeLabels(const bool timecode) {
    timecodeLabels_ = timecode;
    update();
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
    const auto axis = axisForWidth(width());
    if (!axis.has_value()) {
        return;
    }

    painter.setFont(tickFont());
    const auto labelAreaHeight = static_cast<qreal>(height()) - kMajorTickHeight;
    const auto majorLabels = computeMajorTickLabels(*axis, labelAreaHeight, timecodeLabels_);
    const auto minorStep = minorTickStepFrames(*axis);

    // Minor grid first (decision 3: "minors as subtle ticks"), so a coincident major tick paints
    // on top of it below rather than the other way around.
    kit::applyHairlinePen(painter, kit::color(kit::Color::Faint));
    for (std::uint64_t index = (axis->frameIndexForPixel(0) / minorStep) * minorStep;
         index <= axis->maxIndex; index += minorStep) {
        const auto time = frameTimeForIndex(axis->frameRate, axis->duration, index);
        if (!time.has_value()) {
            continue;
        }
        const qreal x = axis->pixelForTime(*time);
        if (x > width()) {
            break;
        }
        painter.drawLine(QPointF(x, static_cast<qreal>(height()) - kMinorTickHeight),
                         QPointF(x, static_cast<qreal>(height()) - 1.0));
        if (axis->maxIndex - index < minorStep) {
            break;
        }
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
                         formatTimelineFrameLabel(label.index, axis->frameRate, timecodeLabels_));
    }

    for (const auto& segment : cachedFrameRects()) {
        painter.fillRect(segment, kit::color(kit::Color::Ok));
    }

    // Playhead: the shared 1px Accent stroke, with its single marker and frame readout in this
    // ruler. The label follows the marker and uses the same Value role as the tick labels.
    paintPlayheadLine(painter, *axis, session_.currentTime(), height());
    const qreal playheadX = std::floor(axis->pixelForTime(session_.currentTime())) + 0.5;
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(kit::color(kit::Color::Accent));
    QPolygonF marker;
    marker << QPointF(playheadX - kPlayheadMarkerHalfWidth, 0.0)
           << QPointF(playheadX + kPlayheadMarkerHalfWidth, 0.0)
           << QPointF(playheadX, kPlayheadMarkerHeight);
    painter.drawPolygon(marker);

    const auto frame = axis->frameIndexForPixel(static_cast<int>(std::lround(playheadX)));
    const QString frameLabel = formatTimelineFrameLabel(frame, axis->frameRate, timecodeLabels_);
    const QFontMetrics frameMetrics(tickFont());
    const qreal labelWidth = frameMetrics.horizontalAdvance(frameLabel);
    const qreal labelX = std::clamp(playheadX + kPlayheadMarkerHalfWidth +
                                        kit::px(kit::Spacing::XS),
                                    0.0, std::max(0.0, width() - labelWidth));
    painter.setFont(tickFont());
    painter.setPen(kit::color(kit::Color::Foreground));
    painter.drawText(QRectF(labelX, 0.0, labelWidth, static_cast<qreal>(height())),
                     Qt::AlignLeft | Qt::AlignVCenter, frameLabel);
}

std::vector<QRectF> TimelineRuler::cachedFrameRects() const {
    std::vector<QRectF> segments;
    const auto* composition = session_.composition();
    const auto probe = previewController_.cacheKeyForTime(session_.currentTime());
    if (composition == nullptr || !probe.has_value()) {
        return segments;
    }
    const auto axis = axisForWidth(width());
    const auto rate = composition->format().frameRate();
    const auto mapping = core::FrameTimeMapping::create(composition->duration(), rate.numerator(),
                                                        rate.denominator());
    if (!axis.has_value() || !mapping.hasValue()) {
        return segments;
    }
    const qreal barHeight = kit::px(kit::Spacing::XXS);
    for (const auto time : previewController_.frameCache().timesFor(*probe)) {
        const auto index = mapping.value()->nearestFrameIndex(time);
        const auto exact = mapping.value()->timeForFrame(index);
        if (!exact.hasValue() || *exact.value() != time) {
            continue; // A cached subframe does not certify an entire frame-grid sample.
        }
        auto end = composition->duration();
        if (index < mapping.value()->maximumFrameIndex()) {
            const auto next = mapping.value()->timeForFrame(index + 1);
            if (!next.hasValue()) {
                continue;
            }
            end = *next.value();
        }
        if (time.toSeconds() >= axis->t1 || end.toSeconds() <= axis->t0) {
            continue;
        }
        const auto left = std::max(0.0, axis->pixelForTime(time));
        const auto right = std::min(static_cast<qreal>(width() - 1), axis->pixelForTime(end));
        if (right > left) {
            segments.emplace_back(left, height() - barHeight, right - left, barHeight);
        }
    }
    return segments;
}

std::vector<QRectF> TimelineRuler::majorTickLabelRectsForTest() const {
    std::vector<QRectF> rects;
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return rects;
    }
    const auto axis = axisForWidth(width());
    if (!axis.has_value()) {
        return rects;
    }
    const auto labelAreaHeight = static_cast<qreal>(height()) - kMajorTickHeight;
    for (const auto& label : computeMajorTickLabels(*axis, labelAreaHeight, timecodeLabels_)) {
        rects.push_back(label.rect);
    }
    return rects;
}

void TimelineRuler::beginScrub(const int pixelX) {
    scrubbing_ = true;
    previewController_.beginInteractiveScrub();
    scrubToPixel(pixelX);
}

void TimelineRuler::updateScrub(const int pixelX) {
    if (!scrubbing_) {
        return;
    }
    scrubToPixel(pixelX);
}

void TimelineRuler::endScrub(const int pixelX) {
    if (!scrubbing_) {
        return;
    }
    scrubbing_ = false;
    scrubToPixel(pixelX);
    previewController_.notifyScrubEnded();
}

void TimelineRuler::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    beginScrub(static_cast<int>(event->position().x()));
}

void TimelineRuler::mouseMoveEvent(QMouseEvent* event) {
    if (!scrubbing_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    updateScrub(static_cast<int>(event->position().x()));
}

void TimelineRuler::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !scrubbing_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    endScrub(static_cast<int>(event->position().x()));
}

void TimelineRuler::scrubToPixel(const int pixelX) {
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    const auto axis = axisForWidth(width());
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

TimelineWorkAreaRow::TimelineWorkAreaRow(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("timelineWorkAreaRow");
    setAccessibleName(tr("Work area and playhead"));
    setFixedHeight(kit::px(kit::Size::Control));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // Keep the strip above the marker so neither covers the other's ink in the compact header.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    strip_ = new TimelineWorkAreaStrip(session_, this);
    layout->addWidget(strip_);
    layout->addStretch(1);

    connect(&session_, &CompositionSession::currentTimeChanged, this,
            qOverload<>(&TimelineWorkAreaRow::update));
    connect(&session_, &CompositionSession::compositionChanged, this,
            qOverload<>(&TimelineWorkAreaRow::update));
    connect(&session_, &CompositionSession::snapshotChanged, this,
            qOverload<>(&TimelineWorkAreaRow::update));
}

void TimelineWorkAreaRow::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), kit::color(kit::Color::Surface));
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

void TimelineKeyframePanel::setRuler(TimelineRuler& ruler) {
    ruler_ = &ruler;
    for (auto* row : rows_) {
        row->setRuler(ruler);
    }
}

void TimelineKeyframePanel::keyPressEvent(QKeyEvent* event) {
    if (gridMode_) {
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
    }
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

void TimelineKeyframePanel::setGridEntries(const std::vector<TimelineLayerEntry>& entries,
                                           int scrollOffset) {
    gridMode_ = true;
    gridRows_.clear();
    gridParameters_.clear();
    gridScroll_ = scrollOffset;
    std::vector<document::AnimationCurveId> curves;
    std::vector<int> indices;
    const auto* composition = session_.composition();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].rowKind != TimelineLayerEntry::Kind::Parameter)
            continue;
        document::AnimationCurveId curveId{};
        const auto* parameter =
            composition ? composition->parameters().find(entries[i].parameterId) : nullptr;
        if (parameter)
            if (const auto* source =
                    std::get_if<document::AnimationCurveSource>(&parameter->source))
                curveId = source->curveId;
        curves.push_back(curveId);
        indices.push_back(static_cast<int>(i));
        gridParameters_.push_back(entries[i].parameterId);
    }
    gridRows_ = indices;
    if (curves != lastCurveIds_) {
        delete rowsLayout_;
        rowsLayout_ = nullptr;
        for (auto* row : rows_) {
            row->hide();
            row->setParent(nullptr);
            row->deleteLater();
        }
        rows_.clear();
        lastCurveIds_ = curves;
        for (auto curve : curves) {
            auto* row = new TimelineKeyframeRow(session_, {}, curve, this);
            row->setObjectName("timelineKeyframeRow");
            row->installEventFilter(this);
            if (ruler_)
                row->setRuler(*ruler_);
            rows_.push_back(row);
        }
    }
    QRegion mask;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const QRect geometry(0, indices[i] * kTimelineRowHeight - scrollOffset, width(),
                             kTimelineRowHeight);
        rows_[i]->setGeometry(geometry);
        rows_[i]->show();
        mask += geometry;
    }
    // An empty Qt mask means unmasked; hide instead so collapsed layer bars retain input.
    setMask(mask);
    if (parentWidget()) {
        parentWidget()->setMask(mask);
        parentWidget()->setVisible(!rows_.empty());
    }
    setVisible(!rows_.empty());
}

void TimelineKeyframePanel::rebuild() {
    if (gridMode_)
        return;
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
        auto* row = new TimelineKeyframeRow(session_, spec.label, spec.curveId, this);
        if (ruler_ != nullptr) {
            row->setRuler(*ruler_);
        }
        rowsLayout_->addWidget(row);
        rows_.push_back(row);
    }

    setVisible(!specs.empty());
}

} // namespace bloom::ui
