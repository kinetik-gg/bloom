#pragma once

#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/playback_controller.hpp>

#include <bloom/document/ids.hpp>

#include <bloom/ui/kit/tokens.hpp>

#include <QMetaObject>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QWidget>

#include <optional>
#include <vector>

class QAction;
class QLabel;
class QScrollBar;
class QMenu;
class QToolButton;

namespace bloom::ui {

class CompositionPreviewController;
class RamPreviewController;
class CompositionSession;
class TimelineColumnHeaders;
class TimelineKeyframePanel;
class TimelineLaneRegion;
class TimelineLayerStack;
class TimelineRuler;
class TimelineWorkAreaRow;

// One row of the timeline's layer stack, resolved once per rebuild from the snapshot and then read
// by both halves of the row (task T1). The left layer-stack column and the right lane region are
// two widgets painting one logical row, so they read one immutable description of it rather than
// each walking the graph and risking a different answer about a layer's name, kind, or color.
struct TimelineLayerEntry final {
    document::LayerId layerId;
    document::LayerSlotId slotId;
    QString name;
    // The layer kind, kept as honest text even though task T1 removes the Kind COLUMN (kind is
    // expressed by the clip color now): it still reaches the artist through the row's tooltip, so
    // dropping the column never drops the information.
    QString kind;
    // The clip bar's fill, from the data-type palette -- the one thing that now carries kind.
    kit::Color clipColor = kit::Color::Muted;
    QColor labelColor;
};

// Layer stack and lanes share one vertical scroll. EditorArea hosts the split header's name,
// menus and ruler; column headings start the body, with transport and navigator below the lanes.
class TimelineEditor final : public QWidget,
                             public EditorHeaderMenuProvider,
                             public EditorHeaderSplitProvider {
    Q_OBJECT

  public:
    // `ramPreview` is the RAM Preview command (task PERF1, item 3), shared with the Composition
    // menu so both entry points call one method. Null leaves the transport's RAM Preview button and
    // shortcut present but disabled -- an affordance that is visibly unavailable rather than one
    // that silently does nothing.
    TimelineEditor(CompositionSession& session, CompositionPreviewController& previewController,
                   RamPreviewController* ramPreview = nullptr, QWidget* parent = nullptr);
    // Exists only to drop the application-wide focusChanged subscription BEFORE Qt starts deleting
    // this panel's children. QWidget's own destructor clears focus from each child as it goes, and
    // a child losing focus re-enters that subscription -- which reads sibling widgets that
    // deleteChildren may already have destroyed. Disconnecting here is the one place that ordering
    // can be fixed; QObject's automatic disconnection happens far too late, in ~QObject, after
    // every child is gone.
    ~TimelineEditor() override;
    [[nodiscard]] QWidget* takeHeaderMenuWidget() override;
    [[nodiscard]] QWidget* takeHeaderRightWidget() override;
    [[nodiscard]] int headerSplitPosition() const override { return layerColumnWidth(); }

    // The fixed width of the LEFT layer-stack column, and therefore the exact x origin of the
    // ruler, of every lane, and of the work-area strip above them. Exposed so a test can assert
    // that alignment against one number instead of re-deriving the cell table.
    [[nodiscard]] static int layerColumnWidth();

    // Test seams (mirroring TimelineRuler::majorTickLabelRectsForTest()'s precedent): the three
    // widgets whose geometry is this task's pinned contract.
    [[nodiscard]] TimelineLayerStack* layerStackForTest() const noexcept { return stack_; }
    [[nodiscard]] TimelineLaneRegion* laneRegionForTest() const noexcept { return lanes_; }
    [[nodiscard]] TimelineRuler* rulerForTest() const noexcept { return ruler_; }
    [[nodiscard]] QScrollBar* verticalScrollBarForTest() const noexcept { return scrollBar_; }
    [[nodiscard]] QToolButton* ramPreviewButtonForTest() const noexcept {
        return ramPreviewButton_;
    }

  private:
    void rebuild();
    void updateSelection();
    void updateHistoryActions();
    void createHeaderMenus();
    void refreshHeaderMenus();
    void selectAllLayers();
    void deleteSelectedLayers();
    void setTimecodeFormat(bool timecode);
    void showEvent(QShowEvent* event) override;
    void updateScrollRange();
    // Reflects PlaybackController::stateChanged() onto the toggle button's text/tooltip/checked
    // state (design decision 4: "button/icon state reflects transport state via a signal").
    void updatePlaybackButton(PlaybackState state);
    void updateRamPreviewButton();
    // Frame stepping (issue #108, decisions 1/2): Left/Right step one frame back/forward from
    // nearestFrameIndex(currentTime()), clamped to [0, maxFrameIndex]; delta is -1 or +1. Home/End
    // (stepToStart()/stepToEnd()) jump to frame 0 / the last frame. Every landing goes through the
    // exact mapped frame time via CompositionSession::setCurrentTime(), and pauses playback FIRST
    // through PlaybackController's own public pause() -- never by racing its tick().
    void stepFrame(int delta);
    void stepToStart();
    void stepToEnd();
    // Updates timeReadout_'s text to the current frame index / exact time (design decision 3),
    // wired to currentTimeChanged() and compositionChanged().
    void updateTimeReadout();

    CompositionSession& session_;
    QWidget* headerFallback_ = nullptr;
    QWidget* headerMenus_ = nullptr;
    QWidget* headerRight_ = nullptr;
    QMenu* editMenu_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* deleteLayerAction_ = nullptr;
    QAction* framesAction_ = nullptr;
    QAction* timecodeAction_ = nullptr;
    bool timecodeFormat_ = false;
    TimelineWorkAreaRow* workArea_ = nullptr;
    TimelineColumnHeaders* columnHeaders_ = nullptr;
    TimelineRuler* ruler_ = nullptr;
    TimelineKeyframePanel* keyframes_ = nullptr;
    // The two halves of one row grid (task T1), replacing the QTreeWidget this panel used to be.
    TimelineLayerStack* stack_ = nullptr;
    TimelineLaneRegion* lanes_ = nullptr;
    // ONE scrollbar drives both halves, so left/right scroll sync is a property of the structure
    // rather than a pair of signal handlers that could drift.
    QScrollBar* scrollBar_ = nullptr;
    QToolButton* addButton_ = nullptr;
    // Borrowed from the preview session; every panel controls the same transport.
    PlaybackController* playback_ = nullptr;
    // Borrowed: the RAM Preview command is application-wide (the Composition menu reaches the same
    // one), so this panel never owns it. Null when none was attached.
    RamPreviewController* ramPreview_ = nullptr;
    // Task U7 (issue #122), decision 5: clickable mouse affordances for the SAME
    // stepBackwardAction_/stepForwardAction_ QActions the Left/Right shortcuts already trigger --
    // wired by connecting the button's clicked() straight to the action's trigger() rather than
    // QToolButton::setDefaultAction(), so this button's own icon/tooltip/objectName stay under this
    // class's control instead of mirroring the action's text. Their enabled state is kept in
    // lockstep with the actions by the SAME focusChanged reconciliation lambda that already
    // disables the actions while the layer stack holds keyboard focus (see the constructor).
    QToolButton* stepBackButton_ = nullptr;
    QToolButton* stepForwardButton_ = nullptr;
    QToolButton* playPauseButton_ = nullptr;
    // RAM Preview (task PERF1, item 3): caches the composition range, then plays it from the cache.
    // Checked while a run is caching, so the one button is also the cancel affordance.
    QToolButton* ramPreviewButton_ = nullptr;
    // Non-interactive (decision 5: "non-interactive if loop isn't toggleable"): playback always
    // loops (PlaybackController::tick()'s exact modulo wrap) with no command to disable it, so this
    // is a status glyph, never a button that would falsely imply a click could turn looping off.
    QLabel* loopIndicator_ = nullptr;
    // Current frame index / exact time readout (design decision 3), living beside playPauseButton_
    // in the same controls cluster.
    QLabel* timeReadout_ = nullptr;
    // Frame-stepping QActions (design decision 2), disabled while the layer stack holds keyboard
    // focus -- see their construction site in the .cpp for the arrow-key conflict this reconciles.
    QAction* stepBackwardAction_ = nullptr;
    QAction* stepForwardAction_ = nullptr;
    QAction* stepToStartAction_ = nullptr;
    QAction* stepToEndAction_ = nullptr;
    QMetaObject::Connection focusConnection_;
};

// The LEFT layer-stack column (task T1), replacing the QTreeWidget this panel used to be: a painted
// row grid whose only real child widgets are the per-row Blending/Parent KDropdowns, recycled from
// a pool bounded by the viewport so widget count is independent of layer count.
//
// objectName "layerStackView" is deliberately unchanged -- same role, same name, new primitive.
class TimelineLayerStack final : public QWidget {
    Q_OBJECT

