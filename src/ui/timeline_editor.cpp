#include <bloom/ui/timeline_editor.hpp>

#include "composition_editor_support.hpp"

#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QModelIndex>
#include <QPainter>
#include <QPen>
#include <QSignalBlocker>
#include <QSize>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <chrono>
#include <cstdint>
#include <string_view>
#include <variant>

namespace bloom::ui {
namespace {

constexpr int kTimelineLayerIdRole = Qt::UserRole + 1;
constexpr int kTimelineSlotIdRole = Qt::UserRole + 2;

QString layerKind(const CompositionSession& session, const document::LayerId layerId) {
    const auto* sourceNode = directSourceNode(session, layerId);
    if (isKnownSource(sourceNode, document::kSolidSourceNodeType,
                      document::kSolidSourceNodeSchemaVersion)) {
        return TimelineEditor::tr("Solid");
    }
    if (isKnownSource(sourceNode, document::kTextSourceNodeType,
                      document::kTextSourceNodeSchemaVersion)) {
        return TimelineEditor::tr("Text");
    }
    return TimelineEditor::tr("Layer");
}

// Lane bar color mapping (task U7, issue #122, decision 2). Bloom's data-type palette
// (kit::Color::Data*) identifies external MEDIA kinds -- image sequences, clips, still images,
// audio -- that this project has no import pipeline for yet (verified: no media-backed layer type
// exists anywhere in src/document). Mapping a Solid or Text layer to one of those roles (e.g.
// "Solid = DataClip green", the literal example this task's own decision explicitly rejects) would
// misrepresent a generated, composition-local layer as referenced media it is not.
// `DataComposition` is the one palette entry that is NOT about referenced media: its own stated
// meaning is "Compositions" -- content that is authored/evaluated INSIDE a project rather than
// referenced from an external asset, exactly the property every layer kind Bloom has today (Solid,
// Text) shares. This is therefore ONE mapping applied uniformly to every layer kind that exists
// right now, not a per-kind table -- there is nothing to honestly differentiate yet. A future
// media-backed layer (image/clip/sequence/audio import) would take its own, more specific Data*
// token when that feature ships; this function is the one place that day's change would land.
[[nodiscard]] kit::Color layerLaneColorToken() { return kit::Color::DataComposition; }

QToolButton* makeToolButton(const QString& text, const QString& accessibleName, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setAccessibleName(accessibleName);
    button->setAutoRaise(true);
    return button;
}

// Task U7 (issue #122), decision 5: transport controls get their kit icon glyph via the SAME
// "plain QToolButton + kit::icon()" idiom EditorArea's own header chrome already uses
// (editor_area.cpp's makeHeaderButton) -- not kit::KButton, which is not a QToolButton and would
// break every existing findChild<QToolButton*>("playPauseButton") test contract
// (composition_projection_test.cpp, playback_controller_tests.cpp). An icon never replaces an
// accessible name (ADR 0010; docs/ux/visual-language.md's Iconography section), so every call site
// still sets both a tooltip and setAccessibleName().
QToolButton* makeIconToolButton(const kit::IconId iconId, const QString& toolTip,
                                const QString& accessibleName, const QString& objectName,
                                QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setIcon(kit::icon(iconId, kit::Size::IconMedium));
    button->setIconSize(QSize(kit::px(kit::Size::IconMedium), kit::px(kit::Size::IconMedium)));
    button->setToolTip(toolTip);
    button->setAccessibleName(accessibleName);
    button->setAutoRaise(true);
    // task U8, issue #131, fix 7: an icon-only QToolButton (this idiom carries no visible text --
    // see the comment above this function) sizes to controlExtent x controlExtent exactly, the
    // same square target kit::KButton's own icon-only sizeHint() now pins.
    button->setFixedSize(kit::px(kit::Size::Control), kit::px(kit::Size::Control));
    return button;
}

// The 2px inset selection edge (task U7, issue #122, decision 1: "selection = Accent inset edge
// per States" -- docs/ux/visual-language.md's "Selected: an Accent fill, or a 2px inset accent edge
// where a fill would hide content"; a full fill would hide this row's own name/kind text, so the
// edge variant applies).
constexpr qreal kTrackRowSelectionEdgeWidth = 2.0;

// Restyles layerStackView's rows (decision 1: "Row striping via surface ladder; selection = Accent
// inset edge per States") without touching a single byte of QTreeWidgetItem's own model data --
// text()/icon()/toolTip()/isSelected() and every existing test reading them through
// findChild<QTreeWidget*>("layerStackView") are completely untouched. Only sizeHint() (row height,
// decision 1's TimelineRow token) and paint() (background + selection) are overridden; the base
// QStyledItemDelegate::paint() still draws each column's own icon/text/embedded item-widget
// afterward, using whatever font/foreground QTreeWidgetItem::setFont()/setForeground() were given
// at row-build time (see TimelineEditor::rebuild()) -- so this delegate owns none of the actual ink
// color decisions, only the shared background/selection recipe every column sits on.
class TimelineTrackRowDelegate final : public QStyledItemDelegate {
  public:
    explicit TimelineTrackRowDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        hint.setHeight(kit::px(kit::Size::TimelineRow));
        return hint;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        // "Row striping via surface ladder": odd rows step one rung up from Background, the same
        // ladder the rest of the interface steps hover/press states along.
        const auto surfaceToken = (index.row() % 2) != 0
                                      ? kit::surfaceStep(kit::Color::Background, 1)
                                      : kit::Color::Background;
        painter->fillRect(option.rect, kit::color(surfaceToken));
        if (selected) {
            const qreal inset = kTrackRowSelectionEdgeWidth / 2.0;
            painter->setPen(QPen(kit::color(kit::Color::Accent), kTrackRowSelectionEdgeWidth));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(QRectF(option.rect).adjusted(inset, inset, -inset, -inset));
        }
        painter->restore();

