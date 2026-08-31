#include <bloom/ui/composition_editors.hpp>

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace bloom::ui {
namespace {

constexpr int kTimelineLayerIdRole = Qt::UserRole + 1;
constexpr int kTimelineSlotIdRole = Qt::UserRole + 2;
constexpr int kCompositionIdRole = Qt::UserRole + 1;
constexpr std::array kDefaultSolidPalette{
    core::Color4d{0.62, 0.08, 0.04, 1.0},
    core::Color4d{0.04, 0.20, 0.72, 1.0},
    core::Color4d{0.06, 0.52, 0.16, 1.0},
    core::Color4d{0.46, 0.07, 0.58, 1.0},
};

const document::LayerOutputBoundary* layerBoundary(const document::Composition& composition,
                                                   const document::LayerId layerId) {
    const auto boundaries = composition.graph().layerOutputs();
    const auto found = std::ranges::find_if(
        boundaries, [layerId](const auto& candidate) { return candidate.layerId == layerId; });
    return found == boundaries.end() ? nullptr : &*found;
}

const document::ParameterRecord* nodeParameter(const document::Composition& composition,
                                               const document::NodeId nodeId,
                                               const std::string_view role) {
    const auto* node = composition.graph().findNode(nodeId);
    if (node == nullptr) {
        return nullptr;
    }
    const auto binding = std::ranges::find_if(
        node->parameters, [role](const auto& candidate) { return candidate.role == role; });
    return binding == node->parameters.end() ? nullptr
                                             : composition.parameters().find(binding->parameterId);
}

QString sourceDescription(const document::ParameterRecord& parameter) {
    if (std::holds_alternative<document::AnimationCurveSource>(parameter.source)) {
        return QStringLiteral("Animated");
    }
    if (std::holds_alternative<document::DriverBindingSource>(parameter.source)) {
        return QStringLiteral("Driven by graph");
    }
    return QStringLiteral("Constant");
}

QString layerName(const document::Composition& composition, const document::LayerId layerId) {
    const auto* boundary = layerBoundary(composition, layerId);
    if (boundary == nullptr || boundary->name.empty()) {
        return QStringLiteral("Layer %1").arg(layerId.value());
    }
    return QString::fromStdString(boundary->name);
}

const document::NodeRecord* directSourceNode(const CompositionSession& session,
                                             const document::LayerId layerId) {
    const auto* composition = session.composition();
    const auto sourceNodeId = session.directSourceNodeForLayer(layerId);
    return composition != nullptr && sourceNodeId.has_value()
               ? composition->graph().findNode(*sourceNodeId)
               : nullptr;
}

bool isKnownSource(const document::NodeRecord* node, const std::string_view typeId,
                   const std::uint32_t schemaVersion) {
    return node != nullptr && node->typeId == typeId && node->schemaVersion == schemaVersion;
}

const document::NodeRecord* selectedPresentationSource(const CompositionSession& session) {
    if (const auto* layerId = std::get_if<document::LayerId>(&session.selection().primary)) {
        return directSourceNode(session, *layerId);
    }
    return session.selectedNode();
}

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

qulonglong nextLayerNumber(const CompositionSession& session, const std::string_view typeId,
                           const std::uint32_t schemaVersion) {
    qulonglong count = 0;
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return 1;
    }
    for (const auto& entry : composition->graph().layerStack().entries()) {
        const auto* sourceNode = directSourceNode(session, entry.layerId);
        if (isKnownSource(sourceNode, typeId, schemaVersion)) {
            ++count;
        }
    }
    return count + 1;
}

QString exactNumber(const double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::general);
    if (result.ec != std::errc{}) {
        return QString::number(value, 'g', std::numeric_limits<double>::max_digits10);
    }
    return QString::fromLatin1(buffer.data(), static_cast<qsizetype>(result.ptr - buffer.data()));
}

QString exactColor(const core::Color4d color) {
    return QStringLiteral("R %1  G %2  B %3  A %4")
        .arg(exactNumber(color.red), exactNumber(color.green), exactNumber(color.blue),
             exactNumber(color.alpha));
}

