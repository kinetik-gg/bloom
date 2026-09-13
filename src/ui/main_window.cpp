#include <bloom/ui/main_window.hpp>

#include <bloom/host/project_session.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/licenses_window.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QKeySequence>
#include <QLabel>
#include <QLatin1StringView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>

namespace {

constexpr auto workspaceLayoutKey = "workspace/compositing/layout";
constexpr auto windowGeometryKey = "window/main/geometry";
constexpr auto chromeModeKey = "appearance/chrome";
constexpr auto issueTrackerUrl = "https://github.com/kinetik-gg/bloom/issues/new";

} // namespace

namespace bloom::ui {

ChromeMode chromeModeFromSettings(const QSettings& settings) {
    const auto value =
        settings.value(QLatin1StringView(chromeModeKey), QStringLiteral("custom")).toString();
    return value.compare(QStringLiteral("native"), Qt::CaseInsensitive) == 0 ? ChromeMode::Native
                                                                             : ChromeMode::Custom;
}

void setChromeModeInSettings(QSettings& settings, const ChromeMode mode) {
    settings.setValue(QLatin1StringView(chromeModeKey), mode == ChromeMode::Native
                                                            ? QStringLiteral("native")
                                                            : QStringLiteral("custom"));
}

MainWindow::MainWindow(const EditorRegistry& editorRegistry, CompositionSession& compositionSession,
                       ProjectHost& projectHost, FrameExportController& frameExportController,
                       QWidget* parent)
    : QMainWindow(parent), compositionSession_(compositionSession), projectHost_(projectHost),
      frameExportController_(frameExportController) {
    setObjectName("bloomMainWindow");
    setWindowTitle("Bloom");
    resize(1600, 1000);

    createChrome();
    createMenus(*menuBar_);
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
                    // A cancelled RANGE says how many frames landed, which is information the
                    // artist needs; a cancelled single frame wrote nothing, so it stays silent as
                    // before.
                    if (message.isEmpty()) {
                        statusBar()->clearMessage();
                    } else {
                        statusBar()->showMessage(message, 4000);
                    }
                    return;
                }
                QMessageBox::warning(this, tr("Export Frame"), message);
            });
    // Range progress reaches the artist through the status bar and keeps the cancel item's enabled
    // state in step, using exactly the signals the controller already emits.
    connect(&frameExportController_, &FrameExportController::rangeProgressChanged, this, [this] {
        updateExportAction();
        if (frameExportController_.isExportingRange()) {
            statusBar()->showMessage(tr("Exporting frame %1 of %2…")
                                         .arg(frameExportController_.publishedFrameCount() + 1)
                                         .arg(frameExportController_.totalFrameCount()));
        }
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

void MainWindow::createChrome() {
    // Native (server-side) window chrome only (task C1, owner: "let OS handle the native window
    // chrome for now" / "no need for custom minimize/maximize/close" / "remove the Bloom app
    // title"). QMainWindow's own classic menu bar, stock OS-drawn window decorations -- the
    // Kinetik TitleBar, FramelessEdgeResizer, and the "appearance/chrome" setting all stay
    // compiled and independently tested (see the ChromeMode comment in main_window.hpp) for a
    // possible future custom-chrome/CSD return, but MainWindow never constructs any of them.
    menuBar_ = menuBar();
}

void MainWindow::toggleFullScreen() {
    if (isFullScreen()) {
        showNormal();
    } else {
        showFullScreen();
    }
}

void MainWindow::createMenus(QMenuBar& menuBar) {
    auto* fileMenu = menuBar.addMenu("&File");
    createFileMenu(*fileMenu);
    auto* editMenu = menuBar.addMenu("&Edit");
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

    viewMenu_ = menuBar.addMenu("&View");
    createViewMenu(*viewMenu_);
    windowMenu_ = menuBar.addMenu("&Window");
    auto* helpMenu = menuBar.addMenu("&Help");
    createHelpMenu(*helpMenu);
}

void MainWindow::createViewMenu(QMenu& viewMenu) {
    // Full Screen (decision 2): F11, plain window fullscreen toggle.
    viewFullScreenAction_ = viewMenu.addAction("Full Screen");
    viewFullScreenAction_->setObjectName("viewFullScreenAction");
    viewFullScreenAction_->setCheckable(true);
    viewFullScreenAction_->setShortcut(QKeySequence(Qt::Key_F11));
    viewFullScreenAction_->setShortcutContext(Qt::WindowShortcut);
    connect(viewFullScreenAction_, &QAction::triggered, this, &MainWindow::toggleFullScreen);

    // Maximize Panel (decision 2): routes to the SAME editor-maximize the Window menu's
    // "Maximize Active Area" already drives -- wired once workspaceHost_ exists, in
    // createWorkspaceActions(), which also keeps both actions' checked state in sync.
    viewMaximizePanelAction_ = viewMenu.addAction("Maximize Panel");
    viewMaximizePanelAction_->setObjectName("viewMaximizePanelAction");
    viewMaximizePanelAction_->setCheckable(true);
    // task C1: the "Use Native Window Frame" toggle is gone -- native chrome is the default and
    // only mode now, so there is no longer a chrome setting for this menu to offer.
}

void MainWindow::createHelpMenu(QMenu& helpMenu) {
    // "Report an Issue…" (decision 2): opens the repository's issue tracker via QDesktopServices,
    // whose url handler is Qt's own interception seam for tests (QDesktopServices::setUrlHandler).
    reportIssueAction_ = helpMenu.addAction("Report an Issue…");
    reportIssueAction_->setObjectName("reportIssueAction");
    connect(reportIssueAction_, &QAction::triggered, this,
            [] { QDesktopServices::openUrl(QUrl(QString::fromLatin1(issueTrackerUrl))); });

    // "Open Source Licenses…" (decision 3): a non-modal LicensesWindow, so triggering the action
    // never blocks the event loop (or an automated test that fires it synchronously).
    openSourceLicensesAction_ = helpMenu.addAction("Open Source Licenses…");
    openSourceLicensesAction_->setObjectName("openSourceLicensesAction");
    connect(openSourceLicensesAction_, &QAction::triggered, this, [this] {
        auto* dialog = new LicensesWindow(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });
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

    // "Export Frame Range…" (task S5, item 3a), beside the single-frame export it reuses stage for
    // stage, plus the cancel that a range -- unlike one frame -- is long enough to need.
    exportFrameRangeAction_ = fileMenu.addAction("Export Frame &Range…");
    exportFrameRangeAction_->setObjectName("exportFrameRangeAction");
    connect(exportFrameRangeAction_, &QAction::triggered, &frameExportController_,
            &FrameExportController::requestRangeExport);

    cancelFrameExportAction_ = fileMenu.addAction("Cancel Frame &Export");
    cancelFrameExportAction_->setObjectName("cancelFrameExportAction");
    connect(cancelFrameExportAction_, &QAction::triggered, &frameExportController_,
            &FrameExportController::requestCancellation);

    fileMenu.addSeparator();

    // "Quit" (task S1): the artist had no way to quit from the menu at all -- closing the window
    // was the only path that reached shutdown. Reuses that exact path (QWidget::close() ->
    // MainWindow::closeEvent() -> confirmUnsavedChanges() -> shutdownRequested()) rather than
    // duplicating any shutdown sequencing here, so busy-state and unsaved-change handling stay in
    // the one place that already owns them.
    quitAction_ = fileMenu.addAction(tr("&Quit"));
    quitAction_->setObjectName("quitAction");
    quitAction_->setMenuRole(QAction::QuitRole);
    quitAction_->setShortcut(QKeySequence::Quit);
    quitAction_->setShortcutContext(Qt::WindowShortcut);
    connect(quitAction_, &QAction::triggered, this, &QWidget::close);
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
    const bool canExport = frameExportController_.canExport() && !isShowingReadOnlyPlaceholder();
    exportFrameAction_->setEnabled(canExport);
    exportFrameRangeAction_->setEnabled(canExport);
    // Offered only while there is actually something to cancel: a range export in flight. A
    // disabled item is honest about that; an always-enabled one would promise a cancel with nothing
    // to cancel.
    cancelFrameExportAction_->setEnabled(frameExportController_.isExportingRange());
}

void MainWindow::updateWindowTitle() {
    // Native window chrome only (task C1, owner: "remove the Bloom app title"): the OS window
    // title IS the document title -- "Untitled" or the file name, with Qt's own "[*]" modified
    // marker -- and nothing else. No "Bloom — " prefix: the owner wants the document title
    // centered in the title bar, and the OS does that centering on its own.
    const auto path = projectHost_.displayPath();
    const QString name =
        path.has_value() ? QString::fromStdString(path->filename().string()) : tr("Untitled");
    setWindowTitle(QStringLiteral("%1[*]").arg(name));
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

    // View menu's "Maximize Panel" (decision 2) routes to the exact same signal -- wired here,
    // now that workspaceHost_ exists, rather than in createViewMenu().
    connect(viewMaximizePanelAction_, &QAction::triggered, workspaceHost_,
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
    // Kept in lockstep with the Window menu's own action: same underlying state, two menu homes.
    viewMaximizePanelAction_->setEnabled(hasMultipleAreas);
    viewMaximizePanelAction_->setChecked(workspaceHost_->isAreaMaximized());
}

} // namespace bloom::ui
