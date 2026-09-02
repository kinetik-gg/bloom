#pragma once

#include <bloom/ui/playback_controller.hpp>

#include <QString>
#include <QWidget>

class QAction;
class QLabel;
class QListWidget;
class QToolButton;
class QTreeWidget;

namespace bloom::core {
struct Color4d;
} // namespace bloom::core

namespace bloom::document {
struct ParameterRecord;
} // namespace bloom::document

namespace bloom::ui {

class CompositionPreviewController;
class CompositionSession;
class TimelineKeyframePanel;
class TimelineRuler;
class TimelineWorkAreaStrip;

namespace kit {
class KValueField;
} // namespace kit

// --- Shared authoring/presentation truth (task U4, issue #123) ---------------------------------
//
// Three decisions the Properties panel and the Timeline's Add menu already owned, now CALLED by the
// Nodes canvas rather than copied into it. Task U4's decision 4 requires the node editor's Add menu
// to offer "exactly the layer kinds the command layer offers today (mirror the timeline's Add menu
// contents)", and its decision 5 requires in-node fields to read and commit through "the EXACT same
// session/command paths Properties uses". Copying the default-name rule, the built-in proof
// palette, or the source wording into node_editor.cpp would have duplicated four raw color literals
// and three user-visible strings, and would let the two surfaces silently drift. These declarations
// exist so there is exactly one definition of each; every one of them is defined in
// composition_editors.cpp, beside the anonymous-namespace helpers it already used.

// "Constant" / "Animated" / "Driven by graph" -- the one wording for a parameter's source, shown by
// the Properties rows' tooltips and by the node card's own field tooltips.
[[nodiscard]] QString parameterSourceDescription(const document::ParameterRecord& parameter);

// The exact, never-rounded and never-clipped "R r  G g  B b  A a" rendering of an authoring color
// (negative and HDR channels included -- docs/architecture/evaluation-primitives.md's straight
// Color4d authoring values). The Properties Solid Source row shows it as its value text; the node
// card's read-only color chip carries it in its tooltip, because a quantized 8-bit swatch cannot
// show an out-of-range channel honestly on its own.
[[nodiscard]] QString exactColorText(core::Color4d color);

// One "Add Solid"/"Add Text" gesture, two entry points (the Timeline's Add menu, the Nodes canvas
// context menu): same default numbered name derived from the composition's own existing layers,
// same next built-in reference-linear-sRGB proof color, same single command transaction. Returns
// what CompositionSession::addSolidLayer()/addTextLayer() returned.
[[nodiscard]] bool addDefaultSolidLayer(CompositionSession& session);
[[nodiscard]] bool addDefaultTextLayer(CompositionSession& session);

class TimelineEditor final : public QWidget {
    Q_OBJECT

  public:
    TimelineEditor(CompositionSession& session, CompositionPreviewController& previewController,
                   QWidget* parent = nullptr);

    // Track row column layout (task U7, issue #122, decision 1). Logical column indices are a
    // stable test contract (composition_projection_test.cpp and playback_controller_tests.cpp
    // already read text(kNameColumn)/text(kKindColumn) and topLevelItem() by these positions), so
    // Name/Kind stay logical columns 0/1 exactly as before this task; the new decision-1 columns
    // are appended after them and reordered to paint LEFTMOST via QHeaderView::moveSection() in
    // the .cpp, which moves visual position only -- QTreeWidgetItem::text()/icon()/toolTip() and
    // QHeaderView::logicalIndex() are completely unaffected by that visual reorder.
    static constexpr int kNameColumn = 0;
    static constexpr int kKindColumn = 1;
    static constexpr int kVisibilityColumn = 2;
    static constexpr int kBlendingColumn = 3;
    static constexpr int kParentColumn = 4;
    static constexpr int kLaneColumn = 5;
    static constexpr int kColumnCount = 6;

