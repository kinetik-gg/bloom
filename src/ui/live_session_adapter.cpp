// The UI thread's side of the shared live session (ADR 0034, section 4). Moved out of the Script
// editor's private runtime so the Script editor and MCP share one mirror and one commit marshal.
#include <bloom/ui/live_session_adapter.hpp>

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/project_host.hpp>

namespace bloom::ui {
namespace {

using Result = scripting::SessionCommandResult;

Result completed(commands::CommandResult result) {
    return {.status = host::ProjectSessionCommandStatus::Completed,
            .command = std::move(result),
            .diagnostic = std::nullopt};
}

// A commit held by an open gesture is retried at this cadence until the gesture ends (the gesture
// signals also retry at once); its deadline withdraws it otherwise.
constexpr auto kGestureRetry = std::chrono::milliseconds{50};

} // namespace

LiveSessionAdapter::LiveSessionAdapter(scripting::LiveSessionHost& host,
                                       CompositionSession& session, ProjectHost& project,
                                       QObject* parent)
    : QObject(parent), host_(host), session_(session), project_(project),
      committer_(std::make_unique<scripting::LiveSessionCommitter>()) {
    committer_->execute = [this](commands::Transaction transaction) {
        return completed(session_.executeTransaction(std::move(transaction)));
    };
    committer_->history = [this](const bool redo) {
        auto* stack = project_.liveDocumentAndStack().second;
        return stack ? completed(redo ? stack->redo() : stack->undo()) : Result{};
    };
    committer_->gestureOpen = [this] {
        return session_.transformInteractionActive() || session_.valueEditActive();
    };
    committer_->revision = [this] { return session_.snapshot().revision(); };

    replace();
    // Rebind refreshes can precede the project host's later sessionReplaced slots: revoke old
    // reads and queued commits before any of those refreshes can publish the new snapshot.
    connect(&session_, &CompositionSession::documentRebound, this, &LiveSessionAdapter::revoke);
    connect(&session_, &CompositionSession::snapshotChanged, this, &LiveSessionAdapter::publish);
    connect(&session_, &CompositionSession::compositionChanged, this, &LiveSessionAdapter::publish);
    connect(&session_, &CompositionSession::selectionChanged, this, &LiveSessionAdapter::publish);
    connect(&session_, &CompositionSession::currentTimeChanged, this, &LiveSessionAdapter::publish);
    connect(&project_, &ProjectHost::sessionReplaced, this, &LiveSessionAdapter::replace);
    connect(&project_, &ProjectHost::colorSettingsChanged, this, &LiveSessionAdapter::publish);
    connect(&project_, &ProjectHost::saveFinished, this, [this] { publish(); });
    // A commit waiting for a gesture applies as soon as the gesture ends.
    connect(&session_, &CompositionSession::transformInteractionChanged, this,
            &LiveSessionAdapter::scheduleDrain);
    connect(&session_, &CompositionSession::liveValueChanged, this,
            &LiveSessionAdapter::scheduleDrain);
    if (auto* application = QCoreApplication::instance())
        connect(application, &QCoreApplication::aboutToQuit, this, &LiveSessionAdapter::shutdown);
    // Commits are not scheduler work, so the completion drain would not see them: a stop-aware
    // waiter turns each queued commit into one queued drain on the UI thread.
    waiter_ = std::jthread(
        [this, subscription = host_.subscribeCommits()](const std::stop_token& stop) mutable {
            auto observed = subscription.serial();
            while (const auto serial = subscription.waitForChange(observed, stop)) {
                observed = *serial;
                scheduleDrain();
            }
        });
}

LiveSessionAdapter::~LiveSessionAdapter() {
    shutdown();
    detachObserver();
}

void LiveSessionAdapter::shutdown() {
    if (shutDown_)
        return;
    shutDown_ = true;
    host_.revoke(scripting::LiveSessionAvailability::ShuttingDown);
    waiter_.request_stop();
    if (waiter_.joinable())
        waiter_.join();
}

void LiveSessionAdapter::setViewerDisplayView(std::string display, std::string view) {
    viewerDisplay_ = std::move(display);
    viewerView_ = std::move(view);
    publish();
}

void LiveSessionAdapter::detachObserver() {
    observerLife_.reset();
    if (observedStack_ != nullptr && observedStack_ == project_.liveDocumentAndStack().second)
        observedStack_->removeObserver(observer_);
    observedStack_ = nullptr;
}

void LiveSessionAdapter::revoke() {
    if (!shutDown_)
        host_.revoke(scripting::LiveSessionAvailability::Replacing);
}

void LiveSessionAdapter::replace() {
    if (shutDown_)
        return;
    detachObserver();
    auto* const stack = project_.liveDocumentAndStack().second;
    if (stack == nullptr) {
        host_.revoke(scripting::LiveSessionAvailability::Replacing);
        return;
    }
    // A fresh event stream per generation: subscriptions to the old one never see the new
    // project's commands.
    auto stream = std::make_shared<scripting::EventStream>();
    observerLife_ = std::make_shared<int>(0);
    observedStack_ = stack;
    observer_ = stack->addObserver(
        [life = std::weak_ptr(observerLife_), stream](const commands::CommandEvent& event) {
            if (!life.expired())
                stream->publish(event);
        });
    host_.replace(
        {std::move(stream), &project_.publicationCoordinator(), &project_.artifactCoordinator()},
        {.renderSource = {session_.snapshot(), {}, std::nullopt},
         .context = {},
         .viewerDisplay = {},
         .viewerView = {},
         .projectPath = std::nullopt,
         .readOnly = false});
    publish();
}

void LiveSessionAdapter::publish() {
    if (shutDown_)
        return;
    const auto snapshot = session_.snapshot();
    scripting::LiveSessionState state{.renderSource = {snapshot, {}, std::nullopt},
                                      .context = {},
                                      .viewerDisplay = viewerDisplay_,
                                      .viewerView = viewerView_,
                                      .projectPath = project_.displayPath(),
                                      .readOnly = false};
    if (state.projectPath)
        state.renderSource.assetBaseDirectory = state.projectPath->parent_path();
    if (const auto* settings = project_.colorSettings())
        state.renderSource.colorSettings = *settings;
    auto& context = state.context;
    context.project = snapshot.project().id().value();
    context.composition = session_.compositionId().value();
    context.time = session_.currentTime();
    for (const auto id : session_.selectedNodes())
        context.selection.push_back(id.value());
    context.layers = scripting::selectedLayers(snapshot, *context.composition, context.selection);
    host_.publish(std::move(state));
}

void LiveSessionAdapter::scheduleDrain() {
    if (shutDown_ || drainQueued_.exchange(true))
        return;
    QMetaObject::invokeMethod(this, &LiveSessionAdapter::drain, Qt::QueuedConnection);
}

void LiveSessionAdapter::drain() {
    drainQueued_ = false;
    if (shutDown_)
        return;
    // At most one commit per event-loop slice.
    static_cast<void>(host_.drainOne(*committer_));
    if (host_.queuedCommits() == 0)
        return;
    if (host_.waitingForGesture())
        QTimer::singleShot(kGestureRetry, this, &LiveSessionAdapter::scheduleDrain);
    else
        scheduleDrain();
}

} // namespace bloom::ui
