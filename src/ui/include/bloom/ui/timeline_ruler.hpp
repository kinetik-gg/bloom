#pragma once

#include <bloom/document/ids.hpp>

#include <QRectF>
#include <QWidget>

#include <vector>

class QKeyEvent;
class QVBoxLayout;

namespace bloom::ui {

class CompositionPreviewController;
class CompositionSession;

// The scrub ruler above TimelineEditor's layer tree (docs/architecture/animation-and-time.md,
// "Session Time And Scrubbing"): paints frame ticks and the session's exact-time playhead, and
// turns click/drag into session.setCurrentTime() calls that request the preview at Interactive
// priority through the controller's trailing cadence (CompositionPreviewController::
// beginInteractiveScrub()/notifyScrubEnded()). Projection and scrub only: no direct Viewer
// manipulation, no playback transport, no key-editing gestures.
//
// Kinetik restyle (task U7, issue #122, decision 3): labeled MAJOR ticks are density-adaptive --
// the step between them is chosen from the ruler's own width and the widest label this axis could
// ever paint (its own FONT metrics, not a guessed pixel budget) so adjacent major labels can never
// collide; unlabeled MINOR ticks fill in at a denser, purely visual grid. The pixel<->frame axis
// math itself (TimelineAxis, private to the .cpp) is completely unchanged -- this only changes
// which ticks get a text label and how far apart they are.
class TimelineRuler final : public QWidget {
    Q_OBJECT

  public:
    TimelineRuler(CompositionSession& session, CompositionPreviewController& previewController,
                  QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    void scrubToPixel(int pixelX);

  public:
    // Exposed purely for tests (mirrors ViewerEditor::zoomDropdownForTest()'s precedent): the
    // exact major-tick label rects paintEvent would draw at the ruler's CURRENT width/composition,
    // so a collision test can assert disjointness without re-deriving the density math or
    // rasterizing a QImage to find text.
    [[nodiscard]] std::vector<QRectF> majorTickLabelRectsForTest() const;

  private:
    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    bool scrubbing_ = false;
};

// The honest "work area" strip (task U7, issue #122, decision 3): a thin Accent-dim band spanning
// the FULL [0, duration) composition range, painted directly above TimelineRuler in
// TimelineEditor's layout. Bloom has no range-editing feature yet -- there is no separate in/out
// point to visualize
// -- so this band always spans the entire width by construction; it is deliberately
// non-interactive (no mouse handling at all) rather than pretend a click could narrow it.
class TimelineWorkAreaStrip final : public QWidget {
    Q_OBJECT

  public:
    explicit TimelineWorkAreaStrip(CompositionSession& session, QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    CompositionSession& session_;
};

// One row per animated parameter of the current selection's layer (position/opacity only in
// version 1 -- docs/architecture/animation-and-time.md, "Durable Type Model"): a name label plus a
// key lane painting each key at its exact time. Clicking a key selects it; dragging past
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
