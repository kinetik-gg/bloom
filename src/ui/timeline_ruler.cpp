#include <bloom/ui/timeline_ruler.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_frame_math.hpp>

#include <bloom/document/animation.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
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
// signals are needed; key selection is reported through a plain callback), keeping it free of moc.
class TimelineKeyframeRow final : public QWidget {
  public:
    TimelineKeyframeRow(CompositionSession& session, QString label,
                        const document::AnimationCurveId curveId, const bool isVec2,
                        const std::optional<document::KeyframeId>* selectedKeyframe,
                        std::function<void(document::KeyframeId)> onKeySelected, QWidget* parent)
        : QWidget(parent), session_(session), label_(std::move(label)), curveId_(curveId),
          isVec2_(isVec2), selectedKeyframe_(selectedKeyframe),
          onKeySelected_(std::move(onKeySelected)) {
        setFixedHeight(kKeyframeRowHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(&session_, &CompositionSession::currentTimeChanged, this, [this] { update(); });
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

        const qreal centerY = height() / 2.0;
        for (const auto& key : collectKeys()) {
            const qreal x = axis->pixelForTime(key.time);
            const bool selected = selectedKeyframe_ != nullptr && selectedKeyframe_->has_value() &&
                                  **selectedKeyframe_ == key.id;
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
        std::optional<document::KeyframeId> closest;
        qreal closestDistance = std::numeric_limits<qreal>::max();
        for (const auto& key : collectKeys()) {
            const qreal distance = std::abs(axis->pixelForTime(key.time) - clickX);
            if (distance <= kKeyHitToleranceLogicalPixels && distance < closestDistance) {
                closest = key.id;
                closestDistance = distance;
            }
        }
        if (closest.has_value() && onKeySelected_) {
            onKeySelected_(*closest);
        }
    }

  private:
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
    const std::optional<document::KeyframeId>* selectedKeyframe_;
    std::function<void(document::KeyframeId)> onKeySelected_;
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

void TimelineKeyframePanel::rebuild() {
    QLayoutItem* item = nullptr;
    while ((item = rowsLayout_->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    rows_.clear();

    const auto specs = collectAnimatedParameters(session_);
    rows_.reserve(specs.size());
    for (const auto& spec : specs) {
        auto* row = new TimelineKeyframeRow(
            session_, spec.label, spec.curveId, spec.isVec2, &selectedKeyframe_,
            [this](const document::KeyframeId id) { handleKeySelected(id); }, this);
        rowsLayout_->addWidget(row);
        rows_.push_back(row);
    }

    pruneSelectionIfMissing();
    setVisible(!specs.empty());
}

void TimelineKeyframePanel::handleKeySelected(const document::KeyframeId id) {
    if (selectedKeyframe_.has_value() && *selectedKeyframe_ == id) {
        return;
    }
    selectedKeyframe_ = id;
    for (auto* row : rows_) {
        row->update();
    }
}

void TimelineKeyframePanel::pruneSelectionIfMissing() {
    if (!selectedKeyframe_.has_value()) {
        return;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        selectedKeyframe_.reset();
        return;
    }
    for (const auto& record : composition->animationCurves().records()) {
        if (const auto* scalar = std::get_if<document::ScalarAnimationCurve>(&record)) {
            for (const auto& key : scalar->keyframes) {
                if (key.id == *selectedKeyframe_) {
                    return;
                }
            }
        } else if (const auto* vec2 = std::get_if<document::Vec2AnimationCurve>(&record)) {
            for (const auto& key : vec2->keyframes) {
                if (key.id == *selectedKeyframe_) {
                    return;
                }
            }
        }
    }
    selectedKeyframe_.reset();
}

} // namespace bloom::ui
