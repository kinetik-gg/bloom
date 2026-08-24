#include <bloom/ui/main_window.hpp>

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QPalette>
#include <QSettings>
#include <QStringList>

namespace {

constexpr auto workspaceLayoutKey = "workspace/compositing/layout";
constexpr auto windowGeometryKey = "window/main/geometry";

} // namespace

namespace bloom::ui {

MainWindow::MainWindow(const EditorRegistry& editorRegistry, CompositionSession& compositionSession,
                       QWidget* parent)
    : QMainWindow(parent), compositionSession_(compositionSession) {
    setObjectName("bloomMainWindow");
    setWindowTitle("Bloom");
    resize(1600, 1000);

    createMenus();
    createWorkspaceSwitcher();
    createEditorLayout(editorRegistry);
    createWorkspaceActions();
    updateEditActions();
    applyFoundationTheme();
}

WorkspaceHost* MainWindow::workspaceHost() const noexcept { return workspaceHost_; }

WorkspaceLayoutRestoreResult MainWindow::restoreApplicationState(QSettings& settings) {
    const auto geometry = settings.value(windowGeometryKey).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }

    const auto result = workspaceHost_->restorePersistedLayout(settings, workspaceLayoutKey);
    workspaceLayoutWritable_ = result != WorkspaceLayoutRestoreResult::UnsupportedVersion;
    updateWorkspaceActions();
    return result;
}

