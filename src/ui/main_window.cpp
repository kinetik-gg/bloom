#include "import_progress_dialog.hpp"
#include "preferences_window.hpp"
#include "project_color_settings_dialog.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/mcp_server_adapter.hpp>
#include <bloom/ui/context_bar.hpp>
#include <bloom/ui/layer_alignment_actions.hpp>
#include <bloom/ui/preferences_aware.hpp>
#include <memory>

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <bloom/ui/acceleration_status.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/cache_purge_controller.hpp>
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/h264_encoder_flow.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/licenses_window.hpp>
#include <bloom/ui/media_disk_cache_settings.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/window_status_bar.hpp>
#include <bloom/ui/workspace_host.hpp>
#include <bloom/ui/workspace_presets.hpp>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLatin1StringView>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>

namespace {

constexpr auto workspaceLayoutKey = "workspace/compositing/layout";
constexpr auto timelineLayerColumnWidthKey = "timeline/layer-column-width";
constexpr auto windowGeometryKey = "window/main/geometry";
constexpr auto chromeModeKey = "appearance/chrome";
// Session/UI state like the workspace layout: whether Window > Context Bar shows the bar.
constexpr auto contextBarVisibleKey = "workspace/context-bar-visible";
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
                       ProjectHost& projectHost,
                       RamPreviewController* const ramPreview,
                       CompositionPreviewController* const previewController, QWidget* parent,
                       PlaybackController* const playbackController,
                       runtime::OperationCache* const operationCache,
                       media::cache::MediaDiskCache* const mediaDiskCache,
                       const AccelerationStatusProvider* const accelerationStatus,
                       CachePurgeController* const cachePurgeController)
    : QMainWindow(parent), compositionSession_(compositionSession), projectHost_(projectHost),
      ramPreview_(ramPreview),
      previewController_(previewController), playbackController_(playbackController),
      operationCache_(operationCache), mediaDiskCache_(mediaDiskCache),
      cachePurgeController_(cachePurgeController), accelerationStatus_(accelerationStatus) {
    displayedViewerUpdateTimer_.setSingleShot(true);
    connect(&displayedViewerUpdateTimer_, &QTimer::timeout, this,
            &MainWindow::updateDisplayedViewer);
    setObjectName("bloomMainWindow");
    setWindowTitle("Bloom");
    resize(kit::px(kit::Size::MainWindowWidth), kit::px(kit::Size::MainWindowHeight));

    createChrome();
    createEditorLayout(editorRegistry);
    // Before createMenus()/updateFileActions(): every one of those paths can already want to say
    // something, and the strip is what says it.
    createCentralStack();
    createMenus(*menuBar_);
    createWorkspaceActions();
    updateEditActions();
    if (cachePurgeController_ != nullptr) {
        connect(cachePurgeController_, &CachePurgeController::purgeStarted, this, [this] {
            updatePurgeActions();
            statusStrip_->setPersistentMessage(tr("Purging caches…"));
        });
        connect(cachePurgeController_, &CachePurgeController::purgeFinished, this,
                [this](const bool /*success*/, const QString& message) {
                    updatePurgeActions();
                    statusStrip_->setPersistentMessage({});
                    statusStrip_->showTransientMessage(message);
                });
    }
    updatePurgeActions();
    // Every batch import shows its progress and outcome over this window (#377).
    if (auto* assets = compositionSession_.assetController()) {
        auto* importProgress = new ImportProgressPresenter(*assets, *this);
        connect(importProgress, &ImportProgressPresenter::notice, this,
                [this](const QString& message) { statusStrip_->showTransientMessage(message); });
    }

    connect(&projectHost_, &ProjectHost::dirtyStateChanged, this, &MainWindow::updateWindowTitle);
    connect(&projectHost_, &ProjectHost::sessionReplaced, this, &MainWindow::updateWindowTitle);
    connect(&projectHost_, &ProjectHost::activityChanged, this, &MainWindow::updateFileActions);
    connect(&projectHost_, &ProjectHost::recoveryScanActivityChanged, this,
            [this](const bool active) {
                if (projectHost_.activity() == ProjectHostActivity::Idle) {
                    statusStrip_->setPersistentMessage(active ? tr("Checking recovery files…")
                                                              : QString{});
                }
            });
    connect(&projectHost_, &ProjectHost::recoveryDiagnostic, this,
            [this](const QString& message) { statusStrip_->showTransientMessage(message); });
    connect(&projectHost_, &ProjectHost::sessionReplaced, this, &MainWindow::updateFileActions);
    // Presentation-level read-only surface (task R1, issue #74): switch which central-stack page
    // is authoritative every time ProjectHost replaces its installed content. Frozen design
    // decision 1's wiring point -- "Switching happens on sessionReplaced() by asking the host's
    // stateSnapshot()".
    connect(&projectHost_, &ProjectHost::sessionReplaced, this, &MainWindow::updateContentSurface);
    connect(&projectHost_, &ProjectHost::saveFinished, this,
            [this](const ProjectHostOperationOutcome outcome, const QString& message) {
                if (outcome == ProjectHostOperationOutcome::Published ||
                    projectHost_.agentOperation()) {
                    // An agent's call reports its failure to the agent, never in a dialog.
                    statusStrip_->showTransientMessage(message);
                    return;
                }
                kit::inform(*this, kit::KPromptTone::Error, tr("Save Project"),
                            tr("The project could not be saved."), message,
                            QStringLiteral("projectSaveFailed"));
            });
    connect(&projectHost_, &ProjectHost::openFinished, this,
            [this](const ProjectHostOperationOutcome outcome, const QString& message) {
                if (projectHost_.agentOperation()) {
                    statusStrip_->showTransientMessage(message);
                    return;
                }
                if (outcome == ProjectHostOperationOutcome::Published) {
                    statusStrip_->showTransientMessage(message);
                    // ADR 0032's one-time note for an older project, in an asynchronous
                    // window-modal notice that does not block the open.
                    if (const auto& notice = projectHost_.openNotice(); !notice.isEmpty())
                        kit::inform(*this, kit::KPromptTone::Info, tr("Open Project"), {}, notice,
                                    QStringLiteral("openNoticeBox"));
                    return;
                }
                if (outcome == ProjectHostOperationOutcome::Cancelled) {
                    statusStrip_->clearTransientMessage();
                    return;
                }
                kit::inform(*this, kit::KPromptTone::Error, tr("Open Project"),
                            tr("The project could not be opened."), message,
                            QStringLiteral("projectOpenFailed"));
            });
    connect(&projectHost_, &ProjectHost::copyFinished, this,
            [this](const ProjectHostOperationOutcome outcome, const QString& message) {
                if (outcome == ProjectHostOperationOutcome::Published) {
                    statusStrip_->showTransientMessage(message);
                    return;
                }
                if (outcome == ProjectHostOperationOutcome::Cancelled) {
                    statusStrip_->clearTransientMessage();
                    return;
                }
                kit::inform(*this, kit::KPromptTone::Error, tr("Save a Copy"),
                            tr("The copy could not be saved."), message,
                            QStringLiteral("projectCopyFailed"));
            });

    updateFileActions();
    updateWindowTitle();
    updateContentSurface();
    updateExportQueueActions();
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

    const bool barVisible = settings.value(QLatin1StringView(contextBarVisibleKey), true).toBool();
    contextBar_->setVisible(barVisible);
    contextBarAction_->setChecked(barVisible);

    const auto result = workspaceHost_->restorePersistedLayout(settings, workspaceLayoutKey);
    workspacePresets_->load();
    if (result == WorkspaceLayoutRestoreResult::Deferred) {
        // A live native surface must retire before the restored tree may replace the current one.
        // Nothing was replaced and no version migration is claimed; the restore completes
        // asynchronously through WorkspaceHost. Report the honest pending result.
        workspaceLayoutWritable_ = true;
        updateWorkspaceActions();
        return result;
    }
    // Version 1 predates the required Assets sidebar and full-height right column.
    // Migrate only a validated legacy layout; future versions remain untouched.
    const int version = QJsonDocument::fromJson(settings.value(workspaceLayoutKey).toByteArray())
                            .object()
                            .value("schema")
                            .toInt();
    if (result == WorkspaceLayoutRestoreResult::Restored && version < kit::Layout::WorkspaceVersion)
        resetCompositingLayout();
    workspaceLayoutWritable_ = result != WorkspaceLayoutRestoreResult::UnsupportedVersion;
    updateWorkspaceActions();
    return result;
}

