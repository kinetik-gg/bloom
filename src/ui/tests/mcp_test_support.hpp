#pragma once

// A scripted McpServerPort for the MCP Server panel's tests and captures: hand-built snapshots,
// recorded commands, an activity ring and a waiter that wakes when a test publishes.

#include <bloom/ui/mcp_server_adapter.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bloom::ui::mcptest {

namespace m = bloom::mcp;

inline m::McpCommandAvailability available() {
    m::McpCommandAvailability a;
    a.available = true;
    a.code = m::McpUnavailableCode::Available;
    return a;
}

inline m::McpCommandAvailability refused(const m::McpUnavailableCode code, const m::McpField field,
                                         std::string reason) {
    m::McpCommandAvailability a;
    a.available = false;
    a.code = code;
    a.field = field;
    a.reason = std::move(reason);
    return a;
}

// A snapshot with every command's availability consistent with `state`.
inline m::McpServerSnapshot makeSnapshot(const m::McpServerState state, const std::uint16_t port = 6767) {
    m::McpServerSnapshot s;
    s.state = state;
    s.preferences.port = port;
    s.token.state = m::McpTokenState::Ready;
    s.token.masked = "blm_ab12••••••••9xyz";
    s.project.bound = true;
    using C = m::McpServerCommand;
    const bool running = state == m::McpServerState::Running;
    const bool stopping = state == m::McpServerState::Stopping;
    for (auto& entry : s.commands.entries)
        entry = available();
    auto set = [&](const C c, m::McpCommandAvailability a) {
        s.commands.entries[static_cast<std::size_t>(c)] = std::move(a);
    };
    if (running || stopping) {
        set(C::Start, refused(stopping ? m::McpUnavailableCode::Stopping
                                       : m::McpUnavailableCode::AlreadyRunning,
                              m::McpField::Status,
                              stopping ? "The server is stopping." : "The server is already running."));
        set(C::SetPort, refused(m::McpUnavailableCode::ServerRunning, m::McpField::Port,
                                "Stop the server to change the port."));
    }
    if (!running)
        set(C::Stop, refused(stopping ? m::McpUnavailableCode::Stopping
                                      : m::McpUnavailableCode::NotRunning,
                             m::McpField::Status,
                             stopping ? "The server is stopping." : "The server isn't running."));
    if (running) {
        m::McpEndpoint endpoint;
        endpoint.port = port;
        s.endpoint = endpoint;
    }
    return s;
}

inline m::McpClientView makeClient(const std::uint64_t id, std::string name, std::string version) {
    m::McpClientView c;
    c.id = id;
    c.info.name = std::move(name);
    c.info.version = std::move(version);
    c.connectedAt = std::chrono::system_clock::now() - std::chrono::minutes(12);
    c.lastActivityAt = std::chrono::system_clock::now() - std::chrono::seconds(4);
    c.inFlight = static_cast<std::uint32_t>(id % 2);
    c.lastTool = m::McpTool::Query;
    return c;
}

inline m::McpActivityEntry makeToolCall(const std::uint64_t id, std::string label, const m::McpTool tool) {
    m::McpActivityEntry e;
    e.id = id;
    e.at = std::chrono::system_clock::now();
    e.kind = m::McpActivityKind::ToolCall;
    e.clientLabel = std::move(label);
    e.tool = tool;
    e.outcome = m::McpCallOutcome::Succeeded;
    e.duration = std::chrono::milliseconds(12);
    return e;
}

class ScriptedMcpPort final : public McpServerPort {
  public:
    ScriptedMcpPort() {
        auto first = makeSnapshot(m::McpServerState::Stopped);
        first.revision = 1;
        current_ = std::make_shared<m::McpServerSnapshot>(std::move(first));
    }