  public:
    TimelineLayerStack(CompositionSession& session, QScrollBar& scrollBar,
                       QWidget* parent = nullptr);

    void setEntries(std::vector<TimelineLayerEntry> entries);
    void setScrollOffset(int offset);

    [[nodiscard]] int rowCount() const noexcept { return static_cast<int>(entries_.size()); }
    // The resolved row descriptions, in stack order -- the durable name and the derived kind every
    // projection assertion used to read off QTreeWidgetItem::text(0)/text(1).
    [[nodiscard]] const std::vector<TimelineLayerEntry>& entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] int contentHeight() const noexcept;
    // The keyboard-navigation current row (the replacement for QTreeWidget::currentItem()/
    // setCurrentItem(), which playback_controller_tests.cpp used to assert the tree's own Home/End
    // navigation still ran while the step shortcuts were suppressed).
    [[nodiscard]] int currentRow() const noexcept { return currentRow_; }
    void setCurrentRow(int row);
    // The y of row `row` in this widget's own coordinates, scroll offset included. Exposed so a
    // test can prove the left column and the lane region move by exactly the same amount.
    [[nodiscard]] int rowTop(int row) const noexcept;
    // The tooltip this column shows at `position`: the honest reason one of the four reserved
    // toggle cells is inert, or the row's own kind/layer/slot line. Rows are transparent for mouse
    // events so that this -- and the hit test, and the keyboard -- all live in ONE place; it
    // doubles as the test seam for the honesty tooltips (the precedent being
    // TimelineRuler::majorTickLabelRectsForTest()).
    [[nodiscard]] QString toolTipAt(QPoint position) const;

