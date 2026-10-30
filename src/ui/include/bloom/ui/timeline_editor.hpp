#include <bloom/ui/kit/surfaces.hpp>
#pragma once

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/preferences_aware.hpp>

#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/media/audio/audio.hpp>

#include <bloom/ui/kit/tokens.hpp>

#include <QColor>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <map>
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
class KColorPicker;
class KIconToggle;
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
struct LayerDropHit;

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
    // ADR 0028: the layer mask a Masks-group row belongs to (invalid elsewhere).
    document::LayerMaskId maskId{};
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
    // Re-applies the ratio-based first-run default (37% of this panel's current width). The
    // workspace calls it after building a default layout once splitter geometry has settled: a
    // nested Timeline's show-time width can precede its splitter's final extent, so the showEvent()
    // default alone would compute the share from a not-yet-final width.
    void applyDefaultLayerColumnWidth();

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
    // The master Shy toggle: hides layers whose Shy switch is on from the layer list (After
    // Effects' Hide Shy Layers). Layout only; per composition, under timeline/hide-shy-layers/<id>.
    void setHideShyLayers(bool hide);
    void loadHideShyLayers();
    [[nodiscard]] static QString hideShySetting(document::CompositionId composition);
    void setLabelFilter(std::set<int> labels);
    // timeline_editor_keys.cpp: After Effects' Timeline keys (property reveals, J/K, [ and ]).
    void createTimelineKeys();
    void revealProperties(int reveal, bool additive);
    void stepToKeyframe(bool forward);
    void editTimingAtPlayhead(int edit);
    [[nodiscard]] std::vector<document::LayerId> keyTargetLayers() const;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void updateScrollRange();
    // #380: where an Assets or file drop at `position` lands (timeline_layer_drop.cpp): a row in
    // the layer stack, a row and a time over the lanes, otherwise on top at the playhead.
    [[nodiscard]] LayerDropHit layerDropHit(QPoint position) const;
    void indicateLayerDrop(const std::optional<LayerDropHit>& hit);

    std::set<document::LayerId> expandedLayers_;
    // A layer with a reveal shows only these parameters (A, P, S, R, T, U, UU, M, E).
    std::map<document::LayerId, std::set<document::ParameterId>> revealed_;
    std::set<int> labelFilter_;
    bool hideShyLayers_ = false;
    std::vector<QAction*> keyActions_;
    QPointer<QWidget> keyWindow_;
    std::optional<std::int64_t> lastRevealKeyMs_;
    bool lastRevealWasKeyed_ = false;
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

    // Drops every pooled row (and, with them, the rows' own session connections) immediately. A
    // session rebind to a composition-less project emits its other changed signals -- live value,
    // snapshot -- right after documentRebound(), and a stale pooled row would otherwise dereference
    // a composition that is already gone. The next rebuild recreates whatever rows the new document
    // needs; a blank project needs none. Also clears the transient row interaction state.
    void clearPooledRows();

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
    // The layer row a drop or reorder at `y` inserts above (rowCount(): below the last layer).
    [[nodiscard]] int insertionRowAt(int y) const noexcept;
    // The Accent insertion line (`timelineLayerInsertionIndicator`) above `row`; -1 hides it.
    void showInsertionIndicator(int row);
    // A group, parameter or component row the artist clicked stays highlighted while the session
    // selection is still the one that click made (its layer, or exactly that row's keys).
    [[nodiscard]] bool rowHighlighted(int row) const;
    [[nodiscard]] int hoveredRowForTest() const noexcept { return hoveredRow_; }
    // Selects row `row` as a click does: a layer, a group (its layer), or a parameter and all of
    // its keys.
    void selectRow(int row);
    // The label menu for `layer` (the selected layers when it is one of them) under `anchor`.
    void openLabelMenu(document::LayerId layer, QWidget* anchor);
    // #401: work the column does per document state, not per playback tick.
    struct Work final {
        std::uint64_t layerRowBinds = 0;
    };
    [[nodiscard]] Work work() const noexcept { return work_; }

  Q_SIGNALS:
    // Emitted whenever this column's own viewport height changes, so the editor can re-derive the
    // shared scrollbar's range from the new viewport rather than polling it.
    void viewportResized();
    void blankContextMenuRequested(QPoint globalPosition);
    void expansionRequested(document::LayerId layer);
    void groupExpansionRequested(document::LayerId layer, QString group);
    void parameterExpansionRequested(document::ParameterId parameter);
    void rowHighlightsChanged();

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void leaveEvent(QEvent* event) override;
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
    // Label Color > Custom…: the kit colour picker popup under the row, committing one "Set Layer
    // Label Color" step when it closes with a different colour (Escape keeps the old one).
    void openLabelColorPicker(int row, std::vector<document::LayerId> layers);
    void commitLabelColor();
    [[nodiscard]] std::vector<document::LayerId> actionLayers(document::LayerId layer) const;
    void toggleShy(document::LayerId layer);
    void setHoveredRow(int row);
    void updateRowStates();
    struct RowKey final {
        document::LayerId layer;
        TimelineLayerEntry::Kind kind = TimelineLayerEntry::Kind::Layer;
        document::ParameterId parameter;
        std::optional<document::AnimationComponent> component;
        QString group;
        document::LayerMaskId mask;
        friend bool operator==(const RowKey&, const RowKey&) = default;
    };
    [[nodiscard]] static RowKey rowKey(const TimelineLayerEntry& entry);
    std::optional<RowKey> selectedRow_;
    CompositionSelection selectionAtSelect_;
    int hoveredRow_ = -1;
    Work work_;
    QPointer<QLineEdit> renameEditor_;
    kit::KColorPicker* labelPicker_ = nullptr;
    QWidget* labelPickerAnchor_ = nullptr;
    document::CompositionId labelPickerComposition_{};
    std::vector<document::LayerId> labelPickerLayers_{};
    QColor labelPickerInitial_;
    bool labelPickerOpen_ = false;
    bool labelPickerCancelled_ = false;
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
    void paintContentExtent(QPainter& painter, int row, const QRect& bar) const;
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
    // A drop from Assets or the file manager (#380): the Accent time marker
    // (`timelineDropTimeMarker`) where the layers would start and the insertion line
    // (`timelineDropRowLine`) above `row` (-1: none). nullopt time hides the marker.
    void setDropMarker(std::optional<core::RationalTime> time, int row);
    [[nodiscard]] std::optional<core::RationalTime> dropMarkerTimeForTest() const noexcept {
        return dropTime_;
    }
    [[nodiscard]] int dropMarkerRowForTest() const noexcept { return dropRow_; }
    // The layer stack's answer for a clicked group or property row, so both halves of the row
    // wear the one selected-row wash.
    void setRowHighlight(std::function<bool(int)> highlighted);
    void syncRowHighlights();

    // #358 work counters: playback moves only the playhead strips, and a collapsed layer's key
    // summary is computed once per document state rather than once per paint.
    struct Work final {
        std::uint64_t keySummaryBuilds = 0;
        std::uint64_t playheadInvalidations = 0;
        std::uint64_t fullInvalidations = 0;
    };
    [[nodiscard]] Work work() const noexcept { return work_; }

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
    [[nodiscard]] std::vector<core::RationalTime> computeKeySummaryTimes(int row) const;
    // Repaints only the strips the playhead leaves and enters.
    void invalidatePlayhead();
    void invalidateAll();
    mutable std::map<std::uint64_t, std::vector<core::RationalTime>> keySummaryCache_;
    mutable document::Revision keySummaryRevision_;
    mutable const void* keySummaryProject_ = nullptr;
    mutable document::CompositionId keySummaryComposition_;
    std::optional<int> paintedPlayheadX_;
    mutable Work work_;
    QWidget* keyframeArea_ = nullptr;
    TimelineKeyframePanel* keyframePanel_ = nullptr;
    TimelineGraphView* graphView_ = nullptr;
    struct RangeDrag {
        document::LayerId layer;
        document::Revision revision;
        document::WorkArea original, preview;
        double pressSeconds;
        int handle; // -1: in, 0: body, 1: out
        // ADR 0024: Alt slips (body) or stretches (edge); the layer clock at press and in preview.
        bool alt = false;
        core::RationalTime startTime{};
        document::LayerSpeed speed{};
        std::optional<core::RationalTime> extentStart{};
        std::optional<core::RationalTime> extentEnd{};
        core::RationalTime previewStart{};
        document::LayerSpeed previewSpeed{};
    };
    std::optional<RangeDrag> drag_{};
    std::optional<core::RationalTime> guide_{};
    std::optional<core::RationalTime> dropTime_{};
    int dropRow_ = -1;
    QWidget* dropTimeMarker_ = nullptr;
    QWidget* dropRowLine_ = nullptr;
    bool keyframesVisible_ = true;
    bool snapping_ = true;
    bool graphEditor_ = false;
    std::function<bool(int)> rowHighlighted_;
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
    void setLabelFilterActive(bool active);
    void setHideShy(bool hide);

  Q_SIGNALS:
    void labelFilterRequested(QPoint globalPosition);
    void hideShyToggled(bool hide);

  protected:
    bool event(QEvent* event) override;

  private:
    kit::KIconToggle* labelFilter_ = nullptr;
    kit::KIconToggle* hideShy_ = nullptr;
    bool labelFilterActive_ = false;
};

} // namespace bloom::ui
