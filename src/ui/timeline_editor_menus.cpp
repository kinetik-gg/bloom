#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/timeline_editor.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QAction>
#include <QActionGroup>
#include <QHBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace bloom::ui {
namespace {

std::set<document::NodeId> selectedLayerNodes(const CompositionSession& session) {
    std::set<document::NodeId> nodes;
    for (const auto node : session.selectedNodes()) {
        const auto layer = session.layerForNode(node);
        if (layer.has_value()) {
            const auto boundary = session.boundaryNodeForLayer(*layer);
            if (boundary.has_value()) {
                nodes.insert(*boundary);
            }
        }
    }
    if (nodes.empty() && session.selection().contextualLayer.has_value()) {
        const auto boundary = session.boundaryNodeForLayer(*session.selection().contextualLayer);
        if (boundary.has_value()) {
            nodes.insert(*boundary);
        }
    }
    return nodes;
}

} // namespace

void TimelineEditor::createHeaderMenus() {
    auto* bar = &chrome_.header;
    bar->owner = this;
    bar->objectName = "timelineHeaderMenus";
    bar->overflowButtonName = "timelineHeaderOverflowButton";
    bar->overflowMenuName = "timelineHeaderOverflowMenu";
    const auto menu = [this](const QString& title, const QString& name) {
        auto* result = kit::makeMenu(title, this);
        result->setObjectName(name);
        return result;
    };
    const auto localAction = [this](QMenu* parent, const QString& title, const QString& name,
                                    const QKeySequence& shortcut, auto callback) {
        auto* action = parent->addAction(title);
        action->setObjectName(name);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        // One QAction, associated with both disjoint focus trees after header transfer. It has
        // one shortcut registration, so standalone and hosted panels cannot double-trigger it.
        addAction(action);
        connect(action, &QAction::triggered, this, callback);
        return action;
    };

    auto* add = menu(tr("Add"), QStringLiteral("addLayerMenu"));
    add->setAccessibleName(tr("Add layer menu"));
    addButton_ = bar->addMenu(add, QStringLiteral("addLayerButton"));
    addButton_->setToolTip(tr("Add a structured layer"));
    auto* solid = localAction(add, tr("Solid"), QStringLiteral("addSolidLayerAction"), {},
                              [this] { (void)addDefaultSolidLayer(session_); });
    solid->setToolTip(tr("Add a solid using the next built-in reference-linear-sRGB proof color"));
    auto* text = localAction(add, tr("Text"), QStringLiteral("addTextLayerAction"), {},
                             [this] { (void)addDefaultTextLayer(session_); });
    text->setToolTip(tr("Add a text layer"));
    for (std::int64_t index = 0; index <= 6; ++index) {
        const auto kind = static_cast<document::ShapeKind>(index);
        auto* shapeAction =
            localAction(add, QString::fromUtf8(document::shapeKindName(kind).data()),
                        QStringLiteral("timelineAddShape.%1").arg(index), {},
                        [this, kind] { (void)addDefaultShapeLayer(session_, kind); });
        shapeAction->setToolTip(
            tr("Add a %1 layer").arg(QString::fromUtf8(document::shapeKindName(kind).data())));
    }

    auto* view = menu(tr("View"), QStringLiteral("timelineViewMenu"));
    const auto toggle = [&](const QString& title, const QString& name, auto callback) {
        auto* action = localAction(view, title, name, {}, [] {});
        action->setCheckable(true);
        connect(action, &QAction::toggled, this, callback);
        return action;
    };
    keyframesAction_ = toggle(tr("Keyframes"), "timelineKeyframesAction",
                              [this](bool checked) { setKeyframesVisible(checked); });
    graphAction_ = toggle(tr("Graph Editor"), "timelineGraphEditorAction",
                          [this](bool checked) { setGraphEditorEnabled(checked); });
    snapAction_ = toggle(tr("Snap to Frames"), "timelineSnappingAction",
                         [this](bool checked) { setSnappingEnabled(checked); });
    view->addSeparator();
    localAction(view, tr("Zoom to Fit"), QStringLiteral("timelineZoomToFitAction"),
                QKeySequence(Qt::CTRL | Qt::Key_0), [this] { ruler_->zoomToFit(); });
    localAction(
        view, tr("Zoom In"), QStringLiteral("timelineZoomInAction"), QKeySequence::ZoomIn,
        [this] { ruler_->zoomBy(1.25, (ruler_->width() - kit::px(kit::Size::Hairline)) / 2.0); });
    localAction(
        view, tr("Zoom Out"), QStringLiteral("timelineZoomOutAction"), QKeySequence::ZoomOut,
        [this] { ruler_->zoomBy(0.8, (ruler_->width() - kit::px(kit::Size::Hairline)) / 2.0); });
    view->addSeparator();
    auto* formats = new QActionGroup(view);
    formats->setExclusive(true);
    framesAction_ = localAction(view, tr("Frames"), QStringLiteral("timelineFramesAction"), {},
                                [this] { setTimecodeFormat(false); });
    timecodeAction_ = localAction(view, tr("Timecode"), QStringLiteral("timelineTimecodeAction"),
                                  {}, [this] { setTimecodeFormat(true); });
    framesAction_->setCheckable(true);
    timecodeAction_->setCheckable(true);
    formats->addAction(framesAction_);
    formats->addAction(timecodeAction_);
    timecodeAction_->setToolTip(tr("Non-drop HH:MM:SS:FF at the nominal frame rate"));
    timecodeFormat_ = QSettings()
                          .value(QStringLiteral("timeline/time-format"), QStringLiteral("frames"))
                          .toString() == QStringLiteral("timecode");
    framesAction_->setChecked(!timecodeFormat_);
    timecodeAction_->setChecked(timecodeFormat_);
    bar->addMenu(view, QStringLiteral("timelineViewButton"));

    editMenu_ = menu(tr("Edit"), QStringLiteral("timelineEditMenu"));
    // Standalone editor fixtures have no application menu. These local menu actions keep that
    // embedding useful. In the application, refreshHeaderMenus inserts the actual shared actions.
    undoAction_ = editMenu_->addAction(tr("Undo"));
    undoAction_->setObjectName("timelineUndoAction");
    undoAction_->setShortcut(QKeySequence::Undo);
    undoAction_->setShortcutContext(Qt::WidgetShortcut);
    connect(undoAction_, &QAction::triggered, &session_, &CompositionSession::undo);
    redoAction_ = editMenu_->addAction(tr("Redo"));
    redoAction_->setObjectName("timelineRedoAction");
    redoAction_->setShortcut(QKeySequence::Redo);
    redoAction_->setShortcutContext(Qt::WidgetShortcut);
    connect(redoAction_, &QAction::triggered, &session_, &CompositionSession::redo);
    deleteLayerAction_ = editMenu_->addAction(tr("Delete Layer"));
    deleteLayerAction_->setObjectName("timelineDeleteLayerAction");
    deleteLayerAction_->setShortcuts(
        {QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    deleteLayerAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(deleteLayerAction_);
    connect(deleteLayerAction_, &QAction::triggered, this, &TimelineEditor::deleteSelectedLayers);
    splitLayerAction_ = localAction(
        editMenu_, tr("Split at Playhead"), QStringLiteral("timelineSplitLayerAction"),
        QKeySequence(Qt::CTRL | Qt::Key_K), [this] {
            commands::Transaction transaction("Split at Playhead", session_.snapshot().revision());
            for (const auto node : selectedLayerNodes(session_))
                if (const auto layer = session_.layerForNode(node))
                    transaction.emplace<commands::SplitLayerAtTime>(session_.compositionId(),
                                                                    *layer, session_.currentTime());
            (void)session_.executeTransaction(std::move(transaction));
        });
    bar->addMenu(editMenu_, QStringLiteral("timelineEditButton"), false);

    auto* select = menu(tr("Select"), QStringLiteral("timelineSelectMenu"));
    localAction(select, tr("All"), QStringLiteral("timelineSelectAllAction"),
                QKeySequence::SelectAll, [this] { selectAllLayers(); });
    localAction(select, tr("None"), QStringLiteral("timelineSelectNoneAction"),
                QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A),
                [this] { session_.clearSelection(); });
    bar->addMenu(select, QStringLiteral("timelineSelectButton"));
    connect(editMenu_, &QMenu::aboutToShow, this, &TimelineEditor::refreshHeaderMenus);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &TimelineEditor::refreshHeaderMenus);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &TimelineEditor::refreshHeaderMenus);
    connect(&session_, &CompositionSession::snapshotChanged, this,
            &TimelineEditor::refreshHeaderMenus);
}

