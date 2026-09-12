#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>

#include <bloom/ui/kit/tokens.hpp>

#include <QRectF>
#include <QWidget>

#include <cstdint>
#include <optional>
#include <vector>

class QKeyEvent;
class QPainter;
class QVBoxLayout;

namespace bloom::document {
class Composition;
} // namespace bloom::document

namespace bloom::ui {

class CompositionPreviewController;
class CompositionSession;

// The ONE pixel <-> frame/time mapping every time-axis surface in the timeline panel reads (task
// T1). It was private to timeline_ruler.cpp while the ruler was the only widget with a time axis;
// the AE-style lane region now paints clip bars and the playhead on exactly the same axis, and the
// work-area header row paints the playhead's head marker on it, so one shared definition is the
// only way those three surfaces cannot drift apart by a pixel. Both directions' arithmetic moved
// verbatim.
//
// Pixel coordinates are UI-space integers, not RationalTime values, so the reverse pixel ->
// frame-index direction (used only for scrubbing) is a deliberately exact integer mapping using the
// same tie-to-greater rule as the time-domain contract (docs/architecture/animation-and-time.md,
// "Session Time And Scrubbing"). The forward frame/time -> pixel direction (used only for painting)
// is ordinary presentational arithmetic, not a clamp/tie/mapping decision.
//
// `widthPixels` is always the width of a LANE-REGION-ALIGNED widget: the ruler, a lane region, or
// the work-area row, all of which share one left edge. That is why frame 0 lands at the lane
// region's left edge and never underneath the layer-stack column.
struct TimelineAxis final {
    document::FrameRate frameRate;
    core::RationalTime duration;
    int widthPixels = 0;
    std::uint64_t maxIndex = 0;

    [[nodiscard]] static std::optional<TimelineAxis>
    create(const document::Composition& composition, int widthPixels);

    // Exact pixel -> frame index, clamped into the widget bounds, with an exact halfway pixel tie
    // going to the greater index (checked integer arithmetic). Falls back to a defensively clamped
    // floating approximation only if the exact product would overflow std::uint64_t -- unreachable
    // for any realistic composition duration/frame rate combined with a practical widget width, but
    // kept safe rather than UB, matching FrameTimeMapping's own defensive-clamp precedent.
    [[nodiscard]] std::uint64_t frameIndexForPixel(int pixelX) const noexcept;

    // Presentational time -> pixel (not a contract decision): clamps into [0, duration] so a key or
    // playhead fractionally outside the composition's range still paints at a visible edge.
    [[nodiscard]] qreal pixelForTime(core::RationalTime time) const noexcept;
};

// The timeline panel's shared row pitch (task T1): the AE-style layer rows, their lanes, and the
// keyframe lanes underneath them all step by exactly this much, so the left column's rows, the
// clip bars beside them, and the key rows below read as one grid.
//
// KIT GAP, disclosed rather than worked around: the design mock specifies 32, and the kit's own
// Size::TimelineRow is 34. Kit edits are outside this task's fence, so this resolves the required
// 32 from the closest existing token that IS exactly 32 (Size::ControlRoomy) instead of spelling a
// raw pixel literal, which tokens.hpp forbids outright. The honest fix is a 32px timeline-row token
// (or Size::TimelineRow becoming 32) in whoever next owns the kit.
inline constexpr int kTimelineRowHeight = kit::px(kit::Size::ControlRoomy);

// The playhead stroke every time-axis surface shares (task T1): a 1px Accent vertical line through
// the ruler and all lanes, with exactly ONE small head marker at its top, painted in the work-area
// header row directly above the ruler. Named here rather than re-spelled per widget so the line in
// the ruler, the line down the lanes, and the marker above them can never disagree.
inline constexpr qreal kPlayheadLineWidth = 1.0;
inline constexpr qreal kPlayheadMarkerHalfWidth = 5.0;
inline constexpr qreal kPlayheadMarkerHeight = 6.0;

// Paints the 1px Accent playhead line for `time` down the full `heightPixels` of the caller's
// widget, snapped so the aliased stroke lands on exactly one whole pixel column. Shared by the
// ruler and the lane region so the line reads as one continuous stroke through the whole panel.
void paintPlayheadLine(QPainter& painter, const TimelineAxis& axis, core::RationalTime time,
                       qreal heightPixels);

// The scrub ruler above the timeline's lane region (docs/architecture/animation-and-time.md,
// "Session Time And Scrubbing"): paints frame ticks and the session's exact-time playhead, and
// turns click/drag into session.setCurrentTime() calls that request the preview at Interactive
// priority through the controller's trailing cadence
// (CompositionPreviewController::beginInteractiveScrub()/ notifyScrubEnded()). Projection and scrub
// only: no direct Viewer manipulation, no playback transport, no key-editing gestures.
//
// Task T1: the ruler is laid out as the RIGHT part of the timeline's column-header row, so its x
// origin is the lane region's left edge and its frame-0 label never paints over the layer-stack
// column. Its own tick density/labelling rules are unchanged -- only the origin and extent moved.
//
// Kinetik restyle (task U7, issue #122, decision 3): labeled MAJOR ticks are density-adaptive --
// the step between them is chosen from the ruler's own width and the widest label this axis could
// ever paint (its own FONT metrics, not a guessed pixel budget) so adjacent major labels can never
// collide; unlabeled MINOR ticks fill in at a denser, purely visual grid. The pixel<->frame axis
// math itself (TimelineAxis above) is completely unchanged.
class TimelineRuler final : public QWidget {
    Q_OBJECT