void MainWindow::saveApplicationState(QSettings& settings) const {
    settings.setValue(windowGeometryKey, saveGeometry());
    settings.setValue(QLatin1StringView(contextBarVisibleKey), contextBarAction_->isChecked());
    if (workspaceLayoutWritable_) {
        workspaceHost_->persistLayout(settings, workspaceLayoutKey);
    }
    if (playbackController_ != nullptr) {
        settings.setValue(QStringLiteral("playback/audio-enabled"),
                          playbackController_->isAudioEnabled());
    }
}

void MainWindow::completeShutdown() {
    shutdownComplete_ = true;
    close();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (shutdownComplete_) {
        event->accept();
        return;
    }
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
    projectHost_.confirmUnsavedChanges(
        [this] {
            if (shutdownRequested_) {
                return;
            }
            shutdownRequested_ = true;
            emit shutdownRequested();
        },
        true, UnsavedChangeAction::Close);
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

    // Edit | Preferences... (macOS routes a PreferencesRole action into the application menu with
    // the standard shortcut; on Windows and Linux it stays here). This edits global application
    // preferences; it is deliberately separate from File | Project Settings..., which edits
    // project truth.
    editMenu->addSeparator();
    preferencesAction_ = editMenu->addAction(tr("Preferences…"));
    preferencesAction_->setObjectName(QStringLiteral("preferencesAction"));
    preferencesAction_->setMenuRole(QAction::PreferencesRole);
    preferencesAction_->setShortcut(QKeySequence::Preferences);
    preferencesAction_->setShortcutContext(Qt::WindowShortcut);
    connect(preferencesAction_, &QAction::triggered, this, &MainWindow::showPreferences);

    // Edit | Purge… — explicit derived-cache commands (docs/user-guide/memory.md "Purging caches").
    // Both actions are present and enabled except while a purge is running: derived caches are
    // optional, and an unavailable one is reported clearly when the command runs rather than
    // silently omitted from the menu. Neither is a project edit, so no command, revision, dirty
    // flag, or undo entry is produced.
    editMenu->addSeparator();
    purgeMenu_ = editMenu->addMenu(tr("Purge…"));
    purgeMenu_->setObjectName(QStringLiteral("editPurgeMenu"));
    purgePreviewCacheAction_ = purgeMenu_->addAction(tr("Purge preview cache"));
    purgePreviewCacheAction_->setObjectName(QStringLiteral("purgePreviewCacheAction"));
    connect(purgePreviewCacheAction_, &QAction::triggered, this, &MainWindow::purgePreviewCache);
    purgeMediaCacheAction_ = purgeMenu_->addAction(tr("Purge media cache"));
    purgeMediaCacheAction_->setObjectName(QStringLiteral("purgeMediaCacheAction"));
    connect(purgeMediaCacheAction_, &QAction::triggered, this, &MainWindow::purgeMediaCache);

    compositionMenu_ = menuBar.addMenu("&Composition");
    createCompositionMenu(*compositionMenu_);
    viewMenu_ = menuBar.addMenu("&View");
    createViewMenu(*viewMenu_);
    windowMenu_ = menuBar.addMenu("&Window");
    auto* helpMenu = menuBar.addMenu("&Help");
    createHelpMenu(*helpMenu);
}

