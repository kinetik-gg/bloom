#include <bloom/ui/timeline_editor.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QAction>
#include <QActionGroup>
#include <QHBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QToolButton>

#include <algorithm>
#include <set>
#include <vector>

namespace bloom::ui {
namespace {

class TimelineCompositionName final : public QWidget {
  public:
    TimelineCompositionName(CompositionSession& session, QWidget* parent)
        : QWidget(parent), session_(session) {
        setObjectName("timelineCompositionName");
        setMinimumHeight(kit::px(kit::Size::Control));
        setFont(kit::font(kit::TypeRole::Ui));
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        connect(&session, &CompositionSession::snapshotChanged, this, [this] { refresh(); });
        connect(&session, &CompositionSession::compositionChanged, this, [this] { refresh(); });
        refresh();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setFont(font());
        painter.setPen(kit::color(kit::Color::Foreground));
        painter.drawText(rect(), Qt::AlignVCenter | Qt::AlignLeft,
                         fontMetrics().elidedText(name_, Qt::ElideRight, width()));
    }

  private:
    void refresh() {
        const auto* composition = session_.composition();
        name_ = composition != nullptr ? QString::fromStdString(composition->name())
                                       : tr("No composition");
        setToolTip(name_);
        setAccessibleName(name_);
        update();
    }
    CompositionSession& session_;
    QString name_;
};

class TimelineHeaderMenuBar final : public QWidget {
  public:
    explicit TimelineHeaderMenuBar(QWidget* parent) : QWidget(parent) {
        setObjectName("timelineHeaderMenus");
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(kit::px(kit::Spacing::XXS));
        setFixedHeight(kit::px(kit::Size::Control));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        overflow_ = new QToolButton(this);
        overflow_->setObjectName("timelineHeaderOverflowButton");
        overflow_->setText(QStringLiteral("…"));
        overflow_->setToolTip(tr("Timeline menus"));
        overflow_->setAccessibleName(overflow_->toolTip());
        configure(*overflow_);
        overflowMenu_ = new QMenu(this);
        overflowMenu_->setObjectName("timelineHeaderOverflowMenu");
        overflow_->setMenu(overflowMenu_);
        row->addWidget(overflow_);
        overflow_->hide();
    }

    QToolButton* addMenu(QMenu* menu, const QString& name) {
        auto* button = new QToolButton(this);
        button->setText(menu->title());
        button->setObjectName(name);
        button->setAccessibleName(menu->title());
        configure(*button);
        button->setMenu(menu);
        static_cast<QHBoxLayout*>(layout())->insertWidget(static_cast<int>(buttons_.size()),
                                                          button);
        buttons_.push_back(button);
        overflowMenu_->addMenu(menu);
        updateGeometry();
        return button;
    }

    QSize sizeHint() const override {
        int width = 0;
        for (auto* button : buttons_) {
            width += button->sizeHint().width() + layout()->spacing();
        }
        return {width, kit::px(kit::Size::Control)};
    }
    QSize minimumSizeHint() const override { return overflow_->sizeHint(); }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        const bool collapsed = width() < sizeHint().width();
        for (auto* button : buttons_) {
            button->setVisible(!collapsed);
        }
        overflow_->setVisible(collapsed);
    }

  private:
    static void configure(QToolButton& button) {
        button.setAutoRaise(true);
        button.setPopupMode(QToolButton::InstantPopup);
        button.setProperty("headerMenuButton", true);
        button.setFont(kit::font(kit::TypeRole::Ui));
        button.setFixedHeight(kit::px(kit::Size::Control));
    }
    std::vector<QToolButton*> buttons_;
    QToolButton* overflow_ = nullptr;
    QMenu* overflowMenu_ = nullptr;
};

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
    headerMenus_ = new QWidget(headerFallback_);
    auto* row = new QHBoxLayout(headerMenus_);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(kit::px(kit::Spacing::S));
    auto* bar = new TimelineHeaderMenuBar(headerMenus_);
    row->addWidget(bar);
    row->addWidget(new TimelineCompositionName(session_, headerMenus_), 1);

    const auto menu = [this](const QString& title, const QString& name) {
        auto* result = new QMenu(title, headerMenus_);
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
        headerMenus_->addAction(action);
        connect(action, &QAction::triggered, this, callback);
        return action;
    };

    auto* add = menu(tr("Add"), QStringLiteral("addLayerMenu"));
    add->setAccessibleName(tr("Add layer menu"));
    auto* solid = localAction(add, tr("Solid"), QStringLiteral("addSolidLayerAction"), {},
                              [this] { (void)addDefaultSolidLayer(session_); });
    solid->setToolTip(tr("Add a solid using the next built-in reference-linear-sRGB proof color"));
    auto* text = localAction(add, tr("Text"), QStringLiteral("addTextLayerAction"), {},
                             [this] { (void)addDefaultTextLayer(session_); });
    text->setToolTip(tr("Add a text layer"));
    addButton_ = bar->addMenu(add, QStringLiteral("addLayerButton"));
    addButton_->setToolTip(tr("Add a structured layer"));

    auto* view = menu(tr("View"), QStringLiteral("timelineViewMenu"));
    localAction(view, tr("Zoom to Fit"), QStringLiteral("timelineZoomToFitAction"),
                QKeySequence(Qt::CTRL | Qt::Key_0), [this] { ruler_->zoomToFit(); });
    localAction(view, tr("Zoom In"), QStringLiteral("timelineZoomInAction"), QKeySequence::ZoomIn,
                [this] { ruler_->zoomBy(1.25, (ruler_->width() - 1) / 2.0); });
    localAction(view, tr("Zoom Out"), QStringLiteral("timelineZoomOutAction"),
                QKeySequence::ZoomOut,
                [this] { ruler_->zoomBy(0.8, (ruler_->width() - 1) / 2.0); });
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
    headerMenus_->addAction(deleteLayerAction_);
    connect(deleteLayerAction_, &QAction::triggered, this, &TimelineEditor::deleteSelectedLayers);
    splitLayerAction_ = localAction(editMenu_, tr("Split at Playhead"), QStringLiteral("timelineSplitLayerAction"), QKeySequence(Qt::CTRL | Qt::Key_K), [this] {
        commands::Transaction transaction("Split at Playhead", session_.snapshot().revision());
        for (const auto node : selectedLayerNodes(session_))
            if (const auto layer = session_.layerForNode(node)) transaction.emplace<commands::SplitLayerAtTime>(session_.compositionId(), *layer, session_.currentTime());
        (void)session_.executeTransaction(std::move(transaction));
    });
    bar->addMenu(editMenu_, QStringLiteral("timelineEditButton"));

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

void TimelineEditor::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshHeaderMenus();
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
    updateTimeReadout();
}

} // namespace bloom::ui