  public:
    TimelineRuler(CompositionSession& session, CompositionPreviewController& previewController,
                  QWidget* parent = nullptr);

    // The ONE scrub path (task T1). The ruler's own mouse handlers call these, and so does the lane
    // region, which shares this widget's exact x axis by construction (both are the expanding
    // member of a row whose leading fixed-width layer column and trailing scrollbar gutter are
    // identical), so a drag across a lane and a drag across the ruler are literally the same code
    // -- no second scrub implementation, no second arming of the interactive preview cadence.
    void beginScrub(int pixelX);
    void updateScrub(int pixelX);
    void endScrub(int pixelX);

    // Exposed purely for tests (mirrors ViewerEditor::zoomDropdownForTest()'s precedent): the exact
    // major-tick label rects paintEvent would draw at the ruler's CURRENT width/composition, so a
    // collision test can assert disjointness without re-deriving the density math or rasterizing a
    // QImage to find text.
    [[nodiscard]] std::vector<QRectF> majorTickLabelRectsForTest() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    void scrubToPixel(int pixelX);

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    bool scrubbing_ = false;
};

// The honest "work area" strip (task U7, issue #122, decision 3): a thin Accent-dim band spanning
// the FULL [0, duration) composition range. Bloom has no range-editing feature yet -- there is no
// separate in/out point to visualize -- so this band always spans the entire width by construction;
// it is deliberately non-interactive (no mouse handling at all) rather than pretend a click could
// narrow it.
class TimelineWorkAreaStrip final : public QWidget {
    Q_OBJECT

  public:
    explicit TimelineWorkAreaStrip(CompositionSession& session, QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    CompositionSession& session_;
};

// The RIGHT part of the timeline's header row (task T1): nothing but the work-area strip, centered
// in the header row's own height, plus the playhead's single head marker at the top of the playhead
// stroke that continues down through the ruler and every lane.
//
// The marker lives here rather than inside TimelineWorkAreaStrip because the strip's one honest
// claim is that its dim band spans the WHOLE composition range; a solid Accent triangle painted
// inside that band would contradict exactly what timeline_ruler_tests.cpp's
// testWorkAreaStripSpansFullWidthWithDimAccentBand samples it for.
class TimelineWorkAreaRow final : public QWidget {
    Q_OBJECT

  public:
    explicit TimelineWorkAreaRow(CompositionSession& session, QWidget* parent = nullptr);

    [[nodiscard]] TimelineWorkAreaStrip* strip() const noexcept { return strip_; }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    CompositionSession& session_;
    TimelineWorkAreaStrip* strip_ = nullptr;
};

// One row per animated parameter of the current selection's layer -- every animatable Layer Output
// parameter, enumerated from the boundary node's own bindings rather than from a list kept here, so
// the panel cannot carry a narrower idea of what is animatable than the schema does
// (docs/architecture/animation-and-time.md, "Durable Type Model"): a name label plus a key lane
// painting each key at its exact time. Clicking a key selects it; dragging past
// QApplication::startDragDistance() moves it (a presentation-only ghost until release); Delete/
// Backspace on the panel deletes the selected key.
//
// Selection-model finding (issue #84): bloom::ui::CompositionSelection now carries a
// bloom::ui::KeyframeSelection alternative (curveId + KeyframeId) as its primary selection, so this
// panel no longer keeps a local selectedKeyframe_ -- it reads/writes CompositionSession::
// selection()/selectKeyframe()/deleteSelectedKeyframe()/moveSelectedKeyframe(), the one
// primary/contextual selection truth every other editor already shares (docs/roadmap.md's Batch-4
// gate). Pruning a vanished key is likewise centralized: CompositionSession::normalizeSelection()
// (run after every session-mediated execute/undo/redo) already invalidates any selection kind that
// no longer resolves against the current snapshot, so this panel needs no local prune pass.
class TimelineKeyframePanel final : public QWidget {
    Q_OBJECT

  public:
    explicit TimelineKeyframePanel(CompositionSession& session, QWidget* parent = nullptr);

  protected:
    void keyPressEvent(QKeyEvent* event) override;

  private:
    void rebuild();

    CompositionSession& session_;
    QVBoxLayout* rowsLayout_ = nullptr;
    std::vector<class TimelineKeyframeRow*> rows_;
    // Memoizes which curves currently have a row, so rebuild() only tears down/recreates widgets
    // when the row SET actually changes (a different contextual layer, or a curve appearing/
    // disappearing) rather than on every selectionChanged/snapshotChanged -- notably including the
    // one a row's OWN click/drag emits via CompositionSession::selectKeyframe(). Without this, a
    // key click would destroy the very row handling the mouse event that triggered it.
    std::vector<document::AnimationCurveId> lastCurveIds_;
};

} // namespace bloom::ui