    // Test control. stage() is invisible until the next pump(): the snapshot is published only
    // there. publish() wakes the waiter.
    void stage(m::McpServerSnapshot snapshot) {
        std::lock_guard lock(mutex_);
        snapshot.revision = ++stagedRevision_;
        snapshot.lastActivityId = lastActivityId_;
        staged_ = std::make_shared<m::McpServerSnapshot>(std::move(snapshot));
    }
    void publish() {
        {
            std::lock_guard lock(mutex_);
            ++serial_;
        }
        wake_.notify_all();
    }
    void addActivity(m::McpActivityEntry entry) {
        std::lock_guard lock(mutex_);
        lastActivityId_ = entry.id;
        ring_.push_back(std::move(entry));
    }
    void trimActivity(const std::uint64_t upToId) {
        std::lock_guard lock(mutex_);
        trimmedBefore_ = upToId;
        std::erase_if(ring_, [&](const auto& e) { return e.id <= upToId; });
    }
    void addEvent(m::McpServerEventPayload payload) {
        std::lock_guard lock(mutex_);
        m::McpServerEvent event;
        event.sequence = ++eventSequence_;
        event.payload = std::move(payload);
        events_.push_back(std::move(event));
    }

    std::shared_ptr<const m::McpServerSnapshot> snapshot() const override {
        std::lock_guard lock(mutex_);
        return current_;
    }
    m::McpServerEventBatch events(const std::uint64_t after) const override {
        std::lock_guard lock(mutex_);
        m::McpServerEventBatch batch;
        for (const auto& e : events_)
            if (e.sequence > after)
                batch.events.push_back(e);
        batch.lastSequence = eventSequence_;
        return batch;
    }
    m::McpActivityBatch activity(const std::uint64_t after) const override {
        std::lock_guard lock(mutex_);
        activityCalls.push_back(after);
        m::McpActivityBatch batch;
        for (const auto& e : ring_)
            if (e.id > after)
                batch.entries.push_back(e);
        batch.gap = trimmedBefore_ > after;
        batch.lastId = lastActivityId_;
        return batch;
    }
    std::uint64_t serial() const override {
        std::lock_guard lock(mutex_);
        return serial_;
    }
    std::optional<std::uint64_t> waitForChange(const std::uint64_t observed,
                                               const std::stop_token& stop) const override {
        std::unique_lock lock(mutex_);
        if (!wake_.wait(lock, stop, [&] { return serial_ > observed; }))
            return std::nullopt;
        return serial_;
    }
    void pump() override {
        std::lock_guard lock(mutex_);
        ++pumps;
        if (staged_) {
            current_ = std::move(staged_);
            staged_.reset();
        }
    }

    m::McpCommandResult start() override { ++starts; return result(); }
    m::McpCommandResult stop() override { ++stops; return result(); }
    m::McpCommandResult setPort(const std::uint32_t port) override {
        setPortCalls.push_back(port);
        return setPortResult.value_or(result());
    }
    m::McpCommandResult setAutoStart(const bool enabled) override {
        autoStartCalls.push_back(enabled);
        return result();
    }
    m::McpCommandResult regenerateToken() override { ++regenerates; return result(); }
    m::McpTokenReveal revealToken() override {
        ++reveals;
        m::McpTokenReveal r;
        r.availability = available();
        r.token = token;
        return r;
    }
    m::McpClientConfig clientConfig(const m::McpClientKind kind,
                                    const m::McpTokenDisclosure disclosure) const override {
        m::McpClientConfig c;
        c.availability = available();
        c.kind = kind;
        c.text = "config:" + std::to_string(static_cast<int>(kind)) + ":" +
                 (disclosure == m::McpTokenDisclosure::IncludeToken ? token : "${BLOOM_MCP_TOKEN}");
        return c;
    }

    std::string token = "blm_secret_token_value_1234567890";
    std::optional<m::McpCommandResult> setPortResult;
    int starts = 0;
    int stops = 0;
    int regenerates = 0;
    int reveals = 0;
    int pumps = 0;
    std::vector<std::uint32_t> setPortCalls;
    std::vector<bool> autoStartCalls;
    mutable std::vector<std::uint64_t> activityCalls;

  private:
    static m::McpCommandResult result() {
        m::McpCommandResult r;
        r.accepted = true;
        r.availability = available();
        return r;
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable_any wake_;
    std::shared_ptr<const m::McpServerSnapshot> current_;
    std::shared_ptr<const m::McpServerSnapshot> staged_;
    std::uint64_t stagedRevision_ = 1;
    std::uint64_t serial_ = 0;
    std::uint64_t eventSequence_ = 0;
    std::uint64_t lastActivityId_ = 0;
    std::uint64_t trimmedBefore_ = 0;
    std::vector<m::McpActivityEntry> ring_;
    std::vector<m::McpServerEvent> events_;
};

} // namespace bloom::ui::mcptest