QString selectionName(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return QStringLiteral("No composition");
    }
    if (const auto* layerId = std::get_if<document::LayerId>(&session.selection().primary)) {
        return layerName(*composition, *layerId);
    }
    if (const auto* nodeId = std::get_if<document::NodeId>(&session.selection().primary)) {
        const auto* node = composition->graph().findNode(*nodeId);
        return node == nullptr ? QStringLiteral("Node unavailable")
                               : QString::fromStdString(node->typeId);
    }
    if (const auto* parameterId =
            std::get_if<document::ParameterId>(&session.selection().primary)) {
        const auto* parameter = composition->parameters().find(*parameterId);
        return parameter == nullptr ? QStringLiteral("Parameter unavailable")
                                    : QString::fromStdString(parameter->schemaKey);
    }
    return QStringLiteral("Nothing selected");
}

QToolButton* makeToolButton(const QString& text, const QString& accessibleName, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setAccessibleName(accessibleName);
    button->setAutoRaise(true);
    return button;
}

// Frame stepping / readout (issue #108): the frame rate, duration, and checked maximum frame index
// every stepFrame()/stepToStart()/stepToEnd()/updateTimeReadout() call needs, resolved the one
// place so they cannot silently drift from each other's notion of the composition's valid frame
// range -- mirroring mappingForComposition() in playback_controller.cpp, which this deliberately
// does NOT reuse (it is private to that translation unit and this task's fence forbids touching the
// playback controller beyond the smallest justified accessor -- see stepFrame()'s own comment on
// why none was needed). std::nullopt covers no live composition or a rate/duration
// bloom::core::FrameTimeMapping itself refuses, exactly like every other caller of
// timeline_frame_math.hpp's adapters.
struct TimelineFrameContext final {
    document::FrameRate frameRate;
    core::RationalTime duration;
    std::uint64_t maxFrameIndexValue;
};

[[nodiscard]] std::optional<TimelineFrameContext>
frameContextFor(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return std::nullopt;
    }
    const auto frameRate = composition->format().frameRate();
    const auto duration = composition->duration();
    const auto maxIndex = maxFrameIndex(frameRate, duration);
    if (!maxIndex.has_value()) {
        return std::nullopt;
    }
    return TimelineFrameContext{frameRate, duration, *maxIndex};
}