void TimelineEditor::createFooter() {
    EditorChromeRowSpec layerControls;
    layerControls.objectName = "timelineLayerFooter";
    const auto actionButton = [&](QAction* action, kit::IconId icon, const QString& name) {
        auto* button = new kit::KIconButton(this);
        button->setObjectName(name);
        button->setDefaultAction(action);
        action->setIcon(kit::icon(icon, kit::IconRole::Chrome));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setToolTip(action->text());
        button->setAccessibleName(action->text());
        layerControls.addWidget(button);
    };
    actionButton(splitLayerAction_, kit::IconId::SplitHorizontal, "timelineFooterSplitLayer");
    actionButton(deleteLayerAction_, kit::IconId::Delete, "timelineFooterDeleteLayer");
    auto* left = EditorArea::buildChromeRow(layerControls, this, true);
    EditorChromeRowSpec laneControls;
    laneControls.objectName = "timelineLaneFooter";
    auto* zoom = new kit::KSlider(this);
    zoom->setObjectName("timelineZoomSlider");
    zoom->setAccessibleName(tr("Horizontal timeline zoom"));
    zoom->setToolTip(tr("Horizontal zoom: full composition to individual frames"));
    zoom->setFixedWidth(kit::px(kit::Size::DropdownWidth));
    zoom->setRange(0, 1);
    laneControls.addWidget(zoom);
    connect(zoom, &kit::KSlider::valueChanged, this, [this](double value) {
        const auto axis = ruler_->axisForWidth(ruler_->width());
        if (!axis)
            return;
        const double total = axis->duration.toSeconds();
        const double frame =
            static_cast<double>(axis->frameRate.denominator()) / axis->frameRate.numerator();
        const double maximum = std::max(1.0, total / frame);
        const double span = total / std::pow(maximum, value);
        ruler_->zoomBy((axis->t1 - axis->t0) / span, ruler_->width() / 2.0);
    });
    const auto refreshZoom = [this, zoom] {
        const auto axis = ruler_->axisForWidth(ruler_->width());
        if (!axis)
            return;
        const double total = axis->duration.toSeconds();
        const double maximum =
            std::max(1.0, total * axis->frameRate.numerator() / axis->frameRate.denominator());
        const QSignalBlocker blocker(zoom);
        zoom->setValue(maximum > 1 ? std::log(total / (axis->t1 - axis->t0)) / std::log(maximum)
                                   : 0);
    };
    connect(ruler_, &TimelineRuler::axisChanged, zoom, refreshZoom);
    refreshZoom();
    laneControls.addExpandingWidget(new TimelineNavigator(*ruler_, this));
    auto* right = EditorArea::buildChromeRow(laneControls, this, true);
    chrome_.footerCanvas = EditorArea::buildSplitChrome(left, right, layerColumnWidth_, this);
    chrome_.footerCanvas->setObjectName("timelineFooter");
    chrome_.footerCanvas->setFixedHeight(kit::px(kit::Size::FooterRow));
    footerLeft_ = left->parentWidget();
    footerLeft_->setObjectName("timelineFooterLeftSplit");
}

