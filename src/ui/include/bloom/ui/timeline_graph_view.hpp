#pragma once

// The graph editor: the timeline's curve view. It replaces the key lanes inside the lane region
// rather than living beside them, because a curve and a lane are two views of the same keys and
// showing both at once would only make the artist ask which one they are editing.
//
// Everything numeric lives in timeline_graph_math.hpp; this widget owns the ink, the viewport
// state, and the pointer. Keys are ALWAYS read through a component-aware address -- never through
// the legacy whole-value projection -- so a vector or colour parameter appears here as the several
// independent curves it actually is.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <compare>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace bloom::runtime {
struct CompiledScalarCurve;
}

namespace bloom::ui {

class TimelineRuler;
struct GraphValueViewport;
struct TimelineLayerEntry;

// One drawable curve: a scalar parameter's curve, or ONE component of a vector or colour one.
struct GraphCurveId final {
    document::AnimationCurveId curveId;
    std::optional<document::AnimationComponent> component;

    friend auto operator<=>(const GraphCurveId&, const GraphCurveId&) = default;
    friend bool operator==(const GraphCurveId&, const GraphCurveId&) = default;
};

// What the pointer is over. The order is the priority order: a selected key's ease handle wins
// over the key itself, which wins over the curve it sits on, and the ACTIVE curve wins a tie --
// otherwise a handle would become unreachable the moment another curve crossed it.
struct GraphHit final {
    enum class Kind : std::uint8_t { Handle, Key, Segment };

    Kind kind = Kind::Key;
    GraphCurveId curve;
    document::KeyframeId keyframeId;
    // For Kind::Handle: which side of the key was grabbed.
    bool outgoing = false;
    // For Kind::Segment: the exact time under the pointer.
    core::RationalTime time;
};

class TimelineGraphView final : public QWidget {
    Q_OBJECT

  public:
    TimelineGraphView(CompositionSession& session, TimelineRuler& ruler, QWidget* parent = nullptr);
    ~TimelineGraphView() override;

    // The rows the timeline is showing. The curve set is derived from the Parameter rows among
    // them, so the graph shows exactly what the twirl-downs expose and nothing else.
    void setEntries(const std::vector<TimelineLayerEntry>& entries);

    [[nodiscard]] const std::vector<GraphCurveId>& curves() const noexcept { return curves_; }
    // The curve whose value ticks the gutter shows: the primary keyframe selection's, else the
    // one belonging to the row the selection is on, else the first.
    [[nodiscard]] std::optional<GraphCurveId> activeCurve() const;
    [[nodiscard]] GraphValueViewport viewportFor(const GraphCurveId& curve) const;
    // The polyline this view would paint for `curve`: one point per pixel column, in widget
    // coordinates. Exposed so a test can map a painted y back through the sampler at the same
    // time and demand the same number, rather than probing pixels.
    [[nodiscard]] std::vector<QPointF> polylineForTest(const GraphCurveId& curve) const;
    [[nodiscard]] int gutterWidth() const noexcept;
    // The vertical band values map into: y of the viewport's maximum, and its extent. Exposed so
    // a test can recompute a painted y from the sampler itself rather than probing pixels.
    [[nodiscard]] std::pair<double, double> valueBandForTest() const {
        return {bandTop(), bandHeight()};
    }
    // Auto-fit every curve to its own keys and control points.
    void fitCurves();
    // Lane snapping's own rule, on the graph's own canvas: `snapping && !Shift`.
    void setSnappingEnabled(bool enabled) noexcept { snapping_ = enabled; }
    [[nodiscard]] bool snappingEnabled() const noexcept { return snapping_; }
    // The hit test, exposed so a test can assert the priority order without synthesising pixels.
    [[nodiscard]] std::optional<GraphHit> hitTestForTest(QPointF position) const {
        return hitTest(position);
    }
    // The pixel a key currently occupies, for a test that wants to press on one.
    [[nodiscard]] std::optional<QPointF> keyCenter(const GraphCurveId& curve,
                                                   document::KeyframeId keyframeId) const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    enum class Gesture : std::uint8_t { None, Box, Keys, Handle };
    [[nodiscard]] double bandTop() const noexcept;
    [[nodiscard]] double bandHeight() const noexcept;
    [[nodiscard]] std::vector<document::ScalarKeyframe> keysFor(const GraphCurveId& curve) const;
    [[nodiscard]] const runtime::CompiledScalarCurve* compiledFor(const GraphCurveId& curve) const;
    void refreshCurves();
    void ensureViewport(const GraphCurveId& curve);
    void zoomValues(double factor, double anchorPixelY);
    [[nodiscard]] std::optional<GraphHit> hitTest(QPointF position) const;
    [[nodiscard]] std::vector<GraphCurveId> curvesByPriority() const;
    [[nodiscard]] std::optional<GraphCurveId> curveOf(const KeyframeSelection& key) const;
    [[nodiscard]] double valueAt(const GraphCurveId& curve, core::RationalTime time) const;
    void updateKeyDrag(const QMouseEvent& event);
    void updateHandleDrag(const QMouseEvent& event);
    void commitGesture();
    void cancelGesture();

    CompositionSession& session_;
    TimelineRuler& ruler_;
    std::vector<GraphCurveId> curves_;
    std::vector<document::ParameterId> parameters_;
    // The parameter each drawn curve belongs to, resolved by the same walk that derived the curve
    // set, so an insert gesture never has to search the store a second time for a different answer.
    std::map<document::AnimationCurveId, document::ParameterId> owners_;
    std::map<GraphCurveId, GraphValueViewport> viewports_;
    // The compiled curves the polyline samples, rebuilt whenever the document revision moves so
    // the painted line is always the CURRENT curve and never a stale table.
    mutable std::map<GraphCurveId, runtime::CompiledScalarCurve> compiled_;
    mutable std::uint64_t compiledRevision_ = 0;
    mutable bool compiledValid_ = false;
    std::optional<QPointF> panOrigin_;
    bool snapping_ = true;

    // Gesture state. `pending*` are the drag's PREVIEW: nothing reaches the document until
    // release, and the ghosts a drag paints are these same numbers, so what the artist sees and
    // what the transaction carries cannot differ.
    Gesture gesture_ = Gesture::None;
    bool dragging_ = false;
    bool copying_ = false;
    QPointF press_;
    std::optional<GraphHit> pressed_;
    document::Revision gestureRevision_{};
    std::vector<KeyframeSelection> gestureKeys_;
    std::vector<commands::KeyframePaste> gestureData_;
    std::vector<commands::KeyframeMove> pendingMoves_;
    std::vector<commands::KeyframeValueEdit> pendingValues_;
    std::vector<commands::KeyframeHandleEdit> pendingHandles_;
    std::optional<QRectF> box_;
    std::optional<core::RationalTime> snapGuide_;
};

} // namespace bloom::ui
