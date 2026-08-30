#include <bloom/ui/timeline_ruler.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_frame_math.hpp>

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

constexpr int kRulerHeight = 26;
constexpr int kKeyframeRowHeight = 24;
constexpr qreal kKeyDiamondRadius = 4.5;
constexpr qreal kKeyHitToleranceLogicalPixels = 6.0;
constexpr qreal kMinimumPixelsPerTick = 28.0;

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

[[nodiscard]] std::uint64_t tickStepFrames(const TimelineAxis& axis) {
    if (axis.maxIndex == 0 || axis.widthPixels <= 1) {
        return 1;
    }
    const double pixelsPerFrame =
        static_cast<double>(axis.widthPixels - 1) / static_cast<double>(axis.maxIndex);
    if (pixelsPerFrame >= kMinimumPixelsPerTick) {
        return 1;
    }
    return static_cast<std::uint64_t>(std::ceil(kMinimumPixelsPerTick / pixelsPerFrame));
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
        painter.fillRect(rect(), palette().color(QPalette::AlternateBase));
        painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
        painter.drawLine(QPointF(0.0, height() - 0.5), QPointF(width(), height() - 0.5));

        const auto* composition = session_.composition();
        if (composition == nullptr) {
            return;
        }
        const auto axis = TimelineAxis::create(*composition, width());
        if (!axis.has_value()) {
            return;
        }

        const qreal playheadX = axis->pixelForTime(session_.currentTime());
        painter.setPen(QPen(palette().color(QPalette::Highlight).lighter(150), 1.0));
        painter.drawLine(QPointF(playheadX, 0.0), QPointF(playheadX, height()));

        const auto* keySelection = std::get_if<KeyframeSelection>(&session_.selection().primary);
        const qreal centerY = height() / 2.0;
        for (const auto& key : collectKeys()) {
            const qreal x = axis->pixelForTime(key.time);
            const bool selected = keySelection != nullptr && keySelection->curveId == curveId_ &&
                                  keySelection->keyframeId == key.id;
            const QColor fill = selected ? palette().color(QPalette::Highlight)
                                         : palette().color(QPalette::ButtonText);
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
            QColor ghostColor = palette().color(QPalette::Highlight);
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

        const QFontMetrics metrics = painter.fontMetrics();
        const QRectF chip(2.0, 2.0, metrics.horizontalAdvance(label_) + 10.0, height() - 4.0);
        QColor chipColor = palette().color(QPalette::Window);
        chipColor.setAlpha(212);
        painter.setPen(Qt::NoPen);
        painter.setBrush(chipColor);
        painter.drawRoundedRect(chip, 3.0, 3.0);
        painter.setPen(palette().color(QPalette::WindowText));
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
    painter.fillRect(rect(), palette().color(QPalette::Base));
    painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
    painter.drawLine(QPointF(0.0, height() - 0.5), QPointF(width(), height() - 0.5));

    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    const auto axis = TimelineAxis::create(*composition, width());
    if (!axis.has_value()) {
        return;
    }

    painter.setPen(palette().color(QPalette::WindowText));
    QFont tickFont = painter.font();
    tickFont.setPointSizeF(tickFont.pointSizeF() * 0.85);
    painter.setFont(tickFont);

    const auto step = tickStepFrames(*axis);
    for (std::uint64_t index = 0; index <= axis->maxIndex; index += step) {
        const auto time = frameTimeForIndex(axis->frameRate, axis->duration, index);
        if (!time.has_value()) {
            continue;
        }
        const qreal x = axis->pixelForTime(*time);
        painter.drawLine(QPointF(x, height() - 8.0), QPointF(x, height() - 1.0));
        painter.drawText(QRectF(x + 2.0, 0.0, 60.0, height() - 9.0),
                         Qt::AlignLeft | Qt::AlignVCenter, QString::number(index));
    }

    const qreal playheadX = axis->pixelForTime(session_.currentTime());
    const QColor playheadColor = palette().color(QPalette::Highlight);
    painter.setPen(QPen(playheadColor, 2.0));
    painter.drawLine(QPointF(playheadX, 0.0), QPointF(playheadX, height()));
    painter.setPen(Qt::NoPen);
    painter.setBrush(playheadColor);
    QPolygonF marker;
    marker << QPointF(playheadX - 5.0, 0.0) << QPointF(playheadX + 5.0, 0.0)
           << QPointF(playheadX, 6.0);
    painter.drawPolygon(marker);
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
