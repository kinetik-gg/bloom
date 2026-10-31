#pragma once

// The graph editor: the timeline's curve view. It replaces the key lanes inside the lane region
// rather than living beside them, because a curve and a lane are two views of the same keys and
// showing both at once would only make the artist ask which one they are editing.
//
// Everything numeric lives in timeline_graph_math.hpp; this widget owns the ink, the viewport
// state, and the pointer. Keys are ALWAYS read through a component-aware address, so a vector or
// colour parameter appears here as the several independent curves it actually is.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <array>
#include <compare>
#include <map>
#include <optional>
#include <utility>
#include <vector>

class QAction;

namespace bloom::runtime {
struct CompiledScalarCurve;
}

namespace bloom::ui {
namespace kit {
class KCheckBox;
class KRadioGroup;
} // namespace kit

class TimelineRuler;
struct GraphLens;
struct GraphNormalization;
struct GraphValueViewport;
struct TimelineLayerEntry;
enum class GraphType : std::uint8_t;

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
    // The curves drawn brighter: every curve carrying a selected key, else the selected row's.
    [[nodiscard]] std::vector<GraphCurveId> selectedCurves() const;
    // A curve's stroke: X/Y/Z red/green/blue, single-value curves the GraphCurve palette in curve
    // order, and a selected curve brought forward (kit::emphasizedStrokeFor). Tokens only.
    [[nodiscard]] QColor curveColor(const GraphCurveId& curve, bool selected) const;
    // The window `curve` is viewed through in the current graph type, in DISPLAY units (the
    // normalized -1...1 window when Normalize is on), and its whole vertical mapping.
    [[nodiscard]] GraphValueViewport viewportFor(const GraphCurveId& curve) const;
    [[nodiscard]] GraphLens lensFor(const GraphCurveId& curve) const;
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
    // Fit All: every curve to its own keys and control points, and the time axis to every key.
    void fitCurves();
    // Fit Selection: the selected keys to the view on both axes; Fit All when nothing is selected.
    void fitSelection();
    // Lane snapping's own rule, on the graph's own canvas: the Snap to Frames toggle.
    void setSnappingEnabled(bool enabled) noexcept { snapping_ = enabled; }
    [[nodiscard]] bool snappingEnabled() const noexcept { return snapping_; }
    // After Effects' graph types, switched in the graph header; persisted.
    void setGraphType(GraphType type);
    [[nodiscard]] GraphType graphType() const noexcept { return graphType_; }
    // Blender's Normalize, toggled in the graph header; persisted.
    void setNormalized(bool normalized);
    [[nodiscard]] bool normalized() const noexcept { return normalized_; }
    // The timeline's Easy Ease / In / Out actions (they carry F9, Shift+F9 and Ctrl+Shift+F9 for the
    // whole panel), offered again in the key context menu so the menu and the keys are one command.
    void setEaseActions(const std::array<QAction*, 3>& actions) { easeActions_ = actions; }
    [[nodiscard]] QWidget* headerForTest() const noexcept { return header_; }
    // The hit test, exposed so a test can assert the priority order without synthesising pixels.
    [[nodiscard]] std::optional<GraphHit> hitTestForTest(QPointF position) const {
        return hitTest(position);
    }
    // The pixel a key currently occupies, for a test that wants to press on one.
    [[nodiscard]] std::optional<QPointF> keyCenter(const GraphCurveId& curve,
                                                   document::KeyframeId keyframeId) const;
    // The pixel of a selected key's handle in the current graph type, for a test that drags one.
    [[nodiscard]] std::optional<QPointF> handleCenter(const GraphCurveId& curve,
                                                      document::KeyframeId keyframeId,
                                                      bool outgoing) const;
    // The plotted quantity of `curve` at `seconds` in the current graph type: its value, or its
    // speed in value units per second.
    [[nodiscard]] double plottedAt(const GraphCurveId& curve, double seconds,
                                   bool outgoing = true) const;
    // The context menu builder, exposed so a test can read the real entries without a popup.
    void populateContextMenu(QMenu& menu);

    // #358 work counters: playback repaints only the playhead strips, and a curve is resampled
    // only when its compiled table, the time axis, the size or its value viewport changes.
    struct Work final {
        std::uint64_t curveSamples = 0;
        std::uint64_t playheadInvalidations = 0;
        std::uint64_t fullInvalidations = 0;
    };
    [[nodiscard]] Work work() const noexcept { return work_; }

  protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    enum class Gesture : std::uint8_t { None, Box, Keys, Handle };
    void buildHeader();
    [[nodiscard]] double bandTop() const noexcept;
    [[nodiscard]] double bandHeight() const noexcept;
    // `preview` reads the live drag's previewed curve when one exists; gesture arithmetic reads the
    // curve as it was at the press.
    [[nodiscard]] std::vector<document::ScalarKeyframe> keysFor(const GraphCurveId& curve,
                                                                bool preview = true) const;
    [[nodiscard]] const runtime::CompiledScalarCurve* compiledFor(const GraphCurveId& curve) const;
    void refreshCurves();
    [[nodiscard]] std::vector<double> plottedExtent(const GraphCurveId& curve) const;
    [[nodiscard]] GraphNormalization normalizationOf(const GraphCurveId& curve) const;
    [[nodiscard]] GraphValueViewport fittedViewport(const GraphCurveId& curve) const;
    void setViewport(const GraphCurveId& curve, const GraphValueViewport& viewport);
    void zoomValues(double factor, double anchorPixelY);
    void panValues(double pixels);
    void frameTimes(double t0, double t1);
    [[nodiscard]] double minimumFitSeconds() const;
    [[nodiscard]] QPointF keyPoint(const GraphCurveId& curve,
                                   const std::vector<document::ScalarKeyframe>& keys,
                                   std::size_t index) const;
    // The handle's plotted point, or nothing when that side of the key has no eased segment.
    [[nodiscard]] std::optional<QPointF>
    handlePoint(const GraphCurveId& curve, const std::vector<document::ScalarKeyframe>& keys,
                std::size_t index, bool outgoing) const;
    [[nodiscard]] std::optional<GraphHit> hitTest(QPointF position) const;
    [[nodiscard]] std::vector<GraphCurveId> curvesByPriority() const;
    [[nodiscard]] std::optional<GraphCurveId> curveOf(const KeyframeSelection& key) const;
    void paintGrid(QPainter& painter, const std::optional<GraphCurveId>& active) const;
    void paintCurve(QPainter& painter, const GraphCurveId& curve, bool selected) const;
    void updateKeyDrag(const QMouseEvent& event);
    void updateSpeedKeyDrag(double dy);
    void updateHandleDrag(const QMouseEvent& event);
    void updateSpeedHandleDrag(const QMouseEvent& event);
    void previewGesture();
    void commitGesture();
    void cancelGesture();

    CompositionSession& session_;
    TimelineRuler& ruler_;
    QWidget* header_ = nullptr;
    kit::KRadioGroup* typeSwitch_ = nullptr;
    kit::KCheckBox* normalizeToggle_ = nullptr;
    std::array<QAction*, 3> easeActions_{};
    std::vector<GraphCurveId> curves_;
    std::vector<std::pair<document::ParameterId, std::optional<document::AnimationComponent>>>
        parameters_;
    // The parameter each drawn curve belongs to, resolved by the same walk that derived the curve
    // set, so an insert gesture never has to search the store a second time for a different answer.
    std::map<document::AnimationCurveId, document::ParameterId> owners_;
    GraphType graphType_{};
    bool normalized_ = false;
    // Real-unit windows, per curve AND graph type: a value in degrees and a speed in degrees per
    // second cannot share one. With Normalize on every curve shares one -1...1 window per type.
    std::map<std::pair<GraphCurveId, GraphType>, GraphValueViewport> viewports_;
    std::map<GraphType, GraphValueViewport> normalizedViewports_;
    // Normalization is frozen for the length of a drag, so the canvas does not rescale under the
    // pointer while the curve's own extent changes; it is recomputed on release.
    std::map<GraphCurveId, GraphNormalization> frozenNormalization_;
    // The compiled curves the polyline samples, rebuilt whenever the document revision or the live
    // preview moves so the painted line is always the CURRENT curve and never a stale table.
    mutable std::map<GraphCurveId, runtime::CompiledScalarCurve> compiled_;
    mutable std::uint64_t compiledRevision_ = 0;
    mutable std::uint64_t compiledPreview_ = 0;
    mutable bool compiledValid_ = false;
    // Bumped whenever compiled_ is rebuilt, so cached polylines can never outlive their tables.
    mutable std::uint64_t compiledGeneration_ = 0;
    struct CachedPath final {
        std::uint64_t generation = 0;
        double t0 = 0.0;
        double t1 = 0.0;
        int width = 0;
        double top = 0.0;
        double band = 0.0;
        double minimum = 0.0;
        double maximum = 0.0;
        double center = 0.0;
        double halfSpan = 1.0;
        GraphType type{};
        QPainterPath path;
    };
    mutable std::map<GraphCurveId, CachedPath> paths_;
    [[nodiscard]] const QPainterPath& pathFor(const GraphCurveId& curve,
                                              const runtime::CompiledScalarCurve& compiled) const;
    void invalidatePlayhead();
    void invalidateAll();
    std::optional<int> paintedPlayheadX_;
    mutable Work work_;
    std::optional<QPointF> panOrigin_;
    bool snapping_ = true;

    // Gesture state. `pending*` are the edit the drag would commit; every move previews exactly
    // them through CompositionSession::previewKeyframeTransaction(), so the canvas, the readouts
    // and the Viewer show the transaction the release then commits as ONE undo entry.
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
    // Each dragged key's speed on either side at the press, so a Speed Graph drag measures its
    // travel from the curve as it was rather than from its own preview.
    struct PressedSpeed final {
        KeyframeSelection key;
        bool outgoing = true;
        double speed = 0.0;
    };
    std::vector<PressedSpeed> pressedSpeeds_;
    [[nodiscard]] double pressedSpeed(const KeyframeSelection& key, bool outgoing) const;
    std::optional<QRectF> box_;
    std::optional<core::RationalTime> snapGuide_;
};

} // namespace bloom::ui
