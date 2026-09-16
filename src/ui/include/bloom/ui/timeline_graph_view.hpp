#pragma once

// The graph editor: the timeline's curve view. It replaces the key lanes inside the lane region
// rather than living beside them, because a curve and a lane are two views of the same keys and
// showing both at once would only make the artist ask which one they are editing.
//
// Everything numeric lives in timeline_graph_math.hpp; this widget owns the ink, the viewport
// state, and the pointer. Keys are ALWAYS read through a component-aware address -- never through
// the legacy whole-value projection -- so a vector or colour parameter appears here as the several
// independent curves it actually is.

#include <bloom/document/animation.hpp>
#include <bloom/document/ids.hpp>

#include <QPointF>
#include <QWidget>

#include <compare>
#include <map>
#include <optional>
#include <vector>

namespace bloom::runtime {
struct CompiledScalarCurve;
}

namespace bloom::ui {

class CompositionSession;
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
    // Auto-fit every curve to its own keys and control points.
    void fitCurves();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    [[nodiscard]] double bandTop() const noexcept;
    [[nodiscard]] double bandHeight() const noexcept;
    [[nodiscard]] std::vector<document::ScalarKeyframe> keysFor(const GraphCurveId& curve) const;
    [[nodiscard]] const runtime::CompiledScalarCurve* compiledFor(const GraphCurveId& curve) const;
    void refreshCurves();
    void ensureViewport(const GraphCurveId& curve);
    void zoomValues(double factor, double anchorPixelY);

    CompositionSession& session_;
    TimelineRuler& ruler_;
    std::vector<GraphCurveId> curves_;
    std::vector<document::ParameterId> parameters_;
    std::map<GraphCurveId, GraphValueViewport> viewports_;
    // The compiled curves the polyline samples, rebuilt whenever the document revision moves so
    // the painted line is always the CURRENT curve and never a stale table.
    mutable std::map<GraphCurveId, runtime::CompiledScalarCurve> compiled_;
    mutable std::uint64_t compiledRevision_ = 0;
    mutable bool compiledValid_ = false;
    std::optional<QPointF> panOrigin_;
};

} // namespace bloom::ui
