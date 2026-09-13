#pragma once
#include <bloom/document/document.hpp>
#include <bloom/ui/composition_session.hpp>

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
struct TimelineLayerEntry;

// Shared presentation mapping. The half-open visible range is in seconds and belongs to the
// editor viewport, never the document. Scrub results always land on exact rational frame times.
struct TimelineAxis final {
    document::FrameRate frameRate;
    core::RationalTime duration;
    int widthPixels = 0;
    std::uint64_t maxIndex = 0;
    double t0 = 0.0;
    double t1 = 0.0;

    [[nodiscard]] static std::optional<TimelineAxis>
    create(const document::Composition& composition, int widthPixels);
    void zoomToRange(double start, double end) noexcept;
    void zoomToFit() noexcept;
    [[nodiscard]] double secondsForPixel(qreal pixelX) const noexcept;
    [[nodiscard]] qreal pixelForSeconds(double seconds) const noexcept;
    [[nodiscard]] double pixelsPerFrame() const noexcept;
    [[nodiscard]] std::uint64_t frameIndexForPixel(int pixelX) const noexcept;
    // Out-of-view times remain outside the widget, so clipping never pins an invisible key or
    // playhead to an edge of the viewport.
    [[nodiscard]] qreal pixelForTime(core::RationalTime time) const noexcept;
};

// Non-drop labels use the nearest nominal integer frame rate for HH:MM:SS:FF.
[[nodiscard]] QString formatTimelineFrameLabel(std::uint64_t frame, document::FrameRate rate,
                                               bool timecode);

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
// The ruler lives in the RIGHT cell of the EditorArea header, sharing the lane origin and
// scrollbar gutter. It owns the editor-local viewport range used by every time-axis surface.
// Frame and non-drop timecode labels use font metrics and density-adaptive spacing.
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
    [[nodiscard]] std::optional<TimelineAxis> axisForWidth(int widthPixels) const;
    void zoomToRange(double start, double end);
    void zoomToFit();
    void zoomBy(double factor, qreal anchorX);
    void setTimecodeLabels(bool timecode);
    [[nodiscard]] bool handleWheel(QWheelEvent* event);

  signals:
    void axisChanged();

  public:
    // Exposed purely for tests (mirrors ViewerEditor::zoomDropdownForTest()'s precedent): the exact
    // major-tick label rects paintEvent would draw at the ruler's CURRENT width/composition, so a
    // collision test can assert disjointness without re-deriving the density math or rasterizing a
    // QImage to find text.
    [[nodiscard]] std::vector<QRectF> majorTickLabelRectsForTest() const;
    [[nodiscard]] std::vector<QRectF> cachedFrameRects() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    void scrubToPixel(int pixelX);
    void wheelEvent(QWheelEvent* event) override;
    double visibleStart_ = 0.0;
    double visibleEnd_ = 0.0;

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    bool scrubbing_ = false;
    bool timecodeLabels_ = false;
};

// Full-duration overview with a draggable window and independently resizable edges.
class TimelineNavigator final : public QWidget {
  public:
    explicit TimelineNavigator(TimelineRuler& ruler, QWidget* parent = nullptr);
    [[nodiscard]] QRectF windowRect() const;

  protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    enum class Drag { None, Pan, Start, End };
    TimelineRuler& ruler_;
    Drag drag_ = Drag::None;
    qreal pressX_ = 0.0;
    double start_ = 0.0;
    double end_ = 0.0;
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
    void setRuler(TimelineRuler& ruler);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    [[nodiscard]] std::optional<TimelineAxis> axis() const;
    CompositionSession& session_;
    TimelineRuler* ruler_ = nullptr;
    std::optional<document::WorkArea> preview_{};
    document::Revision dragRevision_{};
    bool startHandle_ = false;
};

// The top of the header's right cell: a work-area strip with the single head marker immediately
// below it. The shared stroke continues through the ruler labels and every lane.
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
    void setRuler(TimelineRuler& ruler);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    CompositionSession& session_;
    TimelineWorkAreaStrip* strip_ = nullptr;
    TimelineRuler* ruler_ = nullptr;
    bool event(QEvent* event) override;
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
    void setGridEntries(const std::vector<TimelineLayerEntry>& entries, int scrollOffset);
    void paintGridOverlay(QPainter& painter, const QWidget& row) const;
    [[nodiscard]] bool gridMode() const noexcept { return gridMode_; }
    void setRuler(TimelineRuler& ruler);

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

  private:
    void rebuild();
    bool gridMode_ = false;
    struct LaneKey {
        KeyframeSelection selection;
        core::RationalTime time;
        document::KeyframeInterpolation interpolation;
        int row;
    };
    [[nodiscard]] std::vector<LaneKey> laneKeys() const;
    [[nodiscard]] std::optional<LaneKey> hitKey(QPointF position) const;
    void cancelGesture();
    void updateRows();
    std::vector<int> gridRows_;
    std::vector<document::ParameterId> gridParameters_;
    int gridScroll_ = 0;
    QPointF press_;
    std::optional<LaneKey> pressed_;
    bool dragging_ = false, boxing_ = false, copying_ = false;
    std::optional<core::RationalTime> stretchAnchor_;
    document::Revision gestureRevision_{};
    std::vector<KeyframeSelection> gestureKeys_;
    std::vector<commands::KeyframePaste> gestureData_;
    std::vector<commands::KeyframeMove> moves_;
    std::optional<QRectF> box_;
    std::optional<core::RationalTime> snapGuide_;

    CompositionSession& session_;
    QVBoxLayout* rowsLayout_ = nullptr;
    TimelineRuler* ruler_ = nullptr;
    std::vector<class TimelineKeyframeRow*> rows_;
    // Memoizes which curves currently have a row, so rebuild() only tears down/recreates widgets
    // when the row SET actually changes (a different contextual layer, or a curve appearing/
    // disappearing) rather than on every selectionChanged/snapshotChanged -- notably including the
    // one a row's OWN click/drag emits via CompositionSession::selectKeyframe(). Without this, a
    // key click would destroy the very row handling the mouse event that triggered it.
    std::vector<document::AnimationCurveId> lastCurveIds_;
};

} // namespace bloom::ui