void MainWindow::createCompositionMenu(QMenu& compositionMenu) {
    newCompositionAction_ = compositionMenu.addAction("New Composition…");
    newCompositionAction_->setObjectName("compositionNewAction");
    newCompositionAction_->setIcon(kit::icon(kit::IconId::Composition, kit::IconRole::Chrome));
    newCompositionAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_N));
    newCompositionAction_->setShortcutContext(Qt::WindowShortcut);
    connect(newCompositionAction_, &QAction::triggered, this, [this] {
        if (const auto id = showNewCompositionDialog(compositionSession_, *this); id.has_value()) {
            (void)compositionSession_.setComposition(*id);
        }
    });
    // #376: the shared modal in settings mode. After Effects' Ctrl+K already splits layers here, so
    // it takes Ctrl+Alt+K beside New Composition's Ctrl+Alt+N.
    compositionSettingsAction_ = compositionMenu.addAction("Composition Settings…");
    compositionSettingsAction_->setObjectName("compositionSettingsAction");
    compositionSettingsAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_K));
    compositionSettingsAction_->setShortcutContext(Qt::WindowShortcut);
    connect(compositionSettingsAction_, &QAction::triggered, this, [this] {
        (void)showCompositionSettingsDialog(compositionSession_,
                                            compositionSession_.compositionId(), *this);
    });
    duplicateCompositionAction_ = compositionMenu.addAction("Duplicate Composition");
    duplicateCompositionAction_->setObjectName("compositionDuplicateAction");
    connect(duplicateCompositionAction_, &QAction::triggered, this, [this] {
        if (const auto id =
                duplicateComposition(compositionSession_, compositionSession_.compositionId());
            id.has_value()) {
            (void)compositionSession_.setComposition(*id);
        }
    });
    renameCompositionAction_ = compositionMenu.addAction("Rename Composition…");
    renameCompositionAction_->setObjectName("compositionRenameAction");
    connect(renameCompositionAction_, &QAction::triggered, this, [this] {
        renameComposition(compositionSession_, compositionSession_.compositionId(), *this);
    });
    deleteCompositionAction_ = compositionMenu.addAction("Delete Composition");
    deleteCompositionAction_->setObjectName("compositionDeleteAction");
    connect(deleteCompositionAction_, &QAction::triggered, this, [this] {
        // The Assets delete path and its in-use warning.
        requestDeleteComposition(compositionSession_, compositionSession_.compositionId(), *this);
    });
    compositionMenu.addSeparator();
    // ADR 0031: the same Align and Distribute actions as the context bar.
    compositionMenu.addMenu(contextBar_->alignment().createMenu(&compositionMenu));
    compositionMenu.addSeparator();

    connect(&compositionSession_, &CompositionSession::snapshotChanged, this,
            &MainWindow::updateCompositionActions);
    connect(&compositionSession_, &CompositionSession::compositionChanged, this,
            &MainWindow::updateCompositionActions);
    updateCompositionActions();

    // RAM Preview (task PERF1, item 3). Same command the Timeline transport's own button and
    // Ctrl+Shift+Space reach -- one named method called by all three, never a menu item
    // synthesizing a key press (docs/ux/interaction-model.md, "Ownership Boundary"). The shortcut
    // itself is declared on the Timeline editor, which owns the transport; declaring it here too
    // would give one key two owners.
    ramPreviewAction_ = compositionMenu.addAction("&RAM Preview");
    ramPreviewAction_->setObjectName("compositionRamPreviewAction");
    // Numpad 0 is After Effects' RAM Preview key. With Num Lock off the same key reports Insert, so
    // both keypad forms are bound. A keypad digit typed into a text or value field is text input,
    // which Qt delivers as a ShortcutOverride to the field first, so it never fires here.
    ramPreviewAction_->setShortcuts({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Space),
                                     QKeySequence(Qt::KeypadModifier | Qt::Key_0),
                                     QKeySequence(Qt::KeypadModifier | Qt::Key_Insert)});
    ramPreviewAction_->setShortcutContext(Qt::WindowShortcut);
    ramPreviewAction_->setEnabled(ramPreview_ != nullptr);
    // Escape ends a run that is caching. Enabled only while one is, so Escape keeps meaning
    // whatever it already meant everywhere else the rest of the time -- the same gating the
    // frame-export cancel uses.
    cancelRamPreviewAction_ = compositionMenu.addAction("Cancel RAM Preview");
    cancelRamPreviewAction_->setObjectName("cancelRamPreviewAction");
    cancelRamPreviewAction_->setShortcut(QKeySequence(Qt::Key_Escape));
    cancelRamPreviewAction_->setShortcutContext(Qt::WindowShortcut);
    cancelRamPreviewAction_->setEnabled(false);
    if (ramPreview_ != nullptr) {
        connect(ramPreviewAction_, &QAction::triggered, ramPreview_, &RamPreviewController::toggle);
        connect(cancelRamPreviewAction_, &QAction::triggered, ramPreview_,
                &RamPreviewController::cancel);
        connect(ramPreview_, &RamPreviewController::stateChanged, this, [this] {
            cancelRamPreviewAction_->setEnabled(ramPreview_->isCaching());
            ramPreviewAction_->setText(ramPreview_->isCaching() ? "Cancel &RAM Preview"
                                                                : "&RAM Preview");
        });
    }
}