void TimelineEditor::refreshHeaderMenus() {
    // Workspace construction can precede parenting under MainWindow. Resolve on show and before
    // opening Edit, when window() is authoritative, instead of caching an early standalone
    // fallback.
    auto* undo = window()->findChild<QAction*>(QStringLiteral("undoAction"));
    auto* redo = window()->findChild<QAction*>(QStringLiteral("redoAction"));
    for (auto* action : editMenu_->actions()) {
        editMenu_->removeAction(action);
    }
    editMenu_->addAction(undo != nullptr ? undo : undoAction_);
    editMenu_->addAction(redo != nullptr ? redo : redoAction_);
    editMenu_->addAction(deleteLayerAction_);
    editMenu_->addAction(splitLayerAction_);
    splitLayerAction_->setEnabled(!selectedLayerNodes(session_).empty());
    deleteLayerAction_->setEnabled(!selectedLayerNodes(session_).empty());
    const bool available = session_.composition() != nullptr;
    addButton_->setEnabled(available);
    for (const auto* name :
         {"timelineZoomToFitAction", "timelineZoomInAction", "timelineZoomOutAction",
          "timelineSelectAllAction", "timelineSelectNoneAction"}) {
        // These actions belong to the transferred header, so search through the persistent menu
        // parent instead of the editor's body-only QObject tree.
        if (auto* action = editMenu_->parent()->findChild<QAction*>(QLatin1String(name))) {
            action->setEnabled(available);
        }
    }
}

void TimelineEditor::updateHistoryActions() {
    undoAction_->setEnabled(session_.canUndo());
    redoAction_->setEnabled(session_.canRedo());
    undoAction_->setText(session_.undoLabel().isEmpty() ? tr("Undo")
                                                        : tr("Undo %1").arg(session_.undoLabel()));
    redoAction_->setText(session_.redoLabel().isEmpty() ? tr("Redo")
                                                        : tr("Redo %1").arg(session_.redoLabel()));
}

void TimelineEditor::selectAllLayers() {
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    std::set<document::NodeId> nodes;
    for (const auto& entry : composition->graph().layerStack().entries()) {
        const auto boundary = session_.boundaryNodeForLayer(entry.layerId);
        if (boundary.has_value()) {
            nodes.insert(*boundary);
        }
    }
    if (nodes.empty()) {
        session_.clearSelection();
    } else {
        session_.selectNodes(nodes, *nodes.begin());
    }
}

void TimelineEditor::deleteSelectedLayers() {
    auto nodes = selectedLayerNodes(session_);
    if (nodes.empty()) {
        return;
    }
    commands::Transaction transaction("Delete Layer", session_.snapshot().revision());
    transaction.emplace<commands::RemoveNodes>(session_.compositionId(), std::move(nodes));
    (void)session_.executeNodeTransaction(std::move(transaction));
}

void TimelineEditor::setTimecodeFormat(const bool timecode) {
    timecodeFormat_ = timecode;
    framesAction_->setChecked(!timecode);
    timecodeAction_->setChecked(timecode);
    QSettings().setValue(QStringLiteral("timeline/time-format"),
                         timecode ? QStringLiteral("timecode") : QStringLiteral("frames"));
    ruler_->setTimecodeLabels(timecode);
}

} // namespace bloom::ui