        // Suppress Qt's own native full-cell selection fill (already replaced above by the inset
        // edge) before delegating icon/text painting to the base implementation.
        QStyleOptionViewItem plain(option);
        plain.state &= ~QStyle::State_Selected;
        plain.showDecorationSelected = false;
        QStyledItemDelegate::paint(painter, plain, index);
    }
};

// The per-row lane bar (decision 2): a single rounded, type-color-coded bar filling its ENTIRE
// allotted width. There is no trim/range feature (no in/out point exists in the document model at
// all), so "spans the full composition duration honestly" is true by construction here -- the bar
// is never partial, because nothing in this widget can express a partial state.
//
// Honesty disclosure (this task's raw report): this bar's width is the layerStackView TREE
// COLUMN's own width, not literally the same pixel-space TimelineRuler/TimelineKeyframePanel use --
// those are separate, independently-scrollable widgets from the layer list, and pixel-perfect
// cross-widget time-axis alignment between a QTreeWidget column and the ruler was never a wired
// feature before this task. Building that alignment is a genuine architecture change outside a
// restyle task's fence, so this bar honestly represents "this layer's content, for its entire
// duration" within its own dedicated column rather than pretending to share the ruler's exact
// scroll/zoom state.
class TimelineLaneBar final : public QWidget {
  public:
    explicit TimelineLaneBar(const kit::Color colorToken, QWidget* parent)
        : QWidget(parent), colorToken_(colorToken) {
        setObjectName(QStringLiteral("layerLaneBar"));
        setMinimumHeight(kit::px(kit::Size::TimelineRow) - 2 * kit::px(kit::Spacing::XXS));
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF bounds =
            QRectF(rect()).adjusted(0.0, static_cast<qreal>(kit::px(kit::Spacing::XXS)), 0.0,
                                    -static_cast<qreal>(kit::px(kit::Spacing::XXS)));
        kit::fillRoundedSurface(painter, bounds, kit::color(colorToken_), QColor(),
                                kit::Radius::Small);
    }

  private:
    kit::Color colorToken_;
};

// Blending/Parent (decision 1): one always-disabled KDropdown per row, each carrying its single
// honest value ("Normal" / "None") -- no blend-mode breadth and no parenting feature exist yet, so
// there is nothing else to offer, and the tooltip says so rather than the control merely looking
// unresponsive.
kit::KDropdown* makeDisabledPlaceholderDropdown(const QString& value, const QString& toolTip,
                                                const QString& objectName, QWidget* parent) {
    auto* dropdown = new kit::KDropdown(parent);
    dropdown->setObjectName(objectName);
    dropdown->addItem(value);
    dropdown->setEnabled(false);
    dropdown->setToolTip(toolTip);
    return dropdown;
}

} // namespace

