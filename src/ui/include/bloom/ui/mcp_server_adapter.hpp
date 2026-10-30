#pragma once

// The one Qt-side owner of the MCP server's change notification (ADR 0034, section 7).
//
// The MCP server service advances in exactly one place, pump(). In the desktop application ONE
// adapter, owned by the composition root for the application's lifetime, calls pump() on the Qt
// main thread and re-emits a single changed() when the snapshot revision moved. Two things wake it:
// TaskUiBridge::resultsAvailable, and a stop-aware waiter thread on the service's subscription,
// because socket activity is not scheduler work and would otherwise wake no drain. Wakes coalesce
// into at most one queued drain. MCP Server panels are replaceable and may be closed, duplicated
// or never opened, so no panel pumps: they read the snapshot through this adapter and issue
// commands through it.
//
// McpServerPort is the seam between this class and the service. The desktop composition root
// passes a thin port over mcp::McpServerService; tests pass a scripted one. Every member except
// waitForChange() is non-blocking.

#include <bloom/mcp/server_service.hpp>

#include <QObject>
#include <QString>

#include <chrono>
#include <functional>
#include <vector>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>

class QSettings;

namespace bloom::ui {

class TaskUiBridge;
class UiFeedback;

// The MCP server's two global preferences (ADR 0034, section 6), stored as `mcp/port` and
// `mcp/autoStart`. The composition root reads them at launch to construct the service; the adapter
// writes them when the snapshot's preferencesRevision moves. Defaults: port 6767, auto-start off.
[[nodiscard]] mcp::McpServerPreferences loadMcpServerPreferences(const QSettings& settings);
void saveMcpServerPreferences(QSettings& settings, const mcp::McpServerPreferences& preferences);

// An agent edit that left a composition unable to compile. McpServerAdapter::handleEvents() builds
// one from a transact activity entry's compileFailures (the client label, the tool, the composition
// name and the plain detail) and calls noteCompileFailure(); the panel and its text read this struct
// and nothing else.
struct McpCompileFailureNote final {
    std::uint64_t id = 0;
    std::chrono::system_clock::time_point at;
    QString client;
    QString tool;
    QString composition;
    // Plain sentence, no codes or ids.
    QString detail;
};

class McpServerPort {
  public:
    virtual ~McpServerPort() = default;

    [[nodiscard]] virtual std::shared_ptr<const mcp::McpServerSnapshot> snapshot() const = 0;
    [[nodiscard]] virtual mcp::McpServerEventBatch events(std::uint64_t afterSequence) const = 0;
    [[nodiscard]] virtual mcp::McpActivityBatch activity(std::uint64_t afterId) const = 0;

    // The subscription's serial now, and a wait for it to move past `observed`. The wait runs only
    // on the adapter's waiter thread and returns nullopt once `stop` is requested or the
    // subscription closed.
    [[nodiscard]] virtual std::uint64_t serial() const = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t>
    waitForChange(std::uint64_t observed, const std::stop_token& stop) const = 0;

    // The single publisher.
    virtual void pump() = 0;

    virtual mcp::McpCommandResult start() = 0;
    virtual mcp::McpCommandResult stop() = 0;
    virtual mcp::McpCommandResult setPort(std::uint32_t port) = 0;
    virtual mcp::McpCommandResult setAutoStart(bool enabled) = 0;
    virtual mcp::McpCommandResult regenerateToken() = 0;
    [[nodiscard]] virtual mcp::McpTokenReveal revealToken() = 0;
    [[nodiscard]] virtual mcp::McpClientConfig
    clientConfig(mcp::McpClientKind kind, mcp::McpTokenDisclosure disclosure) const = 0;
};

class McpServerAdapter final : public QObject {
    Q_OBJECT

  public:
    // `feedback` may be null (no sound). Both are borrowed and outlive the adapter.
    McpServerAdapter(McpServerPort& port, UiFeedback* feedback = nullptr,
                     QObject* parent = nullptr);
    ~McpServerAdapter() override;

    void setFeedback(UiFeedback* feedback) noexcept { feedback_ = feedback; }

    // Connects the completion drain: every TaskUiBridge::resultsAvailable runs drain().
    void attach(TaskUiBridge& bridge);
    // Starts the waiter thread. Idempotent; the destructor stops and joins it.
    void startWaiter();

    // The only caller of McpServerPort::pump(). Public for the bridge connection and for tests.
    void drain();

    [[nodiscard]] std::shared_ptr<const mcp::McpServerSnapshot> snapshot() const;
    [[nodiscard]] mcp::McpActivityBatch activity(std::uint64_t afterId) const;
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    // Commands. Each forwards to the port (which pumps before returning) and lets the adapter see
    // the new revision, so a panel updates at once.
    mcp::McpCommandResult start();
    mcp::McpCommandResult stop();
    mcp::McpCommandResult setPort(std::uint32_t port);
    mcp::McpCommandResult setAutoStart(bool enabled);
    mcp::McpCommandResult regenerateToken();
    [[nodiscard]] mcp::McpTokenReveal revealToken();
    [[nodiscard]] mcp::McpClientConfig clientConfig(mcp::McpClientKind kind,
                                                    mcp::McpTokenDisclosure disclosure) const;

    // Names a composition id for the Activity line ("left 'Main' unable to render"). Without one
    // the line says "Composition 3". Set by the composition root; read on the UI thread.
    void setCompositionNamer(std::function<QString(std::uint64_t)> namer) {
        compositionNamer_ = std::move(namer);
    }

    // Records an agent edit that broke compilation; the MCP Server panel shows it in Activity.
    void noteCompileFailure(McpCompileFailureNote note);
    [[nodiscard]] const std::vector<McpCompileFailureNote>& compileFailures() const noexcept {
        return compileFailures_;
    }

    // The status bar indicator asks for the panel to be shown.
    void requestReveal() { Q_EMIT revealRequested(); }

  Q_SIGNALS:
    // The snapshot revision moved. Panels and the status bar read the snapshot again.
    void changed();
    void compileFailureNoted();
    void revealRequested();

  private:
    void observe();
    void handleEvents();
    void wakeFromWaiter();

    McpServerPort& port_;
    UiFeedback* feedback_ = nullptr;
    std::uint64_t revision_ = 0;
    std::uint64_t preferencesRevision_ = 0;
    std::uint64_t lastEventSequence_ = 0;
    std::uint64_t lastActivityId_ = 0;
    std::function<QString(std::uint64_t)> compositionNamer_;
    std::vector<McpCompileFailureNote> compileFailures_;
    std::atomic<bool> wakePending_{false};
    std::jthread waiter_;
};

} // namespace bloom::ui