void MainWindow::saveApplicationState(QSettings& settings) const {
    settings.setValue(windowGeometryKey, saveGeometry());
    if (workspaceLayoutWritable_) {
        workspaceHost_->persistLayout(settings, workspaceLayoutKey);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    event->ignore();
    if (shutdownRequested_) {
        return;
    }
    shutdownRequested_ = true;
    emit shutdownRequested();
}

void MainWindow::createMenus() {
    menuBar()->addMenu("&File");
    auto* editMenu = menuBar()->addMenu("&Edit");
    undoAction_ = editMenu->addAction("Undo");
    undoAction_->setObjectName("undoAction");
    undoAction_->setShortcut(QKeySequence::Undo);
    undoAction_->setShortcutContext(Qt::WindowShortcut);
    connect(undoAction_, &QAction::triggered, &compositionSession_, &CompositionSession::undo);

    redoAction_ = editMenu->addAction("Redo");
    redoAction_->setObjectName("redoAction");
    redoAction_->setShortcut(QKeySequence::Redo);
    redoAction_->setShortcutContext(Qt::WindowShortcut);
    connect(redoAction_, &QAction::triggered, &compositionSession_, &CompositionSession::redo);
    connect(&compositionSession_, &CompositionSession::historyChanged, this,
            &MainWindow::updateEditActions);

    menuBar()->addMenu("&View");
    windowMenu_ = menuBar()->addMenu("&Window");
    menuBar()->addMenu("&Help");
}

void MainWindow::updateEditActions() {
    const QString undoLabel = compositionSession_.undoLabel();
    const QString redoLabel = compositionSession_.redoLabel();
    undoAction_->setEnabled(compositionSession_.canUndo());
    redoAction_->setEnabled(compositionSession_.canRedo());
    undoAction_->setText(undoLabel.isEmpty() ? tr("Undo") : tr("Undo %1").arg(undoLabel));
    redoAction_->setText(redoLabel.isEmpty() ? tr("Redo") : tr("Redo %1").arg(redoLabel));
}

void MainWindow::createWorkspaceSwitcher() {
    menuBar()->addSeparator();

    auto* group = new QActionGroup(menuBar());
    group->setExclusive(true);

    const QStringList workspaces = {"Compositing", "Editing", "Grading", "Scripting", "Rendering"};
    for (const QString& workspace : workspaces) {
        auto* action = menuBar()->addAction(workspace);
        action->setCheckable(true);
        action->setEnabled(workspace == "Compositing");
        action->setChecked(workspace == "Compositing");
        group->addAction(action);
    }
}

void MainWindow::createEditorLayout(const EditorRegistry& editorRegistry) {
    workspaceHost_ = new WorkspaceHost(editorRegistry, this);
    setCentralWidget(workspaceHost_);

    resetCompositingLayout();
}

void MainWindow::resetCompositingLayout() {
    workspaceHost_->resetToSingleArea("bloom.viewer");
    auto* viewer = workspaceHost_->activeArea();
    (void)workspaceHost_->splitArea(*viewer, Qt::Vertical, "bloom.timeline", 0.32);
    auto* nodes = workspaceHost_->splitArea(*viewer, Qt::Horizontal, "bloom.nodes", 0.50);
    auto* media = workspaceHost_->splitArea(*nodes, Qt::Horizontal, "bloom.media", 0.28);
    (void)workspaceHost_->splitArea(*media, Qt::Vertical, "bloom.properties", 0.65);
    workspaceHost_->setActiveArea(viewer);
    workspaceLayoutWritable_ = true;
}

void MainWindow::createWorkspaceActions() {
    splitLeftRightAction_ = windowMenu_->addAction("Split Active Area Left/Right");
    splitLeftRightAction_->setObjectName("splitAreaLeftRightAction");
    connect(splitLeftRightAction_, &QAction::triggered, workspaceHost_,
            [this] { workspaceHost_->splitActiveArea(Qt::Horizontal); });

    splitTopBottomAction_ = windowMenu_->addAction("Split Active Area Top/Bottom");
    splitTopBottomAction_->setObjectName("splitAreaTopBottomAction");
    connect(splitTopBottomAction_, &QAction::triggered, workspaceHost_,
            [this] { workspaceHost_->splitActiveArea(Qt::Vertical); });

    windowMenu_->addSeparator();

    closeAreaAction_ = windowMenu_->addAction("Close Active Area");
    closeAreaAction_->setObjectName("closeAreaAction");
    connect(closeAreaAction_, &QAction::triggered, workspaceHost_,
            [this] { (void)workspaceHost_->closeActiveArea(); });

    maximizeAreaAction_ = windowMenu_->addAction("Maximize Active Area");
    maximizeAreaAction_->setObjectName("maximizeAreaAction");
    maximizeAreaAction_->setCheckable(true);
    connect(maximizeAreaAction_, &QAction::triggered, workspaceHost_,
            [this] { workspaceHost_->toggleMaximizeActiveArea(); });

    windowMenu_->addSeparator();
    auto* resetLayoutAction = windowMenu_->addAction("Reset Compositing Layout");
    resetLayoutAction->setObjectName("resetCompositingLayoutAction");
    connect(resetLayoutAction, &QAction::triggered, this, [this] {
        resetCompositingLayout();
        updateWorkspaceActions();
    });

    connect(workspaceHost_, &WorkspaceHost::areaCountChanged, this,
            [this] { updateWorkspaceActions(); });
    connect(workspaceHost_, &WorkspaceHost::maximizeStateChanged, this,
            [this] { updateWorkspaceActions(); });
    connect(workspaceHost_, &WorkspaceHost::activeAreaChanged, this,
            [this] { updateWorkspaceActions(); });
    updateWorkspaceActions();
}

void MainWindow::updateWorkspaceActions() {
    const bool hasMultipleAreas = workspaceHost_->areaCount() > 1;
    const bool canChangeStructure = !workspaceHost_->isAreaMaximized();
    splitLeftRightAction_->setEnabled(canChangeStructure);
    splitTopBottomAction_->setEnabled(canChangeStructure);
    closeAreaAction_->setEnabled(hasMultipleAreas && canChangeStructure);
    maximizeAreaAction_->setEnabled(hasMultipleAreas);
    maximizeAreaAction_->setChecked(workspaceHost_->isAreaMaximized());
    maximizeAreaAction_->setText(workspaceHost_->isAreaMaximized() ? "Restore Active Area"
                                                                   : "Maximize Active Area");
}

void MainWindow::applyFoundationTheme() {
    QPalette palette = QApplication::palette();
    palette.setColor(QPalette::Window, QColor(18, 18, 18));
    palette.setColor(QPalette::WindowText, QColor(215, 215, 215));
    palette.setColor(QPalette::Base, QColor(14, 14, 14));
    palette.setColor(QPalette::AlternateBase, QColor(25, 25, 25));
    palette.setColor(QPalette::Text, QColor(215, 215, 215));
    palette.setColor(QPalette::Button, QColor(28, 28, 28));
    palette.setColor(QPalette::ButtonText, QColor(215, 215, 215));
    palette.setColor(QPalette::Highlight, QColor(23, 142, 230));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    QApplication::setPalette(palette);

    setStyleSheet(R"(
        QMainWindow, QMenuBar, QMenu {
            background: #121212;
            color: #d7d7d7;
        }
        QMenuBar::item:checked {
            background: #303030;
            border-radius: 4px;
        }
        QFrame#editorArea {
            background: #111111;
            border: 1px solid #292929;
        }
        QFrame#editorArea[active="true"] {
            border-color: #178ee6;
        }
        QWidget#editorHeader {
            background: #171717;
            border-bottom: 1px solid #292929;
        }
        QLabel#editorPlaceholder {
            color: #777777;
        }
        QComboBox {
            background: #1b1b1b;
            border: 1px solid #303030;
            border-radius: 4px;
            padding: 3px 8px;
        }
        QToolButton {
            color: #bcbcbc;
            padding: 2px 4px;
        }
        QToolButton:hover {
            background: #303030;
        }
        QSplitter::handle {
            background: #292929;
        }
    )");
}

} // namespace bloom::ui
