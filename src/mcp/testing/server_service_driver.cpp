// Test support for the MCP Server panel (ADR 0034): scripts a real McpServerService.
#include <bloom/mcp/testing/server_service_driver.hpp>

#include "server_service_state.hpp"

namespace bloom::mcp::testing {

using service::Client;
using service::SteadyClock;
using service::SystemClock;

McpServerServiceDriver::McpServerServiceDriver(McpServerService& service) noexcept
    : service_(service) {}

std::uint64_t McpServerServiceDriver::connectClient(McpClientInfo info) {
    auto& s = *service_.state_;
    auto client = std::make_shared<Client>();
    client->scripted = true;
    client->info = std::move(info);
    client->connectedAt = client->lastActivityAt = SystemClock::now();
    client->lastActivity = SteadyClock::now();
    {
        const std::lock_guard lock(s.factsMutex);
        client->id = s.nextClient++;
        client->sessionId = "scripted-" + std::to_string(client->id);
        s.sessions[client->sessionId] = client;
        s.pendingEvents.emplace_back(McpClientConnected{s.view(*client)});
        s.dirty = true;
    }
    McpActivityEntry entry;
    entry.kind = McpActivityKind::ClientConnected;
    entry.client = client->id;
    entry.clientLabel = McpServerService::State::clientLabel(client->info);
    entry.summary = "Client connected";
    s.recordActivity(std::move(entry));
    return client->id;
}

namespace {
std::shared_ptr<Client> find(McpServerService::State& s, const std::uint64_t id) {
    for (const auto& [session, client] : s.sessions) {
        if (client->id == id)
            return client;
    }
    return nullptr;
}
} // namespace

void McpServerServiceDriver::disconnectClient(const std::uint64_t client,
                                              const McpDisconnectCause cause) {
    auto& s = *service_.state_;
    {
        const std::lock_guard lock(s.factsMutex);
        if (const auto found = find(s, client))
            s.dropClientLocked(found->sessionId, cause);
    }
    s.wake();
}

void McpServerServiceDriver::setInFlight(const std::uint64_t client, const std::uint32_t requests) {
    auto& s = *service_.state_;
    {
        const std::lock_guard lock(s.factsMutex);
        if (const auto found = find(s, client)) {
            s.activeRequests =
                s.activeRequests - std::min(s.activeRequests, found->inFlight) + requests;
            found->inFlight = requests;
            s.dirty = true;
        }
    }
    s.wake();
}

void McpServerServiceDriver::recordToolCall(const std::uint64_t client, const McpTool tool,
                                            const McpCallOutcome outcome,
                                            const std::optional<McpToolErrorCode> toolError,
                                            const std::chrono::milliseconds duration,
                                            std::string summary) {
    auto& s = *service_.state_;
    McpActivityEntry entry;
    {
        const std::lock_guard lock(s.factsMutex);
        if (const auto found = find(s, client)) {
            found->lastTool = tool;
            ++found->requests;
            found->lastActivityAt = SystemClock::now();
            entry.clientLabel = McpServerService::State::clientLabel(found->info);
        }
        ++s.totalRequests;
        s.dirty = true;
    }
    entry.kind = McpActivityKind::ToolCall;
    entry.client = client;
    entry.tool = tool;
    entry.outcome = outcome;
    entry.toolError = toolError;
    entry.duration = duration;
    entry.summary = summary.empty() ? std::string(toolName(tool)) : std::move(summary);
    s.recordActivity(std::move(entry));
}

void McpServerServiceDriver::recordRejection(const McpHttpRejection rejection,
                                             const std::optional<std::uint64_t> client) {
    auto& s = *service_.state_;
    McpActivityEntry entry;
    entry.kind = McpActivityKind::RequestRejected;
    entry.rejection = rejection;
    entry.client = client;
    if (client) {
        const std::lock_guard lock(s.factsMutex);
        if (const auto found = find(s, *client))
            entry.clientLabel = McpServerService::State::clientLabel(found->info);
    }
    entry.summary = "Request rejected";
    s.recordActivity(std::move(entry));
}

void McpServerServiceDriver::failListener() {
    auto& s = *service_.state_;
    if (s.network)
        s.network->failed = true;
    s.wake();
}

void McpServerServiceDriver::setToken(const McpTokenState state, std::string token,
                                      std::string reason) {
    auto& s = *service_.state_;
    {
        const std::lock_guard lock(s.tokenMutex);
        s.tokenState = state;
        if (state == McpTokenState::Ready) {
            if (s.token != token && !s.token.empty())
                ++s.tokenGeneration;
            s.token = std::move(token);
        }
        s.tokenReason = std::move(reason);
    }
    {
        const std::lock_guard lock(s.factsMutex);
        s.dirty = true;
    }
    s.wake();
}

std::unique_ptr<platform::LoopbackListener>
McpServerServiceDriver::occupyPort(const std::uint16_t port) {
    return platform::LoopbackListener::listen(port).listener;
}

std::uint16_t McpServerServiceDriver::freePort(const std::uint16_t from) {
    for (std::uint32_t port = from; port <= kMcpMaximumPort; ++port) {
        if (auto listened = platform::LoopbackListener::listen(static_cast<std::uint16_t>(port));
            listened.bind.bound())
            return static_cast<std::uint16_t>(port);
    }
    return 0;
}

} // namespace bloom::mcp::testing