// Formats an exact RationalTime as seconds with EXACTLY 3 truncated decimal digits (millisecond
// resolution), computed purely from the integer numerator/denominator -- never through
// RationalTime::toSeconds()'s binary64 conversion -- so the digits shown are always the value's
// true leading digits, never a rounded/binary64-approximated one (design decision 3: "no
// floating-point accumulation... a subframe time must display honestly"). Three places is a
// deliberately BOUNDED cut of what can be an infinite decimal expansion (e.g. 1 s / 3 has no exact
// finite decimal form); truncating rather than rounding means the displayed digits never overstate
// the exact value. The widening multiply uses a 128-bit intermediate purely so an extreme
// duration's denominator cannot silently overflow a 64-bit product -- unreachable for any realistic
// composition, kept checked rather than UB regardless; this is ordinary display arithmetic, not
// part of the sampling contract's own "no compiler-specific extended integers" rule
// (docs/architecture/animation-and-time.md, "Sampling Semantics Version 1"), which governs curve
// evaluation only.
[[nodiscard]] QString formatExactSeconds(const core::RationalTime time) {
    constexpr int kDecimalPlaces = 3;
    constexpr unsigned __int128 kScale = 1000;
    const std::int64_t numerator = time.numerator();
    const auto denominator = static_cast<unsigned __int128>(time.denominator());
    const bool negative = numerator < 0;
    const auto magnitude = negative
                               ? static_cast<unsigned __int128>(-static_cast<__int128>(numerator))
                               : static_cast<unsigned __int128>(numerator);
    const auto wholeSeconds = static_cast<qulonglong>(magnitude / denominator);
    const auto remainder = magnitude % denominator;
    const auto scaledFraction = static_cast<qulonglong>((remainder * kScale) / denominator);
    return QStringLiteral("%1%2.%3s")
        .arg(negative ? QStringLiteral("-") : QString())
        .arg(wholeSeconds)
        .arg(scaledFraction, kDecimalPlaces, 10, QChar('0'));
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
    controlsLayout->setContentsMargins(8, 5, 8, 5);
    controlsLayout->setSpacing(4);

    auto* title = new QLabel(tr("Layers"), controls);
    title->setObjectName("editorSectionTitle");
    addButton_ = makeToolButton(tr("Add"), tr("Add layer"), controls);
    addButton_->setObjectName("addLayerButton");
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
    // Playback transport (issue #105, decision 4): a play/pause toggle beside the ruler, using the
    // same makeToolButton() idiom as Add/Undo/Redo. Owned per-panel (see the header's comment on
    // playback_) -- constructed here, directly beside the ruler it drives.
    playPauseButton_ = makeToolButton(tr("Play"), tr("Toggle playback"), controls);
    playPauseButton_->setObjectName("playPauseButton");
    playPauseButton_->setCheckable(true);
    // Current time readout (issue #108, decision 3): a small label beside the transport, matching
    // PropertiesEditor::selectionLabel_'s plain-QLabel idiom (setObjectName + selectable text, no
    // bespoke styling). Text is set by updateTimeReadout() below, not here.
    timeReadout_ = new QLabel(controls);
    timeReadout_->setObjectName("timelineTimeReadout");
    timeReadout_->setAccessibleName(tr("Current frame and time"));
    timeReadout_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    undoButton_ = makeToolButton(tr("Undo"), tr("Undo last edit"), controls);
    redoButton_ = makeToolButton(tr("Redo"), tr("Redo last edit"), controls);
    controlsLayout->addWidget(title);
    controlsLayout->addWidget(addButton_);
    controlsLayout->addWidget(playPauseButton_);
    controlsLayout->addWidget(timeReadout_);
    controlsLayout->addStretch(1);
    controlsLayout->addWidget(undoButton_);
    controlsLayout->addWidget(redoButton_);

    ruler_ = new TimelineRuler(session_, previewController, this);
    keyframes_ = new TimelineKeyframePanel(session_, this);
    playback_ = new PlaybackController(session_, previewController, &std::chrono::steady_clock::now,
                                       std::chrono::milliseconds{16}, this);

    layers_ = new QTreeWidget(this);
    layers_->setObjectName("layerStackView");
    layers_->setAccessibleName(tr("Composition layers"));
    layers_->setColumnCount(3);
    layers_->setHeaderLabels({tr("Name"), tr("Kind"), tr("Opacity")});
    layers_->setRootIsDecorated(false);
    layers_->setAlternatingRowColors(true);
    layers_->setSelectionMode(QAbstractItemView::SingleSelection);
    layers_->setUniformRowHeights(true);
    layers_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layers_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layers_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    layout->addWidget(controls);
    layout->addWidget(ruler_);
    layout->addWidget(keyframes_);
    layout->addWidget(layers_, 1);

    connect(addSolidAction, &QAction::triggered, this, [this] {
        const auto layerNumber = nextLayerNumber(session_, document::kSolidSourceNodeType,
                                                 document::kSolidSourceNodeSchemaVersion);
        const auto paletteIndex =
            static_cast<std::size_t>(layerNumber - 1) % kDefaultSolidPalette.size();
        (void)session_.addSolidLayer(tr("Solid %1").arg(layerNumber),
                                     kDefaultSolidPalette[paletteIndex]);
    });
    connect(addTextAction, &QAction::triggered, this, [this] {
        const auto layerNumber = nextLayerNumber(session_, document::kTextSourceNodeType,
                                                 document::kTextSourceNodeSchemaVersion);
        (void)session_.addTextLayer(tr("Text %1").arg(layerNumber), tr("Text"));
    });
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
    // shape, same reliance on Qt's ShortcutOverride mechanism to let a focused text-entry widget --
    // e.g. PropertiesEditor's QDoubleSpinBox editors, verified with a standalone Qt harness to
    // accept ShortcutOverride for Left/Right/Home/End exactly like QLineEdit does -- win over these
    // shortcuts without any bespoke check here).
    stepBackwardAction_ = new QAction(tr("Step Back One Frame"), this);
    stepBackwardAction_->setObjectName("stepBackwardAction");
    stepBackwardAction_->setShortcut(QKeySequence(Qt::Key_Left));
    stepBackwardAction_->setShortcutContext(Qt::WindowShortcut);
    addAction(stepBackwardAction_);
    connect(stepBackwardAction_, &QAction::triggered, this, [this] { stepFrame(-1); });

    stepForwardAction_ = new QAction(tr("Step Forward One Frame"), this);
    stepForwardAction_->setObjectName("stepForwardAction");
    stepForwardAction_->setShortcut(QKeySequence(Qt::Key_Right));
    stepForwardAction_->setShortcutContext(Qt::WindowShortcut);
    addAction(stepForwardAction_);
    connect(stepForwardAction_, &QAction::triggered, this, [this] { stepFrame(1); });

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
    // location (including no focus, the ruler, the keyframe panel, or a QDoubleSpinBox that itself
    // already wins via ShortcutOverride) leaves the actions enabled and the step fires.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        const bool layersFocused = (now == layers_);
        stepBackwardAction_->setEnabled(!layersFocused);
        stepForwardAction_->setEnabled(!layersFocused);
        stepToStartAction_->setEnabled(!layersFocused);
        stepToEndAction_->setEnabled(!layersFocused);
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
            const auto* boundary = layerBoundary(*composition, entry.layerId);
            const QString name = layerName(*composition, entry.layerId);
            const QString kind = layerKind(session_, entry.layerId);
            QString opacity = QStringLiteral("—");
            if (boundary != nullptr) {
                if (const auto* parameter = nodeParameter(*composition, boundary->nodeId,
                                                          document::kOpacityParameterRole)) {
                    const auto* constant =
                        std::get_if<document::ConstantValueSource>(&parameter->source);
                    if (constant != nullptr) {
                        if (const auto* value = std::get_if<double>(&constant->value)) {
                            opacity = QStringLiteral("%1%").arg(*value * 100.0, 0, 'f', 0);
                        }
                    } else {
                        opacity = sourceDescription(*parameter);
                    }
                }
            }

            auto* row = new QTreeWidgetItem(layers_, {name, kind, opacity});
            row->setData(0, kTimelineLayerIdRole,
                         QVariant::fromValue<qulonglong>(entry.layerId.value()));
            row->setData(0, kTimelineSlotIdRole,
                         QVariant::fromValue<qulonglong>(entry.slotId.value()));
            row->setToolTip(
                0, tr("Layer %1 · Slot %2").arg(entry.layerId.value()).arg(entry.slotId.value()));
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
    playPauseButton_->setText(playing ? tr("Pause") : tr("Play"));
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

PropertiesEditor::PropertiesEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("propertiesEditor");
    setAccessibleName(tr("Properties editor"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(10);

    selectionLabel_ = new QLabel(this);
    selectionLabel_->setObjectName("propertiesSelectionTitle");
    selectionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(selectionLabel_);

    auto* transformForm = new QFormLayout;
    transformForm->setContentsMargins(0, 0, 0, 0);
    transformForm->setHorizontalSpacing(10);
    transformForm->setVerticalSpacing(7);

    positionX_ = new QDoubleSpinBox(this);
    positionY_ = new QDoubleSpinBox(this);
    opacity_ = new QDoubleSpinBox(this);
    for (auto* editor : {positionX_, positionY_}) {
        editor->setRange(-1'000'000.0, 1'000'000.0);
        editor->setDecimals(2);
        editor->setSingleStep(1.0);
    }
    positionX_->setObjectName("positionXEditor");
    positionX_->setAccessibleName(tr("Position X"));
    positionY_->setObjectName("positionYEditor");
    positionY_->setAccessibleName(tr("Position Y"));
    opacity_->setObjectName("opacityEditor");
    opacity_->setAccessibleName(tr("Opacity"));
    opacity_->setRange(0.0, 100.0);
    opacity_->setDecimals(1);
    opacity_->setSingleStep(1.0);
    opacity_->setSuffix(QStringLiteral(" %"));

    auto* position = new QWidget(this);
    auto* positionLayout = new QHBoxLayout(position);
    positionLayout->setContentsMargins(0, 0, 0, 0);
    positionLayout->setSpacing(5);
    positionLayout->addWidget(new QLabel(QStringLiteral("X"), position));
    positionLayout->addWidget(positionX_);
    positionLayout->addWidget(new QLabel(QStringLiteral("Y"), position));
    positionLayout->addWidget(positionY_);

    transformForm->addRow(tr("Position"), position);
    transformForm->addRow(tr("Opacity"), opacity_);
    layout->addLayout(transformForm);

    solidColorPanel_ = new QWidget(this);
    solidColorPanel_->setObjectName("solidColorProperties");
    auto* solidColorLayout = new QVBoxLayout(solidColorPanel_);
    solidColorLayout->setContentsMargins(0, 4, 0, 0);
    solidColorLayout->setSpacing(7);
    auto* solidColorTitle = new QLabel(tr("Solid Source"), solidColorPanel_);
    solidColorTitle->setObjectName("editorSectionTitle");
    solidColorLayout->addWidget(solidColorTitle);

    auto* solidColorForm = new QFormLayout;
    solidColorForm->setContentsMargins(0, 0, 0, 0);
    solidColorForm->setHorizontalSpacing(10);
    solidColorForm->setVerticalSpacing(7);
    solidColorValue_ = new QLabel(solidColorPanel_);
    solidColorValue_->setObjectName("solidColorValue");
    solidColorValue_->setAccessibleName(tr("Solid RGBA value"));
    solidColorValue_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                              Qt::TextSelectableByKeyboard);
    solidColorValue_->setWordWrap(true);
    solidAlphaAssociation_ = new QLabel(solidColorPanel_);
    solidAlphaAssociation_->setObjectName("solidAlphaAssociation");
    solidAlphaAssociation_->setAccessibleName(tr("Solid alpha association"));
    solidAlphaAssociation_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                                    Qt::TextSelectableByKeyboard);
    solidColorEncoding_ = new QLabel(solidColorPanel_);
    solidColorEncoding_->setObjectName("solidColorEncoding");
    solidColorEncoding_->setAccessibleName(tr("Solid color encoding"));
    solidColorEncoding_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                                 Qt::TextSelectableByKeyboard);
    solidColorForm->addRow(tr("RGBA"), solidColorValue_);
    solidColorForm->addRow(tr("Alpha"), solidAlphaAssociation_);
    solidColorForm->addRow(tr("Encoding"), solidColorEncoding_);
    solidColorLayout->addLayout(solidColorForm);
    layout->addWidget(solidColorPanel_);
    layout->addStretch(1);

    const auto commitPosition = [this] {
        if (!rebuilding_) {
            (void)session_.setSelectedPosition(positionX_->value(), positionY_->value());
        }
    };
    connect(positionX_, &QDoubleSpinBox::editingFinished, this, commitPosition);
    connect(positionY_, &QDoubleSpinBox::editingFinished, this, commitPosition);
    connect(opacity_, &QDoubleSpinBox::editingFinished, this, [this] {
        if (!rebuilding_) {
            (void)session_.setSelectedOpacity(opacity_->value() / 100.0);
        }
    });
    connect(&session_, &CompositionSession::snapshotChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this, &PropertiesEditor::rebuild);

    rebuild();
}

