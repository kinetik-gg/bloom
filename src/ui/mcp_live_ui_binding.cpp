#include <bloom/ui/mcp_live_ui_binding.hpp>

#include "composition_editor_support.hpp"

#include <bloom/document/project.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QApplication>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <set>
#include <system_error>

namespace bloom::ui {

using mcp::McpUiRefusal;

struct McpLiveUiBinding::Pending {
    mcp::McpProjectRequestKind kind = mcp::McpProjectRequestKind::Save;
    std::function<void(mcp::McpProjectResult)> done;
    // Set while the export queue's jobs are stopping before the request goes on.
    std::function<void()> afterJobs;
    std::chrono::steady_clock::time_point jobsDeadline{};
};

namespace {

constexpr std::chrono::seconds kJobsStopTimeout{15};

mcp::McpUiResult uiResult(const McpUiRefusal refusal, const QString& message) {
    return {.refusal = refusal, .message = message.toStdString()};
}

mcp::McpUiResult uiOk(const QString& message) { return uiResult(McpUiRefusal::None, message); }

bool isBloomFile(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension == ".bloom";
}

QString quoted(const QString& name) { return QStringLiteral("'%1'").arg(name); }

} // namespace

McpLiveUiBinding::McpLiveUiBinding(ProjectHost& project, CompositionSession& session,
                                   PlaybackController& playback, QObject* const parent)
    : QObject(parent), project_(project), session_(session), playback_(playback),
      jobsTimer_(new QTimer(this)) {
    jobsTimer_->setInterval(25);
    connect(jobsTimer_, &QTimer::timeout, this, &McpLiveUiBinding::pollJobs);
    connect(&project_, &ProjectHost::saveFinished, this,
            [this](const ProjectHostOperationOutcome outcome, const QString& message) {
                onSaveFinished(outcome, message);
            });
    connect(&project_, &ProjectHost::openFinished, this,
            [this](const ProjectHostOperationOutcome outcome, const QString& message) {
                onOpenFinished(outcome, message);
            });
}

McpLiveUiBinding::~McpLiveUiBinding() = default;

void McpLiveUiBinding::setWindow(MainWindow* const window) { window_ = window; }

mcp::McpLiveUiPort McpLiveUiBinding::port() {
    return {
        .revealComposition =
            [this](const document::CompositionId composition,
                   const std::optional<core::RationalTime> time) {
                return revealComposition(composition, time);
            },
        .projectLifecycle =
            [this](mcp::McpProjectRequest request,
                   std::function<void(mcp::McpProjectResult)> done) {
                projectLifecycle(std::move(request), std::move(done));
            },
        .setSelection = [this](const mcp::McpSelectionRequest& request) {
            return setSelection(request);
        },
        .activateComposition =
            [this](const document::CompositionId composition) {
                return activateComposition(composition);
            },
        .transport = [this](const mcp::McpTransportRequest& request) {
            return transport(request);
        },
        .setViewer = [this](const mcp::McpViewerRequest& request) { return setViewer(request); },
        .setColorSettings =
            [this](const document::ColorSettings& settings) { return setColorSettings(settings); },
    };
}

// ---- editor.reveal and composition.activate ----------------------------------------------------

bool McpLiveUiBinding::revealComposition(const document::CompositionId composition,
                                         const std::optional<core::RationalTime> time) {
    if (!window_ || window_->workspaceHost() == nullptr)
        return false;
    const auto* found = session_.snapshot().project().findComposition(composition);
    if (found == nullptr)
        return false;
    if (time && (*time < core::RationalTime::fromInteger(0) || !(*time < found->duration())))
        return false;
    if (!session_.setComposition(composition))
        return false;
    auto* workspace = window_->workspaceHost();
    if (workspace->revealEditor("bloom.viewer") == nullptr)
        return false;
    // The Timeline is where a new area is split off, so reveal it last: the Viewer keeps its place.
    if (workspace->revealEditor("bloom.timeline") == nullptr)
        return false;
    if (time) {
        playback_.pause();
        (void)session_.setCurrentTime(*time);
    }
    return true;
}

mcp::McpUiResult McpLiveUiBinding::activateComposition(const document::CompositionId composition) {
    const auto* found = session_.snapshot().project().findComposition(composition);
    if (found == nullptr)
        return uiResult(McpUiRefusal::UnknownTarget,
                        tr("This project has no composition with that id."));
    if (!session_.setComposition(composition))
        return uiResult(McpUiRefusal::Failed, tr("The composition could not be made active."));
    return uiOk(tr("%1 is the active composition.").arg(quoted(QString::fromStdString(found->name()))));
}

// ---- selection.set -----------------------------------------------------------------------------

mcp::McpUiResult McpLiveUiBinding::setSelection(const mcp::McpSelectionRequest& request) {
    const auto* target = session_.snapshot().project().findComposition(request.composition);
    if (target == nullptr)
        return uiResult(McpUiRefusal::UnknownTarget,
                        tr("This project has no composition with that id."));
    const bool clearing = request.mode == mcp::McpSelectionMode::Clear;
    // A selection is of the active composition, as every editor shows it; making it active first is
    // what choosing it in the Assets panel does.
    if (request.composition != session_.compositionId() &&
        !session_.setComposition(request.composition))
        return uiResult(McpUiRefusal::Failed, tr("The composition could not be made active."));
    if (clearing) {
        session_.clearSelection();
        return uiOk(tr("Nothing is selected."));
    }

    const auto& graph = target->graph();
    std::vector<document::NodeId> layerNodes;
    for (const auto layer : request.layers) {
        const auto boundary = session_.boundaryNodeForLayer(layer);
        if (graph.findLayer(layer) == nullptr || !boundary)
            return uiResult(McpUiRefusal::UnknownTarget,
                            tr("The composition has no layer %1.").arg(layer.value()));
        layerNodes.push_back(*boundary);
    }
    for (const auto node : request.nodes)
        if (graph.findNode(node) == nullptr)
            return uiResult(McpUiRefusal::UnknownTarget,
                            tr("The composition has no node %1.").arg(node.value()));

    using Mode = mcp::McpSelectionMode;
    const bool layersOnly = request.nodes.empty() && !request.layers.empty();
    if (layersOnly && request.mode != Mode::Remove) {
        // Exactly the Timeline's own clicks: the first layer replaces, the rest extend.
        bool extend = request.mode == Mode::Add;
        for (const auto layer : request.layers) {
            session_.selectLayer(layer, extend);
            extend = true;
        }
    } else {
        std::set<document::NodeId> wanted(layerNodes.begin(), layerNodes.end());
        wanted.insert(request.nodes.begin(), request.nodes.end());
        std::set<document::NodeId> next;
        if (request.mode == Mode::Set) {
            next = wanted;
        } else if (request.mode == Mode::Add) {
            next = session_.selectedNodes();
            next.insert(wanted.begin(), wanted.end());
        } else {
            next = session_.selectedNodes();
            for (const auto node : wanted)
                next.erase(node);
        }
        if (next.empty()) {
            session_.clearSelection();
        } else {
            // The newest request is the primary one, as the last click is; otherwise the current
            // primary stays while it is still selected.
            document::NodeId primary = *next.begin();
            if (const auto* current = session_.selectedNode(); current && next.contains(current->id))
                primary = current->id;
            if (request.mode != Mode::Remove) {
                if (!request.nodes.empty())
                    primary = request.nodes.back();
                else if (!layerNodes.empty())
                    primary = layerNodes.back();
            }
            session_.selectNodes(std::move(next), primary);
        }
    }
    const auto count = static_cast<int>(session_.selectedNodes().size());
    return uiOk(count == 0 ? tr("Nothing is selected.") : tr("%n item(s) selected.", nullptr, count));
}

// ---- transport ---------------------------------------------------------------------------------

mcp::McpUiResult McpLiveUiBinding::transport(const mcp::McpTransportRequest& request) {
    using Action = mcp::McpTransportAction;
    const auto context = frameContextFor(session_);
    if (!context)
        return uiResult(McpUiRefusal::InvalidArgument,
                        tr("There is no active composition to play. Activate one first."));
    const auto seekTo = [&](const std::uint64_t frame) {
        const auto time = frameTimeForIndex(context->frameRate, context->duration, frame);
        if (!time)
            return uiResult(McpUiRefusal::Failed, tr("That frame cannot be shown."));
        // The footer pauses first, then writes the exact time.
        playback_.pause();
        (void)session_.setCurrentTime(*time);
        return uiOk(tr("The playhead is at frame %1.").arg(frame));
    };
    switch (request.action) {
    case Action::Seek: {
        if (!request.time)
            return uiResult(McpUiRefusal::InvalidArgument, tr("Seek needs a time."));
        const auto time = *request.time;
        if (time < core::RationalTime::fromInteger(0) || !(time < context->duration))
            return uiResult(McpUiRefusal::InvalidArgument,
                            tr("That time is outside the composition (0 up to %1 seconds).")
                                .arg(context->duration.toSeconds(), 0, 'f', 3));
        playback_.pause();
        (void)session_.setCurrentTime(time);
        return uiOk(tr("The playhead moved."));
    }
    case Action::Play:
        playback_.play();
        return uiOk(tr("Playing."));
    case Action::Stop:
        playback_.pause();
        return uiOk(tr("Stopped."));
    case Action::StepForward:
    case Action::StepBackward: {
        const auto nearest = nearestFrameIndexForTime(context->frameRate, context->duration,
                                                      session_.currentTime());
        if (!nearest)
            return uiResult(McpUiRefusal::Failed, tr("The playhead has no frame to step from."));
        std::uint64_t target = *nearest;
        if (request.action == Action::StepBackward)
            target = target > 0 ? target - 1 : 0;
        else
            target = std::min(target + 1, context->maxFrameIndexValue);
        return seekTo(target);
    }
    case Action::GoToStart:
        return seekTo(0);
    case Action::GoToEnd:
        return seekTo(context->maxFrameIndexValue);
    }
    return uiResult(McpUiRefusal::InvalidArgument, tr("Unknown transport action."));
}

// ---- viewer.set --------------------------------------------------------------------------------

mcp::McpUiResult McpLiveUiBinding::setViewer(const mcp::McpViewerRequest& request) {
    auto* viewer = window_ ? window_->displayedViewer() : nullptr;
    if (viewer == nullptr)
        return uiResult(McpUiRefusal::UnknownTarget,
                        tr("There is no Viewer to change. Show one with editor.reveal."));
    ViewerEditor::RemoteChange change;
    change.display = request.display;
    change.view = request.view;
    change.exposure = request.exposure;
    if (request.channel)
        change.channel = static_cast<ViewerChannel>(*request.channel);
    if (request.zoom) {
        switch (*request.zoom) {
        case mcp::McpViewerZoom::Fit:
            change.zoom = ViewerEditor::RemoteChange::Zoom::Fit;
            break;
        case mcp::McpViewerZoom::ActualSize:
            change.zoom = ViewerEditor::RemoteChange::Zoom::ActualSize;
            break;
        case mcp::McpViewerZoom::Factor:
            if (!request.zoomFactor)
                return uiResult(McpUiRefusal::InvalidArgument, tr("A zoom factor needs a value."));
            change.zoom = ViewerEditor::RemoteChange::Zoom::Factor;
            change.zoomFactor = *request.zoomFactor;
            break;
        }
    }
    const auto refusal = viewer->applyRemoteChange(change);
    if (!refusal.isEmpty())
        return uiResult(McpUiRefusal::InvalidArgument, refusal);
    return uiOk(tr("The Viewer was updated."));
}

// ---- project.set-color-settings ----------------------------------------------------------------

mcp::McpUiResult McpLiveUiBinding::setColorSettings(const document::ColorSettings& settings) {
    using Status = host::ProjectSessionColorSettingsStatus;
    if (project_.isBusy() || pending_)
        return uiResult(McpUiRefusal::Busy,
                        tr("Bloom is busy with a project operation. Try again in a moment."));
    switch (project_.setColorSettings(settings)) {
    case Status::Updated:
        return uiOk(tr("The project color settings changed."));
    case Status::NoChange:
        return uiOk(tr("The project already uses these color settings."));
    case Status::ReadOnly:
        return uiResult(McpUiRefusal::ReadOnly,
                        tr("%1 is read-only, so its color settings cannot change.")
                            .arg(quoted(projectName())));
    case Status::InvalidSettings:
        return uiResult(McpUiRefusal::InvalidArgument,
                        tr("Those color settings cannot be used."));
    case Status::InvalidSession:
        break;
    }
    return uiResult(McpUiRefusal::Failed, tr("There is no project to change."));
}

// ---- project lifecycle -------------------------------------------------------------------------

QString McpLiveUiBinding::projectName() const {
    const auto path = project_.displayPath();
    return path ? QString::fromStdString(path->stem().string()) : tr("Untitled");
}

mcp::McpProjectResult McpLiveUiBinding::projectResult(const McpUiRefusal refusal,
                                                      const QString& message) const {
    mcp::McpProjectResult result;
    result.refusal = refusal;
    result.message = message.toStdString();
    if (const auto path = project_.displayPath())
        result.path = *path;
    result.projectName = projectName().toStdString();
    return result;
}

std::optional<mcp::McpProjectResult>
McpLiveUiBinding::refuseUnsettled(const mcp::McpProjectRequest& request) const {
    if (project_.isDirty() && !request.discardChanges)
        return projectResult(
            McpUiRefusal::UnsavedChanges,
            tr("%1 has unsaved changes. Save it first, or pass discardChanges to throw them away.")
                .arg(quoted(projectName())));
    const auto* queue = project_.exportQueue();
    const auto jobs = queue != nullptr ? queue->activeJobCount() : std::size_t{0};
    if (jobs > 0 && !request.stopJobs)
        return projectResult(
            McpUiRefusal::JobsRunning,
            tr("%n export job(s) are running. Wait for them, or pass stopJobs to stop them.",
               nullptr, static_cast<int>(jobs)));
    return std::nullopt;
}

void McpLiveUiBinding::projectLifecycle(mcp::McpProjectRequest request,
                                        std::function<void(mcp::McpProjectResult)> done) {
    using Kind = mcp::McpProjectRequestKind;
    const auto refuse = [&](const McpUiRefusal refusal, const QString& message) {
        done(projectResult(refusal, message));
    };
    if (pending_ || project_.isBusy()) {
        refuse(McpUiRefusal::Busy,
               tr("Bloom is busy with another project operation. Try again in a moment."));
        return;
    }
    if (QApplication::activeModalWidget() != nullptr) {
        refuse(McpUiRefusal::Busy, tr("A dialog is open in Bloom. Close it, then try again."));
        return;
    }

    const auto checkPath = [&](const bool mustExist) -> std::optional<QString> {
        const auto& path = request.path;
        if (path.empty() || !path.is_absolute() || !isBloomFile(path))
            return tr("The path must be an absolute path to a .bloom file.");
        std::error_code error;
        if (mustExist) {
            if (!std::filesystem::is_regular_file(path, error))
                return tr("There is no project file at %1.")
                    .arg(QString::fromStdString(path.generic_string()));
        } else if (!std::filesystem::is_directory(path.parent_path(), error)) {
            return tr("The folder %1 does not exist.")
                .arg(QString::fromStdString(path.parent_path().generic_string()));
        }
        return std::nullopt;
    };
    const auto readOnly = [&] {
        refuse(McpUiRefusal::ReadOnly,
               tr("%1 is read-only, so it cannot be saved.").arg(quoted(projectName())));
    };

    switch (request.kind) {
    case Kind::Save:
        if (!project_.canSave())
            return readOnly();
        if (!project_.displayPath()) {
            refuse(McpUiRefusal::NoPath,
                   tr("%1 has never been saved. Use project.save-as with a path.")
                       .arg(quoted(projectName())));
            return;
        }
        beginPending(request.kind, std::move(done));
        project_.beginSave();
        return;
    case Kind::SaveAs: {
        if (const auto bad = checkPath(false)) {
            refuse(McpUiRefusal::PathInvalid, *bad);
            return;
        }
        if (!project_.canSave())
            return readOnly();
        std::error_code error;
        if (std::filesystem::exists(request.path, error) && !request.replace) {
            refuse(McpUiRefusal::DestinationExists,
                   tr("%1 already exists. Pass replace to overwrite it.")
                       .arg(QString::fromStdString(request.path.generic_string())));
            return;
        }
        beginPending(request.kind, std::move(done));
        project_.beginSaveAs(request.path);
        return;
    }
    case Kind::Open:
    case Kind::New:
    case Kind::Close: {
        if (request.kind == Kind::Open)
            if (const auto bad = checkPath(true)) {
                refuse(McpUiRefusal::PathInvalid, *bad);
                return;
            }
        if (const auto refusal = refuseUnsettled(request)) {
            done(*refusal);
            return;
        }
        beginPending(request.kind, std::move(done));
        settleThenRun(request, [this, request] {
            if (request.kind == Kind::Open) {
                project_.beginOpen(request.path);
                return;
            }
            if (!project_.replaceWithBlankProject()) {
                finishPending(McpUiRefusal::Busy,
                              tr("Bloom is busy with another project operation."));
                return;
            }
            finishPending(McpUiRefusal::None,
                          request.kind == Kind::New ? tr("A new untitled project is open.")
                                                    : tr("The project was closed; Bloom shows an "
                                                         "empty project."));
        });
        return;
    }
    }
}

void McpLiveUiBinding::beginPending(const mcp::McpProjectRequestKind kind,
                                    std::function<void(mcp::McpProjectResult)> done) {
    pending_ = std::make_unique<Pending>();
    pending_->kind = kind;
    pending_->done = std::move(done);
    // The window shows no dialog for a call an agent made (ProjectHost::agentOperation()).
    project_.setAgentOperation(true);
}

void McpLiveUiBinding::settleThenRun(const mcp::McpProjectRequest&, std::function<void()> run) {
    auto* queue = project_.exportQueue();
    if (queue == nullptr || queue->activeJobCount() == 0) {
        run();
        return;
    }
    // stopJobs was asked for: stop through the queue, then go on once it has let go.
    (void)queue->stopAll();
    pending_->afterJobs = std::move(run);
    pending_->jobsDeadline = std::chrono::steady_clock::now() + kJobsStopTimeout;
    jobsTimer_->start();
}

void McpLiveUiBinding::pollJobs() {
    if (!pending_ || !pending_->afterJobs) {
        jobsTimer_->stop();
        return;
    }
    auto* queue = project_.exportQueue();
    if (queue != nullptr)
        queue->pump();
    const bool stopped = queue == nullptr || queue->activeJobCount() == 0;
    if (!stopped && std::chrono::steady_clock::now() < pending_->jobsDeadline)
        return;
    jobsTimer_->stop();
    if (!stopped) {
        finishPending(McpUiRefusal::Failed,
                      tr("The export jobs did not stop in time, so the project was left as it is."));
        return;
    }
    auto run = std::move(pending_->afterJobs);
    pending_->afterJobs = nullptr;
    run();
}

void McpLiveUiBinding::finishPending(const McpUiRefusal refusal, const QString& message) {
    if (!pending_)
        return;
    project_.setAgentOperation(false);
    const auto pending = std::move(pending_);
    pending->done(projectResult(refusal, message));
}

void McpLiveUiBinding::onSaveFinished(const ProjectHostOperationOutcome outcome,
                                      const QString& message) {
    if (!pending_ || (pending_->kind != mcp::McpProjectRequestKind::Save &&
                      pending_->kind != mcp::McpProjectRequestKind::SaveAs))
        return;
    const bool saved = outcome == ProjectHostOperationOutcome::Published ||
                       outcome == ProjectHostOperationOutcome::PublishedWithWarning;
    finishPending(saved ? McpUiRefusal::None : McpUiRefusal::Failed,
                  saved ? tr("Saved %1. %2").arg(quoted(projectName()), message) : message);
}

void McpLiveUiBinding::onOpenFinished(const ProjectHostOperationOutcome outcome,
                                      const QString& message) {
    if (!pending_ || pending_->kind != mcp::McpProjectRequestKind::Open)
        return;
    const bool opened = outcome == ProjectHostOperationOutcome::Published ||
                        outcome == ProjectHostOperationOutcome::PublishedWithWarning;
    finishPending(opened ? McpUiRefusal::None : McpUiRefusal::Failed,
                  opened ? tr("Opened %1.").arg(quoted(projectName())) : message);
}

} // namespace bloom::ui