void MainWindow::updateCompositionActions() {
    if (newCompositionAction_ == nullptr) {
        return;
    }
    const bool hasComposition = compositionSession_.composition() != nullptr;
    duplicateCompositionAction_->setEnabled(hasComposition);
    renameCompositionAction_->setEnabled(hasComposition);
    compositionSettingsAction_->setEnabled(hasComposition);
    // Deleting the final composition is allowed and returns to the blank, usable empty project.
    deleteCompositionAction_->setEnabled(hasComposition);
}

void MainWindow::purgePreviewCache() {
    // Edit | Purge… | Purge preview cache. The RAM preview frame cache and the in-flight preview
    // work are retired on the UI thread (cheap, and the displayed frame stays valid). The
    // operation-cache and GPU-scene clears run through the purge controller once the scheduler has
    // retired every task, so a value computed by work that was in flight cannot repopulate a cache
    // after the clear. Project truth, revision, dirty state, and undo history are untouched.
    if (cachePurgeController_ != nullptr) {
        // Gate first (retires in-flight work and blocks new asks), then clear the frame cache.
        if (!cachePurgeController_->requestPreviewPurge()) {
            statusStrip_->showTransientMessage(tr("A cache purge is already running"));
            return;
        }
        if (previewController_ != nullptr) {
            previewController_->purgePreviewCache();
        } else if (ramPreview_ != nullptr) {
            ramPreview_->cancel();
        }
        return;
    }
    if (previewController_ != nullptr) {
        previewController_->purgePreviewCache();
    } else if (ramPreview_ != nullptr) {
        ramPreview_->cancel();
    }
    // A window built without the purge task system (tests, a headless host): the operation cache's
    // clear is pure in-memory bookkeeping, so it stays safe to do here.
    if (operationCache_ != nullptr) {
        operationCache_->clearOperations();
        statusStrip_->showTransientMessage(tr("Preview cache purged"));
    } else {
        statusStrip_->showTransientMessage(tr("No preview cache is available"));
    }
}