void PropertiesEditor::rebuild() {
    rebuilding_ = true;
    selectionLabel_->setText(selectionName(session_));

    const auto* position = session_.parameterForSelection(document::kPositionParameterRole);
    const auto positionValue =
        position == nullptr ? std::nullopt : session_.constantVec2Value(position->id);
    const bool canEditPosition = positionValue.has_value();
    positionX_->setEnabled(canEditPosition);
    positionY_->setEnabled(canEditPosition);
    if (canEditPosition) {
        const QSignalBlocker blockX(positionX_);
        const QSignalBlocker blockY(positionY_);
        positionX_->setValue(positionValue->x);
        positionY_->setValue(positionValue->y);
    }
    const QString positionTip = position == nullptr
                                    ? tr("Position is not exposed by this selection")
                                    : sourceDescription(*position);
    positionX_->setToolTip(positionTip);
    positionY_->setToolTip(positionTip);

    configureOpacity();
    configureSolidColor();
    rebuilding_ = false;
}

void PropertiesEditor::configureOpacity() {
    const auto* parameter = session_.parameterForSelection(document::kOpacityParameterRole);
    const auto value = parameter == nullptr ? std::nullopt : session_.constantValue(parameter->id);
    opacity_->setEnabled(value.has_value());
    const QSignalBlocker blocker(opacity_);
    opacity_->setValue(value.has_value() ? *value * 100.0 : 100.0);
    opacity_->setToolTip(parameter == nullptr ? tr("Opacity is not exposed by this selection")
                                              : sourceDescription(*parameter));
}