TimelineEditor::TimelineEditor(CompositionSession& session,
                               CompositionPreviewController& previewController, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("timelineEditor");
    setAccessibleName(tr("Layers timeline"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* controls = new QWidget(this);
    controls->setObjectName("timelineControls");
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(kit::px(kit::Spacing::S), kit::px(kit::Spacing::XS),
                                       kit::px(kit::Spacing::S), kit::px(kit::Spacing::XS));
    controlsLayout->setSpacing(kit::px(kit::Spacing::XS));

    auto* title = new QLabel(tr("Layers"), controls);
    title->setObjectName("editorSectionTitle");
    addButton_ = makeToolButton(tr("Add"), tr("Add layer"), controls);
    addButton_->setObjectName("addLayerButton");
    // Decision 5: "Add menu restyled" -- the Add glyph itself; menu entries stay plain text (no
    // Data* token honestly names a generic structured layer the way Add names the action).
    addButton_->setIcon(kit::icon(kit::IconId::Add, kit::Size::IconMedium));
    addButton_->setIconSize(QSize(kit::px(kit::Size::IconMedium), kit::px(kit::Size::IconMedium)));
    // task U8, issue #131, fix 7: icon-only (its "Add" text() is carried only for
    // accessibility/tests -- the default IconOnly toolButtonStyle never paints it) so it sizes to
    // controlExtent x controlExtent exactly, like every other icon-only button in the audit.
    addButton_->setFixedSize(kit::px(kit::Size::Control), kit::px(kit::Size::Control));
    addButton_->setPopupMode(QToolButton::InstantPopup);
    addButton_->setToolTip(tr("Add a structured layer"));
    auto* addMenu = new QMenu(tr("Add Layer"), addButton_);
    addMenu->setObjectName("addLayerMenu");
    addMenu->setAccessibleName(tr("Add layer menu"));
    auto* addSolidAction = addMenu->addAction(tr("Solid"));
    addSolidAction->setObjectName("addSolidLayerAction");
    addSolidAction->setToolTip(
        tr("Add a solid using the next built-in reference-linear-sRGB proof color"));
    auto* addTextAction = addMenu->addAction(tr("Text"));
    addTextAction->setObjectName("addTextLayerAction");
    addTextAction->setToolTip(tr("Add a text layer"));
    addButton_->setMenu(addMenu);

    // Playback transport (issue #105, decision 4; restyled task U7, issue #122, decision 5): a
    // StepBack/Play-Pause/StepForward/Loop row. playPauseButton_ MUST stay a QToolButton with its
    // existing text()/isChecked() contract (playback_controller_tests.cpp,
    // composition_projection_test.cpp both read it by exactly that type/objectName) -- only its
    // icon changes here; updatePlaybackButton() below swaps the icon alongside the existing
    // text/tooltip swap.
    stepBackButton_ = makeIconToolButton(kit::IconId::StepBack, tr("Step back one frame (Left)"),
                                         tr("Step back one frame"),
                                         QStringLiteral("timelineStepBackButton"), controls);
    playPauseButton_ = makeToolButton(tr("Play"), tr("Toggle playback"), controls);
    playPauseButton_->setObjectName("playPauseButton");
    playPauseButton_->setCheckable(true);
    // task U8, issue #131, fix 7: icon-only in the same sense addButton_ is above (its text()/
    // isChecked() stay a pinned programmatic contract; the default IconOnly toolButtonStyle never
    // paints the label), so it takes the same controlExtent square as its transport siblings.
    playPauseButton_->setFixedSize(kit::px(kit::Size::Control), kit::px(kit::Size::Control));
    stepForwardButton_ = makeIconToolButton(
        kit::IconId::StepForward, tr("Step forward one frame (Right)"),
        tr("Step forward one frame"), QStringLiteral("timelineStepForwardButton"), controls);
    // Loop indicator (decision 5): non-interactive status glyph, not a button -- playback always
    // loops (PlaybackController::tick()'s exact modulo wrap) and there is no command to disable it,
    // so a clickable control here would dishonestly imply a toggle that does not exist.
    loopIndicator_ = new QLabel(controls);
    loopIndicator_->setObjectName(QStringLiteral("timelineLoopIndicator"));
    loopIndicator_->setAccessibleName(tr("Playback loops continuously"));
    loopIndicator_->setToolTip(tr("Playback always loops; there is no command to disable it yet"));
    loopIndicator_->setPixmap(kit::iconPixmap(kit::IconId::Loop, kit::Size::IconMedium,
                                              kit::Color::Accent, kit::State::Normal,
                                              kit::IconWeight::Fill));
    // Current time readout (issue #108, decision 3): a small label beside the transport, matching
    // PropertiesEditor::selectionLabel_'s plain-QLabel idiom (setObjectName + selectable text, no
    // bespoke styling). Text is set by updateTimeReadout() below, not here. Restyled (decision 5:
    // "frame/time readout stays the exact formatter, Geist Mono") to the Value type role -- the
    // FORMATTER (formatExactSeconds(), updateTimeReadout()'s own "Frame %1 · %2" shape) is
    // completely unchanged, only the font.
    timeReadout_ = new QLabel(controls);
    timeReadout_->setObjectName("timelineTimeReadout");
    timeReadout_->setAccessibleName(tr("Current frame and time"));
    timeReadout_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    timeReadout_->setFont(kit::font(kit::TypeRole::Value));
    undoButton_ = makeToolButton(tr("Undo"), tr("Undo last edit"), controls);
    redoButton_ = makeToolButton(tr("Redo"), tr("Redo last edit"), controls);
    controlsLayout->addWidget(title);
    controlsLayout->addWidget(addButton_);
    controlsLayout->addWidget(stepBackButton_);
    controlsLayout->addWidget(playPauseButton_);
    controlsLayout->addWidget(stepForwardButton_);
    controlsLayout->addWidget(loopIndicator_);
    controlsLayout->addWidget(timeReadout_);
    controlsLayout->addStretch(1);
    controlsLayout->addWidget(undoButton_);
    controlsLayout->addWidget(redoButton_);

    workArea_ = new TimelineWorkAreaStrip(session_, this);
    ruler_ = new TimelineRuler(session_, previewController, this);
    keyframes_ = new TimelineKeyframePanel(session_, this);
    playback_ = new PlaybackController(session_, previewController, &std::chrono::steady_clock::now,
                                       std::chrono::milliseconds{16}, this);

    layers_ = new QTreeWidget(this);
    layers_->setObjectName("layerStackView");
    layers_->setAccessibleName(tr("Composition layers"));
    layers_->setColumnCount(kColumnCount);
    // Header text stays attached to its LOGICAL column below regardless of the visual reorder
    // (moveSection() a few lines down): text(kNameColumn)/text(kKindColumn) remain the pinned test
    // contract's exact logical columns 0/1.
    layers_->setHeaderLabels(
        {tr("Name"), tr("Kind"), tr("Visible"), tr("Blending"), tr("Parent"), tr("Lane")});
    layers_->setRootIsDecorated(false);
    // Striping now comes from TimelineTrackRowDelegate's own surface-ladder recipe (decision 1),
    // not Qt's native AlternateBase.
    layers_->setAlternatingRowColors(false);
    layers_->setSelectionMode(QAbstractItemView::SingleSelection);
    layers_->setUniformRowHeights(true);
    layers_->setItemDelegate(new TimelineTrackRowDelegate(layers_));
    layers_->header()->setSectionResizeMode(kNameColumn, QHeaderView::Interactive);
    layers_->header()->setSectionResizeMode(kKindColumn, QHeaderView::ResizeToContents);
    layers_->header()->setSectionResizeMode(kVisibilityColumn, QHeaderView::ResizeToContents);
    layers_->header()->setSectionResizeMode(kBlendingColumn, QHeaderView::ResizeToContents);
    layers_->header()->setSectionResizeMode(kParentColumn, QHeaderView::ResizeToContents);
    layers_->header()->setSectionResizeMode(kLaneColumn, QHeaderView::Stretch);
    // Decision 1's left-to-right narrative order (visibility, then name, kind, blending, parent,
    // with the lane body trailing) is a VISUAL reorder only -- QHeaderView::moveSection() moves
    // visual position, never logical index, so text(kNameColumn)/text(kKindColumn) and every
    // setItemWidget(item, kBlendingColumn/...) call below keep addressing the same logical columns
    // regardless of where the header paints them.
    layers_->header()->moveSection(kVisibilityColumn, 0);

    layout->addWidget(controls);
    layout->addWidget(workArea_);
    layout->addWidget(ruler_);
    layout->addWidget(keyframes_);
    layout->addWidget(layers_, 1);

    // One definition of the "Add Solid"/"Add Text" default name and proof color, shared with the
    // Nodes canvas context menu (task U4, issue #123) -- see composition_editors.hpp.
    connect(addSolidAction, &QAction::triggered, this,
            [this] { (void)addDefaultSolidLayer(session_); });
    connect(addTextAction, &QAction::triggered, this,
            [this] { (void)addDefaultTextLayer(session_); });
    connect(undoButton_, &QToolButton::clicked, &session_, &CompositionSession::undo);
    connect(redoButton_, &QToolButton::clicked, &session_, &CompositionSession::redo);
    connect(playPauseButton_, &QToolButton::clicked, playback_, &PlaybackController::toggle);
    connect(playback_, &PlaybackController::stateChanged, this,
            &TimelineEditor::updatePlaybackButton);
    updatePlaybackButton(playback_->state());

    // Spacebar application shortcut (decision 4), gated against stealing Space from text-entry
    // focus using the SAME idiom main_window.cpp's own window-level shortcuts use (QAction +
    // setShortcutContext(Qt::WindowShortcut), e.g. undoAction_/redoAction_ in createMenus()):
    // Qt::WindowShortcut fires whenever this widget's top-level window is active, independent of
    // which descendant currently holds focus, EXCEPT that a focused text-entry widget (QLineEdit/
    // QTextEdit and friends) accepts the ShortcutOverride event for an ordinary printable key like
    // Space itself first, so the widget's own text-input handling wins over this action's shortcut
    // whenever a text field has focus -- the standard Qt mechanism for exactly this gating, not a
    // bespoke focus check.
    auto* playPauseAction = new QAction(tr("Play/Pause"), this);
    playPauseAction->setObjectName("playPauseAction");
    playPauseAction->setShortcut(QKeySequence(Qt::Key_Space));
    playPauseAction->setShortcutContext(Qt::WindowShortcut);
    addAction(playPauseAction);
    connect(playPauseAction, &QAction::triggered, playback_, &PlaybackController::toggle);

    // Frame-stepping shortcuts (issue #108, decisions 1/2), mirroring playPauseAction's own
    // WindowShortcut idiom exactly (same setObjectName/setShortcut/setShortcutContext/addAction
    // shape, same reliance on Qt's ShortcutOverride mechanism to let a focused text-entry widget
    // win over these shortcuts without any bespoke check here). This relied on PropertiesEditor's
    // Position X/Y editors being QDoubleSpinBox, whose embedded QLineEdit accepts ShortcutOverride
    // for Left/Right/Home/End. Issue #120 (task U5) replaced those editors with
    // kit::KValueField, which has no line edit and does NOT accept ShortcutOverride for those
    // keys -- so Left/Right/Home/End typed while a Position field has focus now ALSO fires these
    // WindowShortcut actions, unlike before. Task U5's own fence forbids touching TimelineEditor
    // beyond what shared code forces, so this is flagged in that task's raw report rather than
    // fixed here; the fix belongs to whoever next owns either kit::KValueField's key handling or
    // this focusChanged reconciliation below.
    stepBackwardAction_ = new QAction(tr("Step Back One Frame"), this);
    stepBackwardAction_->setObjectName("stepBackwardAction");
    stepBackwardAction_->setShortcut(QKeySequence(Qt::Key_Left));
    stepBackwardAction_->setShortcutContext(Qt::WindowShortcut);
    addAction(stepBackwardAction_);
    connect(stepBackwardAction_, &QAction::triggered, this, [this] { stepFrame(-1); });
    // Task U7 (issue #122), decision 5: the visible stepBackButton_ triggers this SAME action --
    // one behavior, two entry points (keyboard shortcut, mouse click) -- rather than a parallel
    // stepFrame(-1) call site.
    connect(stepBackButton_, &QToolButton::clicked, stepBackwardAction_, &QAction::trigger);

    stepForwardAction_ = new QAction(tr("Step Forward One Frame"), this);
    stepForwardAction_->setObjectName("stepForwardAction");
    stepForwardAction_->setShortcut(QKeySequence(Qt::Key_Right));
    stepForwardAction_->setShortcutContext(Qt::WindowShortcut);
    addAction(stepForwardAction_);
    connect(stepForwardAction_, &QAction::triggered, this, [this] { stepFrame(1); });
    connect(stepForwardButton_, &QToolButton::clicked, stepForwardAction_, &QAction::trigger);

    stepToStartAction_ = new QAction(tr("Go To Start"), this);
    stepToStartAction_->setObjectName("stepToStartAction");
    stepToStartAction_->setShortcut(QKeySequence(Qt::Key_Home));
    stepToStartAction_->setShortcutContext(Qt::WindowShortcut);
    addAction(stepToStartAction_);
    connect(stepToStartAction_, &QAction::triggered, this, &TimelineEditor::stepToStart);

    stepToEndAction_ = new QAction(tr("Go To End"), this);
    stepToEndAction_->setObjectName("stepToEndAction");
    stepToEndAction_->setShortcut(QKeySequence(Qt::Key_End));
    stepToEndAction_->setShortcutContext(Qt::WindowShortcut);
    addAction(stepToEndAction_);
    connect(stepToEndAction_, &QAction::triggered, this, &TimelineEditor::stepToEnd);

    // Arrow-key conflict finding (this task's own investigation, verified with a standalone Qt
    // harness before writing this code): layers_ (QTreeWidget) already consumes Left/Right/Home/End
    // itself -- Left/Right collapse/expand the current item, Home/End jump to the first/last item
    // -- but, unlike QLineEdit/QDoubleSpinBox, QAbstractItemView does NOT accept the
    // ShortcutOverride event for those keys. That means a WindowShortcut action bound to the same
    // key silently wins over the tree's OWN keyPressEvent()-based navigation the instant it exists:
    // the harness showed the tree's current item and expand state never change at all once a
    // same-key action is installed, with no error -- a genuine behavior change this task must not
    // ship silently. The reconciliation rule (frozen by this task): widget-focus wins; the step
    // action fires otherwise. Implemented by disabling these four actions outright while layers_
    // holds keyboard focus -- a disabled QAction never claims ShortcutOverride, so the key event
    // reaches layers_ and its native navigation runs completely unchanged; every other focus
    // location (including no focus, the ruler, and the keyframe panel) leaves the actions enabled
    // and the step fires. That "every other focus location" list no longer includes
    // PropertiesEditor's Position X/Y fields without a side effect -- see the stepBackwardAction_
    // construction comment above (issue #120, task U5): kit::KValueField does not win
    // Left/Right/Home/End via ShortcutOverride the way the QDoubleSpinBox it replaced did.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        const bool layersFocused = (now == layers_);
        stepBackwardAction_->setEnabled(!layersFocused);
        stepForwardAction_->setEnabled(!layersFocused);
        stepToStartAction_->setEnabled(!layersFocused);
        stepToEndAction_->setEnabled(!layersFocused);
        // The visible stepBackButton_/stepForwardButton_ mirror their action's enabled state
        // exactly (task U7, issue #122, decision 5), so the same reconciliation this comment
        // block already documents for the keyboard shortcuts is visible on the mouse affordance
        // too rather than showing a clickable button that would silently do nothing.
        stepBackButton_->setEnabled(!layersFocused);
        stepForwardButton_->setEnabled(!layersFocused);
    });

    connect(layers_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (rebuilding_) {
            return;
        }
        const auto selected = layers_->selectedItems();
        if (selected.empty()) {
            session_.clearSelection();
            return;
        }
        session_.selectLayer(document::LayerId::fromRaw(
            selected.front()->data(0, kTimelineLayerIdRole).toULongLong()));
    });
    connect(&session_, &CompositionSession::snapshotChanged, this, &TimelineEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &TimelineEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &TimelineEditor::updateSelection);
    connect(&session_, &CompositionSession::historyChanged, this,
            &TimelineEditor::updateHistoryActions);
    // Readout updates on every session-time change and on a composition switch (which resets
    // session time to exact zero -- docs/architecture/animation-and-time.md, "Session Time And
    // Scrubbing": "Switching compositions resets the session time to exact zero in version 1"), so
    // the label always reflects the SAME time compositionChanged's reset already produced rather
    // than momentarily showing the previous composition's stale frame/time.
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &TimelineEditor::updateTimeReadout);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &TimelineEditor::updateTimeReadout);

    rebuild();
    updateHistoryActions();
    updateTimeReadout();
}

