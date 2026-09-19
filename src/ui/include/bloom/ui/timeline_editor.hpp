#include <bloom/ui/kit/surfaces.hpp>
#pragma once

#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/preferences_aware.hpp>

#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/media/audio/audio.hpp>

#include <bloom/ui/kit/tokens.hpp>

#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QWidget>

#include <memory>
#include <optional>
#include <set>
#include <vector>

class QAction;
class QLabel;
class QLineEdit;
class QScrollBar;
class QMenu;
class QContextMenuEvent;
class QToolButton;

namespace bloom::ui {

namespace kit {
class KSplitHandle;
} // namespace kit

class CompositionPreviewController;
class CompositionSession;
class TimelineColumnHeaders;
class TimelineKeyframePanel;
class TimelineLaneRegion;
class TimelineGraphView;
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
    QColor labelColor{};
    enum class Kind { Layer, Group, Parameter, Component };
    Kind rowKind = Kind::Layer;
    // task TL-FIX2: the row's nesting depth -- layer 0, group 1, parameter 2, component 3 (a
    // DRIVE-1 upstream group and its parameters nest the same as Object/Transform/Source, at 1 and
    // 2). Carried explicitly rather than derived from rowKind so a future reordering of Kind can
    // never silently change what the layer stack indents by.
    int depth = 0;
    document::ParameterId parameterId{};
    // The component this row addresses, for a vector or colour parameter split into per-component
    // rows. Empty for aggregate parameter rows.
    std::optional<document::AnimationComponent> component{};
    std::string role{};
    bool expanded = false;
    QString group{};
    document::NodeId imageNodeId{};
    document::NodeId audioNodeId{};
    std::shared_ptr<const media::audio::WaveformSummary> waveform{};
};

// Layer stack and lanes share one vertical scroll. EditorArea hosts the split header's name,
// menus and ruler; column headings start the body, with transport and navigator below the lanes.
class TimelineEditor final : public QWidget, public EditorChromeProvider, public PreferencesAware {
    Q_OBJECT

  public:
    [[nodiscard]] EditorChromeSpec& editorChrome() override { return chrome_; }
    // Applies the Preferences window's committed timeline preferences through the panel's own
    // setters, so the Preferences window and the Timeline's own View menu share one code path.
    void applyApplicationPreferences(const ApplicationPreferences& preferences) override;
    // Task VIEW-1 moved the transport -- and with it the RAM Preview button this constructor used
    // to take a controller for -- to the viewer footer. This panel is the layer stack, the ruler,
    // the lanes and the navigator now; it owns no transport command at all.
    TimelineEditor(CompositionSession& session, CompositionPreviewController& previewController,
                   QWidget* parent = nullptr);

    // Construction-time fallback for the LEFT layer-stack column. The live first-run/reset
    // default is 37% of the Timeline's width once geometry exists; this token-sized fallback keeps
    // the panel non-degenerate during construction and preserves the existing API.
    [[nodiscard]] static int layerColumnWidth();
    [[nodiscard]] static int propertyNameIndent();

    // Test seams (mirroring TimelineRuler::majorTickLabelRectsForTest()'s precedent): the three
    // widgets whose geometry is this task's pinned contract.
    [[nodiscard]] TimelineLayerStack* layerStackForTest() const noexcept { return stack_; }
    [[nodiscard]] TimelineLaneRegion* laneRegionForTest() const noexcept { return lanes_; }
    [[nodiscard]] TimelineRuler* rulerForTest() const noexcept { return ruler_; }
    [[nodiscard]] QScrollBar* verticalScrollBarForTest() const noexcept { return scrollBar_; }
    // task TL-FIX2. The draggable layer-table/lanes divider, and the live width it edits.
    [[nodiscard]] kit::KSplitHandle* splitHandleForTest() const noexcept { return splitHandle_; }
    [[nodiscard]] int layerColumnWidthForTest() const noexcept { return layerColumnWidth_; }
    // The floor setLayerColumnWidth() clamps to; public only so a test can assert the clamp without
    // re-deriving the token arithmetic.
    [[nodiscard]] int minLayerColumnWidthForTest() const noexcept { return minLayerColumnWidth(); }
    void persistLayerColumnWidth();

  private:
    EditorChromeSpec chrome_;
    void rebuild();
    // task TL-FIX2. Applies `width` (clamped to the layer table's own minimum, and, once this
    // widget's own width() is meaningful, to a maximum that leaves Size::PanelMinWidth for the
    // lanes) to every widget that reads the split: the layer stack and its column headers, the
    // EditorArea-hosted header split via chrome_.refreshSplit, and the bare-panel fallback
    // header/navigator leading cells. Persists to QSettings when `persist` is true (a completed
    // drag or a reset, never a live drag frame, so scrubbing the handle does not thrash disk).
    void setLayerColumnWidth(int width, bool persist);
    [[nodiscard]] int minLayerColumnWidth() const noexcept;
    void updateSelection();
    void updateHistoryActions();
    void createHeaderMenus();
    void createFooter();
    void refreshHeaderMenus();
    void selectAllLayers();
    void deleteSelectedLayers();
    void setTimecodeFormat(bool timecode);
    // The three header toggles. Each persists under its own QSettings key, is applied to the
    // widgets it governs, and re-tints its own glyph -- there is no third place that decides what
    // "on" looks like.
    void setKeyframesVisible(bool visible);
    void setSnappingEnabled(bool enabled);
    void setGraphEditorEnabled(bool enabled);
    void showEvent(QShowEvent* event) override;
    void updateScrollRange();