  Q_SIGNALS:
    // Emitted whenever this column's own viewport height changes, so the editor can re-derive the
    // shared scrollbar's range from the new viewport rather than polling it.
    void viewportResized();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool event(QEvent* event) override;

  private:
    void relayoutRows();
    // Re-points currentRow_ at whatever layer the session now has selected, so a selection made in
    // another editor (the node canvas, the properties panel) moves this column's current row too.
    void syncCurrentRowFromSelection();

    CompositionSession& session_;
    QScrollBar& scrollBar_;
    std::vector<TimelineLayerEntry> entries_;
    std::vector<class TimelineLayerRow*> rowPool_;
    int scrollOffset_ = 0;
    int currentRow_ = -1;
};

// The RIGHT lane region (task T1): one lane per layer row, each carrying a rounded clip bar in the
// layer's own data-type color spanning the composition range, with the shared 1px Accent playhead
// drawn once across ALL of them. Entirely painted -- no child widgets at all -- so hundreds of rows
// cost one clipped paint, and the playhead moving never triggers a relayout.
class TimelineLaneRegion final : public QWidget {
    Q_OBJECT

  public:
    TimelineLaneRegion(CompositionSession& session, TimelineRuler& ruler, QScrollBar& scrollBar,
                       QWidget* parent = nullptr);

    void setEntries(std::vector<TimelineLayerEntry> entries);
    void setScrollOffset(int offset);

    [[nodiscard]] int contentHeight() const noexcept;
    [[nodiscard]] int rowTop(int row) const noexcept;
    // The clip bar rect of row `row`, in this widget's own coordinates -- the pinned geometry for
    // "the bar spans the composition range on its own lane".
    [[nodiscard]] std::optional<QRect> clipBarRect(int row) const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

  private:
    CompositionSession& session_;
    // Scrubbing a lane goes through the ruler's own scrub path, not a second copy of it.
    TimelineRuler& ruler_;
    QScrollBar& scrollBar_;
    std::vector<TimelineLayerEntry> entries_;
    int scrollOffset_ = 0;
};

// The column-header row's LEFT half (task T1): the four per-layer toggle glyphs (eye, audio, solo,
// lock) plus the Name/Blending/Parent labels, on exactly the same cell table the rows use. Every
// glyph is painted disabled, because not one of the four features exists -- see the .cpp.
class TimelineColumnHeaders final : public QWidget {
    Q_OBJECT

  public:
    explicit TimelineColumnHeaders(QWidget* parent = nullptr);

    // The honest reason the toggle cell at `x` is inert, or an empty string for the text columns.
    // Same seam, same reason, as TimelineLayerStack::toolTipAt().
    [[nodiscard]] static QString toolTipAtX(int x);

  protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;
};

} // namespace bloom::ui