void TimelineEditor::rebuild() {
    rebuilding_ = true;
    layers_->clear();
    const auto* composition = session_.composition();
    if (composition != nullptr) {
        for (const auto& entry : composition->graph().layerStack().entries()) {
            const QString name = layerName(*composition, entry.layerId);
            const QString kind = layerKind(session_, entry.layerId);

            // kNameColumn/kKindColumn stay exactly the pinned text() contract
            // (composition_projection_test.cpp, playback_controller_tests.cpp); the row's OTHER
            // columns are new (task U7, issue #122, decision 1) and carry no text of their own
            // (icon+tooltip or an embedded item widget instead), so this constructor still only
            // ever receives two strings.
            auto* row = new QTreeWidgetItem(layers_, {name, kind});
            row->setFont(kNameColumn, kit::font(kit::TypeRole::Ui));
            row->setForeground(kNameColumn, QBrush(kit::color(kit::Color::Foreground)));
            row->setFont(kKindColumn, kit::font(kit::TypeRole::Ui));
            row->setForeground(kKindColumn, QBrush(kit::color(kit::Color::Muted)));
            row->setData(0, kTimelineLayerIdRole,
                         QVariant::fromValue<qulonglong>(entry.layerId.value()));
            row->setData(0, kTimelineSlotIdRole,
                         QVariant::fromValue<qulonglong>(entry.slotId.value()));
            row->setToolTip(
                kNameColumn,
                tr("Layer %1 · Slot %2").arg(entry.layerId.value()).arg(entry.slotId.value()));

            // Visibility (decision 1): NO user-facing visibility command exists anywhere in
            // src/commands today (verified by reading src/commands/include/bloom/commands/
            // operations.hpp -- AddSolidLayer, AddTextLayer, SetParameterSource, MoveLayerBefore,
            // and the SetProjectName/SetCompositionName/SetCompositionDuration/
            // SetCompositionFormat document-settings ops are the complete list; no per-layer
            // visibility flag exists in the document model at all, layer_graph_model.md included).
            // The honesty rule therefore applies verbatim: a permanently dimmed, non-interactive
            // icon with a tooltip that says exactly why, never a toggle that would silently do
            // nothing. Reported in this task's raw report.
            row->setIcon(kVisibilityColumn,
                         QIcon(kit::iconPixmap(kit::IconId::Visible, kit::Size::IconSmall,
                                               kit::withOpacity(kit::color(kit::Color::Muted),
                                                                kit::kDisabledOpacity))));
            row->setToolTip(kVisibilityColumn,
                            tr("Layer visibility has no command yet -- every layer renders"));

            // Reserved audio-mute/solo/lock columns (decision 1) are deliberately OMITTED, not
            // rendered as disabled ghost icons: none of the three has ANY model presence today
            // (no audio-layer kind, no solo concept, no lock field anywhere in src/document), so
            // three permanently-inert icons crammed into this already-dense 34px row would read as
            // broken chrome rather than an honest "reserved" placeholder. Justification recorded in
            // this task's raw report per decision 1's own "omitted entirely... YOUR call,
            // justified" allowance.

            // Blending / Parent (decision 1): one always-disabled dropdown per row, each carrying
            // its single honest value.
            layers_->setItemWidget(row, kBlendingColumn,
                                   makeDisabledPlaceholderDropdown(
                                       tr("Normal"), tr("Blend modes are not implemented yet"),
                                       QStringLiteral("layerBlendingDropdown"), layers_));
            layers_->setItemWidget(row, kParentColumn,
                                   makeDisabledPlaceholderDropdown(
                                       tr("None"), tr("Layer parenting does not exist yet"),
                                       QStringLiteral("layerParentDropdown"), layers_));

            // Lane bar (decision 2): one type-color-coded bar per row, spanning this column's full
            // width (see TimelineLaneBar's own honesty disclosure comment above).
            layers_->setItemWidget(row, kLaneColumn,
                                   new TimelineLaneBar(layerLaneColorToken(), layers_));
        }
    }
    updateSelection();
    rebuilding_ = false;
}

