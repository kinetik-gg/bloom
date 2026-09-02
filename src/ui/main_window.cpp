#include <bloom/ui/main_window.hpp>

#include <bloom/host/project_session.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QVBoxLayout>

#include <filesystem>

namespace {

constexpr auto workspaceLayoutKey = "workspace/compositing/layout";
constexpr auto windowGeometryKey = "window/main/geometry";

} // namespace

namespace bloom::ui {

MainWindow::MainWindow(const EditorRegistry& editorRegistry, CompositionSession& compositionSession,
                       ProjectHost& projectHost, FrameExportController& frameExportController,
                       QWidget* parent)
    : QMainWindow(parent), compositionSession_(compositionSession), projectHost_(projectHost),
      frameExportController_(frameExportController) {
    setObjectName("bloomMainWindow");
    setWindowTitle("Bloom");
    resize(1600, 1000);

    createMenus();
    createWorkspaceSwitcher();
    createEditorLayout(editorRegistry);
    createCentralStack();
    createWorkspaceActions();
    updateEditActions();

    connect(&projectHost_, &ProjectHost::dirtyStateChanged, this, &MainWindow::updateWindowTitle);
    connect(&projectHost_, &ProjectHost::sessionReplaced, this, &MainWindow::updateWindowTitle);
    connect(&projectHost_, &ProjectHost::activityChanged, this, &MainWindow::updateFileActions);
    connect(&projectHost_, &ProjectHost::sessionReplaced, this, &MainWindow::updateFileActions);
    // Presentation-level read-only surface (task R1, issue #74): switch which central-stack page
    // is authoritative every time ProjectHost replaces its installed content. Frozen design
    // decision 1's wiring point -- "Switching happens on sessionReplaced() by asking the host's
    // stateSnapshot()".
    connect(&projectHost_, &ProjectHost::sessionReplaced, this, &MainWindow::updateContentSurface);
    connect(&projectHost_, &ProjectHost::saveFinished, this,
            [this](const ProjectHostOperationOutcome outcome, const QString& message) {
                if (outcome == ProjectHostOperationOutcome::Published) {
                    statusBar()->showMessage(message, 4000);
                    return;
                }
                QMessageBox::warning(this, tr("Save Project"), message);
            });
    connect(&projectHost_, &ProjectHost::openFinished, this,
            [this](const ProjectHostOperationOutcome outcome, const QString& message) {
                if (outcome == ProjectHostOperationOutcome::Published) {
                    statusBar()->showMessage(message, 4000);
                    return;
                }
                if (outcome == ProjectHostOperationOutcome::Cancelled) {
                    statusBar()->clearMessage();
                    return;
                }
                QMessageBox::warning(this, tr("Open Project"), message);
            });
    connect(&projectHost_, &ProjectHost::copyFinished, this,
            [this](const ProjectHostOperationOutcome outcome, const QString& message) {
                if (outcome == ProjectHostOperationOutcome::Published) {
                    statusBar()->showMessage(message, 4000);
                    return;
                }
                if (outcome == ProjectHostOperationOutcome::Cancelled) {
                    statusBar()->clearMessage();
                    return;
                }
                QMessageBox::warning(this, tr("Save a Copy"), message);
            });

    // "File -> Export Frame..." gating (task F3, issue #103): a composition exists and no export
    // is currently in flight (FrameExportController::canExport()), and the read-only placeholder is
    // not currently authoritative (a preserved-read-only install leaves CompositionSession bound to
    // its stale prior document -- see updateContentSurface()'s own comment -- so this action must
    // never be offered while that placeholder is showing). Every signal that can change either
    // condition is wired here, mirroring updateFileActions()'s own precedent.
    connect(&frameExportController_, &FrameExportController::busyChanged, this,
            &MainWindow::updateExportAction);
    connect(&compositionSession_, &CompositionSession::compositionChanged, this,
            &MainWindow::updateExportAction);
    connect(&projectHost_, &ProjectHost::sessionReplaced, this, &MainWindow::updateExportAction);
    connect(&frameExportController_, &FrameExportController::exportFinished, this,
            [this](const FrameExportOutcome outcome, const QString& message) {
                if (outcome == FrameExportOutcome::Published ||
                    outcome == FrameExportOutcome::PublishedWithWarning) {
                    statusBar()->showMessage(message, 4000);
                    return;
                }
                if (outcome == FrameExportOutcome::Cancelled) {
                    statusBar()->clearMessage();
                    return;
                }
                QMessageBox::warning(this, tr("Export Frame"), message);
            });

    updateFileActions();
    updateWindowTitle();
    updateContentSurface();
    updateExportAction();
}

WorkspaceHost* MainWindow::workspaceHost() const noexcept { return workspaceHost_; }

bool MainWindow::isShowingReadOnlyPlaceholder() const noexcept {
    return centralStack_ != nullptr && centralStack_->currentWidget() == readOnlyPlaceholderPage_;
}

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
    if (projectHost_.isBusy()) {
        // v1 simplification (no progress/cancellation UI in scope): a close request while a
        // project I/O operation or an unsaved-change decision is already in flight is silently
        // ignored; the artist retries once it finishes.
        return;
    }
    projectHost_.confirmUnsavedChanges([this] {
        if (shutdownRequested_) {
            return;
        }
        shutdownRequested_ = true;
        emit shutdownRequested();
    });
}