void MainWindow::purgeMediaCache() {
    // Edit | Purge… | Purge media cache. The on-disk clear flushes the pending writer and removes
    // entries from disk, so it must run on the task system's BlockingIo lane; the controller waits
    // for scheduler retirement first. Source files are never touched. No confirmation: the caches
    // are rebuildable derived state.
    if (cachePurgeController_ != nullptr) {
        if (!cachePurgeController_->requestMediaPurge()) {
            statusStrip_->showTransientMessage(tr("A cache purge is already running"));
            return;
        }
        // Completed frames also encode the retained source revision. Drop them while the producer
        // gate is active, so the first request after refresh cannot bypass source verification.
        if (previewController_ != nullptr) {
            previewController_->purgePreviewCache();
        } else if (ramPreview_ != nullptr) {
            ramPreview_->cancel();
        }
        return;
    }
    if (previewController_ != nullptr) {
        previewController_->purgePreviewCache();
    } else if (ramPreview_ != nullptr) {
        ramPreview_->cancel();
    }
    if (operationCache_ != nullptr) {
        // Memory-only fallback for a host with no task system. The disk store is deliberately not
        // cleared here, because that I/O must never run on the UI thread.
        operationCache_->clearDecodedMedia();
        statusStrip_->showTransientMessage(tr("Media cache purged"));
    } else {
        statusStrip_->showTransientMessage(tr("No media cache is available"));
    }
}