void TimelineEditor::updateSelection() {
    const QSignalBlocker blocker(layers_);
    layers_->clearSelection();
    const auto* layerId = std::get_if<document::LayerId>(&session_.selection().primary);
    if (layerId == nullptr && session_.selection().contextualLayer.has_value()) {
        layerId = &*session_.selection().contextualLayer;
    }
    if (layerId == nullptr) {
        return;
    }
    for (int index = 0; index < layers_->topLevelItemCount(); ++index) {
        auto* row = layers_->topLevelItem(index);
        if (row->data(0, kTimelineLayerIdRole).toULongLong() == layerId->value()) {
            row->setSelected(true);
            layers_->scrollToItem(row);
            break;
        }
    }
}

void TimelineEditor::updateHistoryActions() {
    undoButton_->setEnabled(session_.canUndo());
    redoButton_->setEnabled(session_.canRedo());
    const QString undoLabel = session_.undoLabel();
    const QString redoLabel = session_.redoLabel();
    undoButton_->setToolTip(undoLabel.isEmpty() ? tr("Nothing to undo")
                                                : tr("Undo %1").arg(undoLabel));
    redoButton_->setToolTip(redoLabel.isEmpty() ? tr("Nothing to redo")
                                                : tr("Redo %1").arg(redoLabel));
}