void MainWindow::createMenus() {
    auto* fileMenu = menuBar()->addMenu("&File");
    createFileMenu(*fileMenu);
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

void MainWindow::createFileMenu(QMenu& fileMenu) {
    newProjectAction_ = fileMenu.addAction("&New");
    newProjectAction_->setObjectName("newProjectAction");
    newProjectAction_->setShortcut(QKeySequence::New);
    newProjectAction_->setShortcutContext(Qt::WindowShortcut);
    connect(newProjectAction_, &QAction::triggered, &projectHost_, &ProjectHost::newProject);

    openProjectAction_ = fileMenu.addAction("&Open…");
    openProjectAction_->setObjectName("openProjectAction");
    openProjectAction_->setShortcut(QKeySequence::Open);
    openProjectAction_->setShortcutContext(Qt::WindowShortcut);
    connect(openProjectAction_, &QAction::triggered, &projectHost_, &ProjectHost::requestOpen);

    saveProjectAction_ = fileMenu.addAction("&Save");
    saveProjectAction_->setObjectName("saveProjectAction");
    saveProjectAction_->setShortcut(QKeySequence::Save);
    saveProjectAction_->setShortcutContext(Qt::WindowShortcut);
    connect(saveProjectAction_, &QAction::triggered, &projectHost_, &ProjectHost::beginSave);

    saveProjectAsAction_ = fileMenu.addAction("Save &As…");
    saveProjectAsAction_->setObjectName("saveProjectAsAction");
    saveProjectAsAction_->setShortcut(QKeySequence::SaveAs);
    saveProjectAsAction_->setShortcutContext(Qt::WindowShortcut);
    connect(saveProjectAsAction_, &QAction::triggered, &projectHost_, &ProjectHost::requestSaveAs);

    // "Save a Copy…" (task SC1, issue #77): no default shortcut, placed right after Save As.
    saveProjectCopyAction_ = fileMenu.addAction("Save a &Copy…");
    saveProjectCopyAction_->setObjectName("saveProjectCopyAction");
    connect(saveProjectCopyAction_, &QAction::triggered, &projectHost_,
            &ProjectHost::requestSaveCopy);

    fileMenu.addSeparator();

    // "Export Frame…" (task F3, issue #103): no default shortcut, its own group below the
    // project-file actions.
    exportFrameAction_ = fileMenu.addAction("Export &Frame…");
    exportFrameAction_->setObjectName("exportFrameAction");
    connect(exportFrameAction_, &QAction::triggered, &frameExportController_,
            &FrameExportController::requestExport);

    fileMenu.addSeparator();
}

void MainWindow::updateFileActions() {
    const bool busy = projectHost_.isBusy();
    newProjectAction_->setEnabled(!busy);
    openProjectAction_->setEnabled(!busy);
    saveProjectAction_->setEnabled(!busy && projectHost_.canSave());
    saveProjectAsAction_->setEnabled(!busy && projectHost_.canSave());
    saveProjectCopyAction_->setEnabled(!busy && projectHost_.canSaveCopy());

    switch (projectHost_.activity()) {
    case ProjectHostActivity::Saving:
        statusBar()->showMessage(tr("Saving…"));
        break;
    case ProjectHostActivity::Opening:
        statusBar()->showMessage(tr("Opening…"));
        break;
    case ProjectHostActivity::ResolvingUnsavedChanges:
        statusBar()->showMessage(tr("Waiting for a decision about unsaved changes…"));
        break;
    case ProjectHostActivity::Idle:
        // Never force QMainWindow::statusBar() to lazily create a status bar just to clear a
        // message that was never shown (e.g. at initial construction, before any project I/O has
        // ever run): only clear one that already exists.
        if (auto* bar = findChild<QStatusBar*>()) {
            bar->clearMessage();
        }
        break;
    }
}

void MainWindow::updateExportAction() {
    exportFrameAction_->setEnabled(frameExportController_.canExport() &&
                                   !isShowingReadOnlyPlaceholder());
}

void MainWindow::updateWindowTitle() {
    const auto path = projectHost_.displayPath();
    const QString name =
        path.has_value() ? QString::fromStdString(path->filename().string()) : tr("Untitled");
    setWindowTitle(QStringLiteral("%1[*] — Bloom").arg(name));
    setWindowModified(projectHost_.isDirty());
}

void MainWindow::updateContentSurface() {
    const auto snapshot = projectHost_.stateSnapshot();
    const bool readOnly =
        snapshot.contentKind == host::ProjectSessionContentKind::PreservedReadOnly;
    if (!readOnly) {
        centralStack_->setCurrentWidget(workspaceHost_);
        return;
    }

    const auto path = projectHost_.displayPath();
    const QString fileName =
        path.has_value() ? QString::fromStdString(path->filename().string()) : tr("Untitled");
    readOnlyPlaceholderFileNameLabel_->setText(fileName);
    // Honest reason + options (frozen design decision 1, updated by task SC1/issue #77 now that
    // Save a Copy is live rather than promised): this file needs capabilities this Bloom cannot
    // edit safely; it is opened read-only; editing and saving are disabled; File → Save a Copy
    // creates a byte-exact copy of this file today. No apology, no promise beyond what is true
    // today.
    readOnlyPlaceholderBodyLabel_->setText(
        tr("“%1” uses capabilities this version of Bloom cannot edit safely, so it was "
           "opened read-only. Editing and saving are disabled. Use File → Save a Copy to create "
           "a byte-exact copy of this file.")
            .arg(fileName));
    centralStack_->setCurrentWidget(readOnlyPlaceholderPage_);
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
    resetCompositingLayout();
}

void MainWindow::createCentralStack() {
    // The read-only placeholder (task R1, issue #74) is a QStackedWidget wrapping the existing
    // central widget rather than any CompositionSession/ProjectHost surgery: WorkspaceHost is
    // already MainWindow's one central widget with no panel-replacement seam of its own at this
    // level, so a two-page stack is the smallest mechanism that lets MainWindow pick which page is
    // authoritative for the current ProjectHost content kind while leaving WorkspaceHost, its
    // layout, and CompositionSession completely untouched.
    centralStack_ = new QStackedWidget(this);
    centralStack_->addWidget(workspaceHost_);
    readOnlyPlaceholderPage_ = createReadOnlyPlaceholderPage();
    centralStack_->addWidget(readOnlyPlaceholderPage_);
    centralStack_->setCurrentWidget(workspaceHost_);
    setCentralWidget(centralStack_);
}

QWidget* MainWindow::createReadOnlyPlaceholderPage() {
    auto* page = new QWidget(this);
    page->setObjectName("readOnlyPlaceholderPage");

    auto* heading = new QLabel(tr("Read-only project"), page);
    heading->setObjectName("readOnlyPlaceholderHeading");

    readOnlyPlaceholderFileNameLabel_ = new QLabel(page);
    readOnlyPlaceholderFileNameLabel_->setObjectName("readOnlyPlaceholderFileName");

    readOnlyPlaceholderBodyLabel_ = new QLabel(page);
    readOnlyPlaceholderBodyLabel_->setObjectName("readOnlyPlaceholderBody");
    readOnlyPlaceholderBodyLabel_->setWordWrap(true);
    readOnlyPlaceholderBodyLabel_->setMaximumWidth(520);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(48, 48, 48, 48);
    layout->addStretch(1);
    layout->addWidget(heading);
    layout->addWidget(readOnlyPlaceholderFileNameLabel_);
    layout->addWidget(readOnlyPlaceholderBodyLabel_);
    layout->addStretch(2);
    return page;
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

} // namespace bloom::ui