void PropertiesEditor::configureSolidColor() {
    const auto* parameter = session_.parameterForSelection(document::kSolidColorParameterRole);
    const auto* sourceNode = selectedPresentationSource(session_);
    const bool isSolid = isKnownSource(sourceNode, document::kSolidSourceNodeType,
                                       document::kSolidSourceNodeSchemaVersion) &&
                         parameter != nullptr &&
                         parameter->schemaKey == document::kSolidColorParameterSchemaKey;
    solidColorPanel_->setVisible(isSolid);
    if (!isSolid) {
        return;
    }

    const auto value = session_.constantColorValue(parameter->id);
    solidColorValue_->setText(value.has_value() ? exactColor(*value)
                                                : sourceDescription(*parameter));
    solidColorValue_->setToolTip(
        tr("Straight scene-linear authoring values; negative and HDR RGB are not clipped"));
    solidAlphaAssociation_->setText(tr("Straight (unassociated)"));
    solidColorEncoding_->setText(
        QString::fromUtf8(document::kSolidColorEncoding.data(),
                          static_cast<qsizetype>(document::kSolidColorEncoding.size())));
}

MediaEditor::MediaEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("mediaEditor");
    setAccessibleName(tr("Project media editor"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(7);
    auto* title = new QLabel(tr("Project"), this);
    title->setObjectName("editorSectionTitle");
    compositions_ = new QListWidget(this);
    compositions_->setObjectName("compositionList");
    compositions_->setAccessibleName(tr("Project compositions"));
    compositions_->setAlternatingRowColors(true);
    layout->addWidget(title);
    layout->addWidget(compositions_, 1);

    connect(compositions_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current) {
                if (!rebuilding_ && current != nullptr) {
                    (void)session_.setComposition(document::CompositionId::fromRaw(
                        current->data(kCompositionIdRole).toULongLong()));
                }
            });
    connect(&session_, &CompositionSession::snapshotChanged, this, &MediaEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &MediaEditor::updateSelection);

    rebuild();
}

void MediaEditor::rebuild() {
    rebuilding_ = true;
    compositions_->clear();
    for (const auto& composition : session_.snapshot().project().compositions()) {
        auto* item = new QListWidgetItem(QString::fromStdString(composition.name()), compositions_);
        item->setData(kCompositionIdRole,
                      QVariant::fromValue<qulonglong>(composition.id().value()));
        item->setToolTip(tr("Composition %1").arg(composition.id().value()));
    }
    updateSelection();
    rebuilding_ = false;
}

void MediaEditor::updateSelection() {
    const QSignalBlocker blocker(compositions_);
    for (int index = 0; index < compositions_->count(); ++index) {
        auto* item = compositions_->item(index);
        if (item->data(kCompositionIdRole).toULongLong() == session_.compositionId().value()) {
            compositions_->setCurrentItem(item);
            return;
        }
    }
    compositions_->setCurrentItem(nullptr);
}

} // namespace bloom::ui