  private:
    void rebuild();
    void updateSelection();
    void updateHistoryActions();
    // Reflects PlaybackController::stateChanged() onto the toggle button's text/tooltip/checked
    // state (design decision 4: "button/icon state reflects transport state via a signal").
    void updatePlaybackButton(PlaybackState state);
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
    TimelineWorkAreaStrip* workArea_ = nullptr;
    TimelineRuler* ruler_ = nullptr;
    TimelineKeyframePanel* keyframes_ = nullptr;
    QTreeWidget* layers_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* undoButton_ = nullptr;
    QToolButton* redoButton_ = nullptr;
    // Owned here rather than shared with any sibling TimelineEditor a workspace split could open
    // (issue #105): each TimelineEditor instance gets its own transport, matching how every other
    // per-panel affordance in this class (the layer tree selection sync, the undo/redo buttons'
    // enabled state) is already independently driven off the shared CompositionSession/
    // CompositionPreviewController rather than a single cross-panel singleton.
    PlaybackController* playback_ = nullptr;
    // Task U7 (issue #122), decision 5: clickable mouse affordances for the SAME
    // stepBackwardAction_/stepForwardAction_ QActions the Left/Right shortcuts already trigger --
    // wired by connecting the button's clicked() straight to the action's trigger() rather than
    // QToolButton::setDefaultAction(), so this button's own icon/tooltip/objectName stay under
    // this class's control instead of mirroring the action's text. Their enabled state is kept in
    // lockstep with the actions by the SAME focusChanged reconciliation lambda that already
    // disables the actions while layers_ holds keyboard focus (see the constructor).
    QToolButton* stepBackButton_ = nullptr;
    QToolButton* stepForwardButton_ = nullptr;
    QToolButton* playPauseButton_ = nullptr;
    // Non-interactive (decision 5: "non-interactive if loop isn't toggleable"): playback always
    // loops (PlaybackController::tick()'s exact modulo wrap) with no command to disable it, so this
    // is a status glyph, never a button that would falsely imply a click could turn looping off.
    QLabel* loopIndicator_ = nullptr;
    // Current frame index / exact time readout (design decision 3), living beside playPauseButton_
    // in the same controls row.
    QLabel* timeReadout_ = nullptr;
    // Frame-stepping QActions (design decision 2), disabled while layers_ holds keyboard focus --
    // see their construction site in the .cpp for the arrow-key conflict this reconciles.
    QAction* stepBackwardAction_ = nullptr;
    QAction* stepForwardAction_ = nullptr;
    QAction* stepToStartAction_ = nullptr;
    QAction* stepToEndAction_ = nullptr;
    bool rebuilding_ = false;
};

class PropertiesEditor final : public QWidget {
    Q_OBJECT

  public:
    explicit PropertiesEditor(CompositionSession& session, QWidget* parent = nullptr);

  private:
    void rebuild();
    void configurePosition();
    void configureOpacity();
    void configureSolidColor();
    // Issue #120, decision 3: composition/document properties shown in place of an empty panel
    // when nothing is selected. Toggles documentSection_/selectionSection_ visibility and fills
    // documentSection_'s rows from composition()'s own read-only format/duration -- never a new
    // CompositionSession API.
    void configureDocumentProperties();

    CompositionSession& session_;
    QLabel* selectionLabel_ = nullptr;

    // The selection-driven groups (Transform/Appearance/source-specific), shown together and
    // hidden as one unit whenever configureDocumentProperties() shows documentSection_ instead
    // (issue #120, decision 3).
    QWidget* selectionSection_ = nullptr;
    kit::KValueField* positionX_ = nullptr;
    kit::KValueField* positionY_ = nullptr;
    QLabel* positionKeyframe_ = nullptr;
    kit::KValueField* opacity_ = nullptr;
    QLabel* opacityKeyframe_ = nullptr;
    QWidget* solidColorPanel_ = nullptr;
    QLabel* solidColorKeyframe_ = nullptr;
    QLabel* solidColorValue_ = nullptr;
    QLabel* solidAlphaAssociation_ = nullptr;
    QLabel* solidColorEncoding_ = nullptr;

    // The no-selection document/composition view (issue #120, decision 3).
    QWidget* documentSection_ = nullptr;
    QLabel* documentName_ = nullptr;
    QLabel* documentFormat_ = nullptr;
    QLabel* documentFrameRate_ = nullptr;
    QLabel* documentDuration_ = nullptr;
    QLabel* documentPixelAspect_ = nullptr;

    bool rebuilding_ = false;
};

class MediaEditor final : public QWidget {
    Q_OBJECT

  public:
    explicit MediaEditor(CompositionSession& session, QWidget* parent = nullptr);

  private:
    void rebuild();
    void updateSelection();

    CompositionSession& session_;
    QListWidget* compositions_ = nullptr;
    bool rebuilding_ = false;
};

} // namespace bloom::ui