void TimelineEditor::updatePlaybackButton(const PlaybackState state) {
    const bool playing = state == PlaybackState::Playing;
    playPauseButton_->setChecked(playing);
    // text()/isChecked() stay the pinned test contract verbatim (playback_controller_tests.cpp);
    // the icon swap (task U7, issue #122, decision 5: "Play/Pause swap") is purely additive.
    playPauseButton_->setText(playing ? tr("Pause") : tr("Play"));
    playPauseButton_->setIcon(
        kit::icon(playing ? kit::IconId::Pause : kit::IconId::Play, kit::Size::IconMedium));
    playPauseButton_->setIconSize(
        QSize(kit::px(kit::Size::IconMedium), kit::px(kit::Size::IconMedium)));
    playPauseButton_->setToolTip(playing ? tr("Pause playback (Space)")
                                         : tr("Play from the current time (Space)"));
}

void TimelineEditor::stepFrame(const int delta) {
    const auto context = frameContextFor(session_);
    if (!context.has_value()) {
        return;
    }
    const auto nearest =
        nearestFrameIndexForTime(context->frameRate, context->duration, session_.currentTime());
    if (!nearest.has_value()) {
        return;
    }
    // Stepping while playing pauses playback FIRST through PlaybackController's own public
    // transport API (design decision 1) -- composing with pause() explicitly here rather than
    // relying on handleCurrentTimeChanged()'s existing "any external setCurrentTime() while playing
    // pauses" side effect (playback_controller.cpp), so this call site is honest about what it does
    // and the transport state change is never a coincidental side effect of the time write below.
    // Called unconditionally (idempotent no-op if already Stopped -- PlaybackController::pause()'s
    // own documented guard), not only when the step actually moves the playhead, matching the
    // decision's "stepping... pauses playback FIRST" without making pausing conditional on the
    // clamp outcome.
    playback_->pause();

    // Left/Right move exactly one frame index from the nearest index to the CURRENT (possibly
    // subframe) time, clamped to [0, maxFrameIndex] (design decision 1). nearestFrameIndex()'s own
    // tie rule (bloom::core::FrameTimeMapping::nearestFrameIndex(), and docs/architecture/
    // animation-and-time.md's "Session Time And Scrubbing": "selects the nearest index with an
    // exact halfway tie going to the greater index") decides which frame a subframe time steps
    // from, not this call site.
    std::uint64_t target = *nearest;
    if (delta < 0) {
        target = target > 0 ? target - 1 : 0;
    } else {
        target = target < context->maxFrameIndexValue ? target + 1 : context->maxFrameIndexValue;
    }
    const auto targetTime = frameTimeForIndex(context->frameRate, context->duration, target);
    if (targetTime.has_value()) {
        // A clamped step that lands back on the CURRENT exact time (e.g. Left at frame 0) is a
        // true no-op through CompositionSession::setCurrentTime()'s own early-return-on-equal-time
        // guard (composition_session.cpp) -- no currentTimeChanged signal churn, verified by that
        // mutator's own implementation rather than re-checked here.
        (void)session_.setCurrentTime(*targetTime);
    }
}

void TimelineEditor::stepToStart() {
    const auto context = frameContextFor(session_);
    if (!context.has_value()) {
        return;
    }
    playback_->pause();
    const auto targetTime = frameTimeForIndex(context->frameRate, context->duration, 0);
    if (targetTime.has_value()) {
        (void)session_.setCurrentTime(*targetTime);
    }
}

void TimelineEditor::stepToEnd() {
    const auto context = frameContextFor(session_);
    if (!context.has_value()) {
        return;
    }
    playback_->pause();
    const auto targetTime =
        frameTimeForIndex(context->frameRate, context->duration, context->maxFrameIndexValue);
    if (targetTime.has_value()) {
        (void)session_.setCurrentTime(*targetTime);
    }
}

void TimelineEditor::updateTimeReadout() {
    const auto time = session_.currentTime();
    const auto context = frameContextFor(session_);
    QString frameText = QStringLiteral("—");
    if (context.has_value()) {
        const auto nearest = nearestFrameIndexForTime(context->frameRate, context->duration, time);
        if (nearest.has_value()) {
            frameText = QString::number(*nearest);
        }
    }
    timeReadout_->setText(tr("Frame %1 · %2").arg(frameText, formatExactSeconds(time)));
}

} // namespace bloom::ui
