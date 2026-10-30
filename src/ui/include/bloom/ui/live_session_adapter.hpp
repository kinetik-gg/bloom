#pragma once

#include <QObject>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace bloom::commands {
class CommandStack;
} // namespace bloom::commands
namespace bloom::scripting {
class EventStream;
class LiveSessionHost;
struct LiveSessionCommitter;
} // namespace bloom::scripting

namespace bloom::ui {
class CompositionSession;
class ProjectHost;

// The application-lifetime bridge between the UI thread's live session and
// scripting::LiveSessionHost (ADR 0034, section 4). One instance feeds the host that the Script
// editor and the in-app MCP server share:
// - it publishes the mirror (snapshot, render source, editor context, the Viewer's display and
//   view, the project path) on every snapshot, composition, selection, time, colour-settings and
//   save change;
// - it revokes the generation as soon as the document is rebound and starts the next one when the
//   project host has replaced its session, so no old commit can reach a new project;
// - it drains queued commits on the UI thread, at most one per event-loop slice, woken by
//   LiveCommitSubscription (a stop-aware waiter thread) instead of a poll, and never while an
//   interactive gesture is open (ADR 0017): a commit held by a gesture is retried when it ends.
// It holds no project truth; commits go through CompositionSession like any panel edit and land in
// the artist's undo history with the transaction's label.
class LiveSessionAdapter final : public QObject {
    Q_OBJECT
  public:
    LiveSessionAdapter(scripting::LiveSessionHost& host, CompositionSession& session,
                       ProjectHost& project, QObject* parent = nullptr);
    ~LiveSessionAdapter() override;
    LiveSessionAdapter(const LiveSessionAdapter&) = delete;
    LiveSessionAdapter& operator=(const LiveSessionAdapter&) = delete;

    [[nodiscard]] scripting::LiveSessionHost& host() const noexcept { return host_; }
    // The Viewer's display and view for the context composition; empty means the project's
    // defaults. Set by whoever owns the Viewer's display state; republishes the mirror.
    void setViewerDisplayView(std::string display, std::string view);
    // Revokes with ShuttingDown and stops the waiter. Idempotent; also runs on aboutToQuit.
    void shutdown();

  private:
    void publish();
    void replace();
    void revoke();
    void scheduleDrain();
    void drain();
    void detachObserver();

    scripting::LiveSessionHost& host_;
    CompositionSession& session_;
    ProjectHost& project_;
    std::unique_ptr<scripting::LiveSessionCommitter> committer_;
    std::string viewerDisplay_;
    std::string viewerView_;
    commands::CommandStack* observedStack_ = nullptr;
    std::uint64_t observer_ = 0;
    std::shared_ptr<int> observerLife_;
    std::atomic_bool drainQueued_ = false;
    std::atomic_bool shutDown_ = false;
    std::jthread waiter_;
};

} // namespace bloom::ui