    std::set<document::LayerId> expandedLayers_;
    std::set<document::ParameterId> expandedParameters_;
    std::set<std::pair<document::LayerId, QString>> collapsedGroups_;
    CompositionSession& session_;
    QWidget* headerFallback_ = nullptr;
    QWidget* headerMenus_ = nullptr;
    QWidget* headerRight_ = nullptr;
    QMenu* editMenu_ = nullptr;
    QMenu* blankLayerContextMenu_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* deleteLayerAction_ = nullptr;
    QAction* splitLayerAction_ = nullptr;
    QAction* framesAction_ = nullptr;
    QAction* timecodeAction_ = nullptr;
    QAction* keyframesAction_ = nullptr;
    QAction* graphAction_ = nullptr;
    QAction* snapAction_ = nullptr;
    QWidget* footerLeft_ = nullptr;
    bool timecodeFormat_ = false;
    bool keyframesVisible_ = true;
    bool snapping_ = true;
    bool graphEditor_ = false;
    TimelineWorkAreaRow* workArea_ = nullptr;
    TimelineColumnHeaders* columnHeaders_ = nullptr;
    TimelineRuler* ruler_ = nullptr;
    // The two halves of one row grid (task T1), replacing the QTreeWidget this panel used to be.
    TimelineLayerStack* stack_ = nullptr;
    TimelineLaneRegion* lanes_ = nullptr;
    // ONE scrollbar drives both halves, so left/right scroll sync is a property of the structure
    // rather than a pair of signal handlers that could drift.
    QScrollBar* scrollBar_ = nullptr;
    QToolButton* addButton_ = nullptr;
    // task TL-FIX2: the draggable layer-table/lanes divider, the live (persisted) column width it
    // edits, and the width layerColumnWidth_ held at the start of the current drag gesture --
    // dragged() reports a cumulative delta from press, not a per-move one, so applying it needs the
    // gesture's starting point, not the ever-changing current width.
    kit::KSplitHandle* splitHandle_ = nullptr;
    int layerColumnWidth_ = layerColumnWidth();
    int dragStartColumnWidth_ = 0;
};

// The LEFT layer-stack column (task T1), replacing the QTreeWidget this panel used to be: a painted
// row grid whose only real child widgets are the per-row Blending/Parent KDropdowns, recycled from
// a pool bounded by the viewport so widget count is independent of layer count.
//
// objectName "layerStackView" is deliberately unchanged -- same role, same name, new primitive.
class TimelineLayerStack final : public kit::KListSurface {
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
    void blankContextMenuRequested(QPoint globalPosition);
    void expansionRequested(document::LayerId layer);
    void groupExpansionRequested(document::LayerId layer, QString group);
    void parameterExpansionRequested(document::ParameterId parameter);

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void relayoutRows();
    void renameLayer(document::LayerId layer);
    void updateRenameGeometry();
    QPointer<QLineEdit> renameEditor_;
    document::LayerId renamingLayer_{};
    int anchorRow_ = -1;
    int dragRow_ = -1;
    int insertionRow_ = -1;
    QPoint dragStart_{};
    document::Revision dragRevision_{};
    QWidget* insertion_ = nullptr;
    // Re-points currentRow_ at whatever layer the session now has selected, so a selection made in
    // another editor (the node canvas, the properties panel) moves this column's current row too.
    void syncCurrentRowFromSelection();

    CompositionSession& session_;
    QScrollBar& scrollBar_;
    std::vector<TimelineLayerEntry> entries_;
    std::vector<class TimelineLayerRow*> rowPool_;
    std::vector<class TimelinePropertyRow*> propertyPool_;
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
    [[nodiscard]] std::vector<core::RationalTime> keySummaryTimes(int row) const;
    // Hiding keys hides BOTH surfaces that show them: the per-parameter key lanes and the
    // collapsed layer rows' summary glyphs. Showing one without the other would make a collapsed
    // layer claim keys the expanded rows no longer draw.
    void setKeyframesVisible(bool visible);
    [[nodiscard]] bool keyframesVisible() const noexcept { return keyframesVisible_; }
    void setSnappingEnabled(bool enabled);
    // In graph mode the curve view REPLACES the key lanes and this region paints only its own
    // Surface backdrop behind it. Two views of the same keys at once would only leave the artist
    // asking which one they are editing.
    void setGraphEditorEnabled(bool enabled);
    [[nodiscard]] bool graphEditorEnabled() const noexcept { return graphEditor_; }
    [[nodiscard]] TimelineGraphView* graphViewForTest() const noexcept { return graphView_; }

  Q_SIGNALS:
    void expansionRequested(document::LayerId layer);

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    void syncKeyframeSurfaces();
    QWidget* keyframeArea_ = nullptr;
    TimelineKeyframePanel* keyframePanel_ = nullptr;
    TimelineGraphView* graphView_ = nullptr;
    struct RangeDrag {
        document::LayerId layer;
        document::Revision revision;
        document::WorkArea original, preview;
        double pressSeconds;
        int handle; // -1: in, 0: body, 1: out
    };
    std::optional<RangeDrag> drag_{};
    std::optional<core::RationalTime> guide_{};
    bool keyframesVisible_ = true;
    bool snapping_ = true;
    bool graphEditor_ = false;
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
    bool event(QEvent* event) override;
};

} // namespace bloom::ui
