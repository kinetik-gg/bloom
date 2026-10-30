#pragma once

// Private to src/mcp (and the bloom_mcp_testing driver): McpServerService's state (ADR 0034).
//
// Three groups, each under its own lock:
// - owner state (the UI thread only, no lock): lifecycle, preferences, failure, endpoint;
// - facts (factsMutex): what network threads and request workers record -- clients, request
//   counts, the activity ring and events waiting to be published;
// - the published snapshot and event ring (publishMutex), read by any thread.
// Network threads never publish; pump() turns recorded facts into the next snapshot.

#include <bloom/mcp/server_service.hpp>
#include <bloom/platform/loopback_listener.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <list>
#include <map>
#include <mutex>
#include <thread>

namespace bloom::mcp {

struct McpServerSubscription::State {
    mutable std::mutex mutex;
    mutable std::condition_variable_any changed;
    std::uint64_t serial = 0;
    bool closed = false;
};

namespace service {

using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;

// One MCP client: one Mcp-Session-Id.
struct Client {
    std::uint64_t id = 0;
    std::string sessionId;
    std::shared_ptr<McpProtocolSession> protocol;
    McpClientInfo info;
    SystemClock::time_point connectedAt;
    SystemClock::time_point lastActivityAt;
    SteadyClock::time_point lastActivity;
    // Executing or pending.
    std::uint32_t inFlight = 0;
    std::uint64_t requests = 0;
    std::optional<McpTool> lastTool;
    // Request key -> its stop source, for notifications/cancelled from this client only.
    std::map<std::string, std::shared_ptr<std::stop_source>> active;
    // Scripted by the testing driver: no protocol session.
    bool scripted = false;
};

// One request handed to the worker pool and answered on its connection thread.
struct Exchange {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<McpProgress> progress;
    bool done = false;
    McpHandled handled;
};

struct Job {
    std::shared_ptr<Client> client;
    std::string body;
    std::shared_ptr<std::stop_source> stop;
    bool streamProgress = false;
    std::shared_ptr<Exchange> exchange;
};

// A queued call onto the owner thread (the export queue's thread).
struct OwnerTask {
    McpExportAccess::Work work;
    bool started = false;
    bool done = false;
    bool withdrawn = false;
    std::exception_ptr error;
};

// Everything one Running period owns. Destroyed by pump() once quiescent.
struct Network {
    std::unique_ptr<platform::LoopbackListener> listener;
    std::uint16_t port = 0;
    std::stop_source stop;
    std::mutex mutex;
    std::condition_variable_any changed;
    // Open connections (closed from the owner thread on stop).
    std::list<std::shared_ptr<platform::LoopbackConnection>> connections;
    std::deque<Job> pending;
    std::size_t executing = 0;
    // Threads still running (acceptor, connections, workers); the network is quiescent at zero.
    std::atomic<std::size_t> threads = 0;
    std::vector<std::jthread> workers;
    std::jthread acceptor;
    // Each connection thread and whether it finished (finished ones are joined by the acceptor).
    std::list<std::pair<std::shared_ptr<std::atomic_bool>, std::jthread>> connectionThreads;
    std::atomic_bool failed = false;
};

struct TokenTask {
    std::mutex mutex;
    bool done = false;
    McpTokenState state = McpTokenState::Unavailable;
    std::string token;
    std::string reason;
};

} // namespace service

struct McpServerService::State : std::enable_shared_from_this<McpServerService::State> {
    explicit State(McpServerServiceDependencies value) : dependencies(std::move(value)) {}

    McpServerServiceDependencies dependencies;
    std::unique_ptr<McpServerCore> core;

    // ---- Owner thread.
    McpServerState state = McpServerState::Stopped;
    McpServerPreferences preferences;
    std::uint64_t preferencesRevision = 0;
    std::optional<McpServerFailure> failure;
    std::optional<McpEndpoint> endpoint;
    std::optional<McpStopCause> stopCause;
    bool shuttingDown = false;
    bool autoStartPending = false;
    std::uint64_t exportGeneration = 1;
    std::shared_ptr<service::TokenTask> tokenTask;
    bool tokenRegenerating = false;
    std::unique_ptr<service::Network> network;
    std::uint64_t publishedLiveGeneration = 0;

    // ---- Token (tokenMutex): read by connection threads for admission.
    mutable std::mutex tokenMutex;
    std::string token;
    McpTokenState tokenState = McpTokenState::Loading;
    std::uint64_t tokenGeneration = 0;
    std::string tokenReason;

    // ---- Facts (factsMutex).
    mutable std::mutex factsMutex;
    std::map<std::string, std::shared_ptr<service::Client>> sessions;
    std::uint64_t nextClient = 1;
    std::uint32_t activeRequests = 0;
    std::uint64_t totalRequests = 0;
    std::deque<McpActivityEntry> activityRing;
    std::uint64_t nextActivity = 1;
    bool activityGap = false;
    std::vector<McpServerEventPayload> pendingEvents;
    std::size_t openConnections = 0;
    bool dirty = true;

    // ---- Published (publishMutex).
    mutable std::mutex publishMutex;
    std::shared_ptr<const McpServerSnapshot> published;
    std::deque<McpServerEvent> eventRing;
    std::uint64_t nextEvent = 1;

    // ---- Wake (wakeMutex).
    std::mutex wakeMutex;
    std::vector<std::shared_ptr<McpServerSubscription::State>> subscribers;

    // ---- Owner tasks (ownerMutex).
    std::mutex ownerMutex;
    std::condition_variable_any ownerChanged;
    std::deque<std::shared_ptr<service::OwnerTask>> ownerTasks;
    bool ownerOpen = true;

    // Advances every subscription's serial (any thread).
    void wake();
    // Records under factsMutex and wakes the owner.
    void recordActivity(McpActivityEntry entry);
    void recordEvent(McpServerEventPayload payload);
    // A client's label as entries capture it: the title, else the name, at most 64 bytes.
    [[nodiscard]] static std::string clientLabel(const McpClientInfo& info);
    [[nodiscard]] McpClientView view(const service::Client& client) const;
    // Removes a client (factsMutex held), cancels its requests and records the disconnect.
    void dropClientLocked(const std::string& sessionId, McpDisconnectCause cause);

    // Owner-thread queue for the export queue.
    bool runOnOwner(const McpExportAccess::Work& work, const std::stop_token& stop);
    void runOwnerTasks();
    void withdrawOwnerTasks();

    // Network (server_http.cpp).
    void startNetwork(std::unique_ptr<platform::LoopbackListener> listener);
    void stopNetwork();
    [[nodiscard]] bool networkQuiescent() const;
};

} // namespace bloom::mcp