void MainWindow::updatePurgeActions() {
    if (purgePreviewCacheAction_ == nullptr || purgeMediaCacheAction_ == nullptr) {
        return;
    }
    const bool busy = cachePurgeController_ != nullptr && cachePurgeController_->isPurging();
    purgePreviewCacheAction_->setEnabled(!busy);
    purgeMediaCacheAction_->setEnabled(!busy);
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
    if (playbackController_ != nullptr) {
        viewMenu.addSeparator();
        viewAudioEnabledAction_ = viewMenu.addAction("Audio Playback");
        viewAudioEnabledAction_->setObjectName("viewAudioEnabledAction");
        viewAudioEnabledAction_->setCheckable(true);
        viewAudioEnabledAction_->setChecked(playbackController_->isAudioEnabled());
        connect(viewAudioEnabledAction_, &QAction::toggled, playbackController_,
                &PlaybackController::setAudioEnabled);
    }
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
    openSourceLicensesAction_ = helpMenu.addAction(tr("Open Source Licenses…"));
    openSourceLicensesAction_->setObjectName("openSourceLicensesAction");
    connect(openSourceLicensesAction_, &QAction::triggered, this, [this] {
        // One window: asking again raises the open one.
        if (!licensesWindow_) {
            licensesWindow_ = new LicensesWindow(*this);
            licensesWindow_->setAttribute(Qt::WA_DeleteOnClose);
        }
        licensesWindow_->show();
        licensesWindow_->raise();
        licensesWindow_->activateWindow();
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
    cancelOpenAction_ = fileMenu.addAction(tr("Cancel Open"));
    cancelOpenAction_->setObjectName(QStringLiteral("cancelOpenAction"));
    connect(cancelOpenAction_, &QAction::triggered, &projectHost_, &ProjectHost::cancelOpen);

    auto* importAction = fileMenu.addAction(tr("&Import…"));
    importAction->setObjectName(QStringLiteral("importAssetsAction"));
    connect(importAction, &QAction::triggered, this, [this] {
        if (auto* assets = compositionSession_.assetController())
            assets->requestImport(this);
    });
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

    projectColorSettingsAction_ = fileMenu.addAction(tr("Project Settings…"));
    projectColorSettingsAction_->setObjectName(QStringLiteral("projectColorSettingsAction"));
    connect(projectColorSettingsAction_, &QAction::triggered, this,
            &MainWindow::showProjectColorSettings);

    fileMenu.addSeparator();

    // ADR 0033: the export queue. The job's own range chooses a frame, a range, the work area or the
    // whole composition, with any preset.
    addToExportQueueAction_ = fileMenu.addAction(tr("Add to Export &Queue"));
    addToExportQueueAction_->setObjectName(QStringLiteral("addToExportQueueAction"));
    addToExportQueueAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    addToExportQueueAction_->setShortcutContext(Qt::WindowShortcut);
    addToExportQueueAction_->setEnabled(false);
    connect(addToExportQueueAction_, &QAction::triggered, this, [this] {
        if (exportQueue_ == nullptr)
            return;
        const auto result =
            exportQueue_->addCompositionToQueue(compositionSession_.compositionId());
        if (!result.accepted && statusStrip_ != nullptr && !result.reason.isEmpty())
            statusStrip_->showTransientMessage(result.reason);
    });

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

void MainWindow::showProjectColorSettings() { showProjectColorSettingsDialog(projectHost_, *this); }

void MainWindow::showPreferences() {
    // The window edits a value, not QSettings; the commit path here is the only writer, so a
    // cancelled dialog leaves the stored preferences untouched.
    const QSettings currentSettings;
    // Modal and synchronous (exec): the commit path is the preferencesApplied connection below.
    PreferencesWindow dialog(*this, loadApplicationPreferences(currentSettings),
                             accelerationStatus_);
    connect(&dialog, &PreferencesWindow::preferencesApplied, this,
            [this](const ApplicationPreferences& preferences) {
                QSettings settings;
                saveApplicationPreferences(settings, preferences);
                settings.sync();
                applyPreferencesToOpenEditors(preferences);
                emit preferencesChanged();
            });
    connect(&dialog, &PreferencesWindow::soundTestRequested, this, &MainWindow::soundTestRequested);
    connect(&dialog, &PreferencesWindow::mcpSettingsRequested, this, [this, &dialog] {
        dialog.reject();
        if (mcpServer_ != nullptr)
            mcpServer_->requestReveal();
        else
            statusStrip_->showTransientMessage(tr("The MCP server is not available in this build"));
    });
    dialog.exec();
}

void MainWindow::applyPreferencesToOpenEditors(const ApplicationPreferences& preferences) {
    // Editors are constructed with only their session and controller, not a preferences reference,
    // so the window reaches the live ones through the widget tree. A widget that does not implement
    // PreferencesAware is skipped; new editors built after this point read the saved value at
    // construction.
    const auto descendants = findChildren<QWidget*>();
    for (QWidget* widget : descendants) {
        if (auto* aware = dynamic_cast<PreferencesAware*>(widget); aware != nullptr) {
            aware->applyApplicationPreferences(preferences);
        }
    }
    // The audio View action is a QAction, not a PreferencesAware widget, so the loop above never
    // reaches it; without this it would keep reporting the pre-commit check state and the next
    // click would toggle from stale state. Mirror the committed preference with signals blocked so
    // no duplicate playback command runs here; the app's preferencesChanged handler owns the
    // playback engine update.
    if (viewAudioEnabledAction_ != nullptr &&
        viewAudioEnabledAction_->isChecked() != preferences.audioEnabled) {
        const QSignalBlocker blocker(viewAudioEnabledAction_);
        viewAudioEnabledAction_->setChecked(preferences.audioEnabled);
    }
}

void MainWindow::applyCommittedPreferencesForTest(const ApplicationPreferences& preferences) {
    applyPreferencesToOpenEditors(preferences);
}

void MainWindow::updateFileActions() {
    const bool busy = projectHost_.isBusy();
    if (auto* import = findChild<QAction*>("importAssetsAction"))
        import->setEnabled(projectHost_.canSave());
    newProjectAction_->setEnabled(!busy);
    openProjectAction_->setEnabled(!busy);
    cancelOpenAction_->setEnabled(projectHost_.activity() == ProjectHostActivity::Opening);
    saveProjectAction_->setEnabled(!busy && projectHost_.canSave());
    saveProjectAsAction_->setEnabled(!busy && projectHost_.canSave());
    saveProjectCopyAction_->setEnabled(!busy && projectHost_.canSaveCopy());

    switch (projectHost_.activity()) {
    case ProjectHostActivity::Saving:
        statusStrip_->setPersistentMessage(tr("Saving…"));
        break;
    case ProjectHostActivity::Opening:
        statusStrip_->setPersistentMessage(tr("Opening…"));
        break;
    case ProjectHostActivity::ResolvingUnsavedChanges:
        statusStrip_->setPersistentMessage(tr("Waiting for a decision about unsaved changes…"));
        break;
    case ProjectHostActivity::Idle:
        statusStrip_->setPersistentMessage({});
        break;
    }
}

void MainWindow::setExportQueue(ExportQueueAdapter* const adapter) {
    if (exportQueue_ == adapter)
        return;
    if (exportQueue_ != nullptr)
        disconnect(exportQueue_, nullptr, this, nullptr);
    exportQueue_ = adapter;
    if (adapter != nullptr) {
        connect(adapter, &ExportQueueAdapter::queueChanged, this,
                &MainWindow::updateExportQueueActions);
        connect(adapter, &ExportQueueAdapter::revealRequested, this,
                &MainWindow::revealExportPanel);
        connect(adapter, &ExportQueueAdapter::remedyRequested, this, [this](const int remedy) {
            switch (static_cast<host::ExportRemedy>(remedy)) {
            case host::ExportRemedy::InstallOpenH264:
            case host::ExportRemedy::LocateOpenH264:
                if (h264Encoder_ != nullptr)
                    showH264EncoderFlow(*this, *h264Encoder_);
                break;
            case host::ExportRemedy::OpenProjectSettings:
                showProjectColorSettings();
                break;
            default:
                break;
            }
        });
        connect(&compositionSession_, &CompositionSession::compositionChanged, this,
                &MainWindow::updateExportQueueActions);
        connect(&projectHost_, &ProjectHost::sessionReplaced, this,
                &MainWindow::updateExportQueueActions);
    }
    updateExportQueueActions();
}

void MainWindow::updateExportQueueActions() {
    if (addToExportQueueAction_ == nullptr)
        return;
    QString reason;
    if (exportQueue_ == nullptr)
        reason = tr("The export queue is not available");
    else if (isShowingReadOnlyPlaceholder())
        reason = tr("This project is open read-only");
    else if (!compositionSession_.compositionId().isValid())
        reason = tr("Open a composition to add it to the export queue");
    else
        reason = exportQueue_->addUnavailableReason();
    addToExportQueueAction_->setEnabled(reason.isEmpty());
    addToExportQueueAction_->setToolTip(reason.isEmpty() ? tr("Add the composition to the export queue")
                                                         : reason);
}

void MainWindow::revealExportPanel() {
    if (workspaceHost_ == nullptr)
        return;
    if (workspaceHost_->revealEditor("bloom.export") == nullptr && statusStrip_ != nullptr)
        statusStrip_->showTransientMessage(tr("The Export panel could not be shown"));
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
    // The bar authors through the session, which a read-only placeholder leaves bound to its stale
    // document: it is disabled while the placeholder is authoritative.
    contextBar_->setEnabled(!readOnly);
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
    workspacePresets_ = new WorkspacePresets(*workspaceHost_, this);
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

    // Task VIEW-1: the window status bar is a row of the central column, below the workspace and
    // below the read-only placeholder alike -- it reports on the application, so it stays visible
    // whichever page is authoritative.
    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("mainWindowCentralColumn"));
    auto* column = new QVBoxLayout(central);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    statusStrip_ = new WindowStatusBar(compositionSession_, previewController_, mediaDiskCache_,
                                       operationCache_, central);
    // ADR 0031: the contextual bar is the first row under the menu bar, above both pages. It is a
    // widget of the central column rather than a QToolBar, so the native macOS menu bar is
    // untouched and the bar can never be docked, floated or moved.
    contextBar_ = new ContextBar(compositionSession_, previewController_, central);
    connect(contextBar_, &ContextBar::notice, statusStrip_,
            [this](const QString& message) { statusStrip_->showTransientMessage(message); });
    contextBar_->setWorkspacePresets(workspacePresets_);
    column->addWidget(contextBar_);
    column->addWidget(centralStack_, 1);
    column->addWidget(statusStrip_);
    setCentralWidget(central);
    qApp->installEventFilter(this);
    scheduleDisplayedViewerUpdate();
}

QWidget* MainWindow::createReadOnlyPlaceholderPage() {
    auto* page = new QWidget(this);
    page->setObjectName("readOnlyPlaceholderPage");

    auto* heading = new kit::KLabel(tr("Read-only project"), page, kit::TypeRole::Title);
    heading->setObjectName("readOnlyPlaceholderHeading");

    readOnlyPlaceholderFileNameLabel_ = new kit::KLabel(page);
    readOnlyPlaceholderFileNameLabel_->setObjectName("readOnlyPlaceholderFileName");

    readOnlyPlaceholderBodyLabel_ = new kit::KLabel(page);
    readOnlyPlaceholderBodyLabel_->setObjectName("readOnlyPlaceholderBody");
    readOnlyPlaceholderBodyLabel_->setWordWrap(true);
    readOnlyPlaceholderBodyLabel_->setMaximumWidth(kit::px(kit::Size::DialogTextWidth));

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(kit::px(kit::Spacing::XXL), kit::px(kit::Spacing::XXL),
                               kit::px(kit::Spacing::XXL), kit::px(kit::Spacing::XXL));
    layout->addStretch(1);
    layout->addWidget(heading);
    layout->addWidget(readOnlyPlaceholderFileNameLabel_);
    layout->addWidget(readOnlyPlaceholderBodyLabel_);
    layout->addStretch(2);
    return page;
}

void MainWindow::resetCompositingLayout(const bool persist) {
    if (persist) {
        // TimelineEditor owns the divider's live geometry. Removing its persisted width before
        // rebuilding the tree makes the new editor apply its 37% default, while the ordinary
        // constructor path leaves an existing artist width untouched.
        QSettings settings;
        settings.remove(QLatin1StringView(timelineLayerColumnWidthKey));
    }

    // The first-run and Reset Workspace arrangement is the Default preset (owner, 2026-10-31). The
    // startup and migration paths do not overwrite the stored active preset; Reset does.
    workspacePresets_->apply(QStringLiteral("Default"), persist);
    workspaceLayoutWritable_ = true;

    // The Timeline's 37% divider default is applied at show time, but the Timeline is nested inside
    // a splitter whose weights settle on a later event turn, so its show-time width can precede its
    // final extent. Re-apply the ratio once geometry is final, before the deferred persist below
    // records the width.
    QTimer::singleShot(0, workspaceHost_, [this] {
        if (auto* timeline = workspaceHost_->findChild<TimelineEditor*>())
            timeline->applyDefaultLayerColumnWidth();
    });

    if (persist) {
        const auto persistResetState = [this] {
            QSettings settings;
            workspaceHost_->persistLayout(settings, QLatin1StringView(workspaceLayoutKey));
            if (auto* timeline = workspaceHost_->findChild<TimelineEditor*>())
                timeline->persistLayerColumnWidth();
        };
        persistResetState();
        // replaceRoot() has the tree in the host immediately, but QSplitter's final sizes are
        // resolved after the host's layout pass. Persist once more on the next turn so Reset
        // records the ratio-based sizes rather than construction-time placeholders.
        QTimer::singleShot(0, workspaceHost_, persistResetState);
    }
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
    contextBarAction_ = windowMenu_->addAction(tr("Context Bar"));
    contextBarAction_->setObjectName(QStringLiteral("windowContextBarAction"));
    contextBarAction_->setCheckable(true);
    contextBarAction_->setChecked(true);
    connect(contextBarAction_, &QAction::toggled, contextBar_, &QWidget::setVisible);

    windowMenu_->addSeparator();
    auto* resetLayoutAction = windowMenu_->addAction("Reset Workspace");
    resetLayoutAction->setObjectName("resetWorkspaceAction");
    connect(resetLayoutAction, &QAction::triggered, this, [this] {
        resetCompositingLayout(true);
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

void MainWindow::setMcpServer(McpServerAdapter* const adapter) {
    if (mcpServer_ == adapter)
        return;
    if (mcpServer_ != nullptr)
        disconnect(mcpServer_, nullptr, this, nullptr);
    mcpServer_ = adapter;
    if (adapter != nullptr)
        connect(adapter, &McpServerAdapter::revealRequested, this,
                &MainWindow::revealMcpServerPanel);
    statusStrip_->setMcpServer(adapter);
}

void MainWindow::revealMcpServerPanel() {
    if (workspaceHost_ == nullptr)
        return;
    if (workspaceHost_->revealEditor("bloom.mcp-server") == nullptr && statusStrip_ != nullptr)
        statusStrip_->showTransientMessage(tr("The MCP Server panel could not be shown"));
}

void MainWindow::updateWorkspaceActions() {
    scheduleDisplayedViewerUpdate();
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

void MainWindow::scheduleDisplayedViewerUpdate() {
    if (!displayedViewerUpdateTimer_.isActive())
        displayedViewerUpdateTimer_.start(0);
}

void MainWindow::updateDisplayedViewer() {
    if (!statusStrip_ || !workspaceHost_)
        return;
    auto* selected = displayedViewer();
    statusStrip_->setDisplayedViewer(selected);
    contextBar_->setViewer(selected);
}

ViewerEditor* MainWindow::displayedViewer() {
    if (!workspaceHost_)
        return nullptr;
    auto viewers = workspaceHost_->findChildren<ViewerEditor*>();
    viewers.erase(std::remove_if(viewers.begin(), viewers.end(),
                                 [](const auto* viewer) { return !viewer->isVisible(); }),
                  viewers.end());
    const auto areaId = [](const ViewerEditor* viewer) {
        for (auto* ancestor = viewer->parentWidget(); ancestor; ancestor = ancestor->parentWidget())
            if (const auto* area = qobject_cast<EditorArea*>(ancestor))
                return area->areaId();
        return QString{};
    };
    std::sort(viewers.begin(), viewers.end(),
              [&](const auto* a, const auto* b) { return areaId(a) < areaId(b); });
    ViewerEditor* selected = nullptr;
    if (auto* active = workspaceHost_->activeArea()) {
        auto* viewer = active->findChild<ViewerEditor*>();
        if (viewer && viewers.contains(viewer)) {
            selected = viewer;
            lastActiveViewer_ = viewer;
        }
    }
    if (!selected && lastActiveViewer_ && viewers.contains(lastActiveViewer_.data()))
        selected = lastActiveViewer_;
    if (!selected && !viewers.empty())
        selected = viewers.front();
    return selected;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (workspaceHost_ &&
        (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
         event->type() == QEvent::ChildAdded || event->type() == QEvent::ChildRemoved)) {
        const auto* widget = qobject_cast<QWidget*>(watched);
        if (widget && (widget == workspaceHost_ || workspaceHost_->isAncestorOf(widget)))
            scheduleDisplayedViewerUpdate();
    }
    return QMainWindow::eventFilter(watched, event);
}

} // namespace bloom::ui
