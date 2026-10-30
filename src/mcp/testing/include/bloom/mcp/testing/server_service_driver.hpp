#pragma once

#include <bloom/mcp/server_service.hpp>
#include <bloom/platform/loopback_listener.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// Test support only (never linked into production): scripts a real McpServerService into every
// state the MCP Server panel shows (ADR 0034, section 7) without real MCP clients. Like
// bloom_host_export_testing's fake runner, it drives the service through the same recording paths
// its network threads use, so snapshots, events, the activity log and availability come from the
// production code.
//
// Every method runs on the service's owner thread. Each one records facts and wakes the service's
// subscription exactly as network activity does; call McpServerService::pump() (or let the
// application-lifetime adapter drain) to publish them.
namespace bloom::mcp::testing {

class McpServerServiceDriver final {
  public:
    explicit McpServerServiceDriver(McpServerService& service) noexcept;

    // A scripted client completes initialize: McpClientConnected and a ClientConnected entry.
    // Returns its display id (McpClientView::id). Works while the server is stopped too, so a panel
    // test can show clients without binding a port.
    std::uint64_t connectClient(McpClientInfo info);
    // McpClientDisconnected with `cause` and a ClientDisconnected entry labelled with the client.
    void disconnectClient(std::uint64_t client,
                          McpDisconnectCause cause = McpDisconnectCause::Ended);
    // The client's in-flight requests (and the snapshot's activeRequests).
    void setInFlight(std::uint64_t client, std::uint32_t requests);
    // A finished tools/call: a ToolCall entry with the typed facts, and the client's last tool.
    void recordToolCall(std::uint64_t client, McpTool tool,
                        McpCallOutcome outcome = McpCallOutcome::Succeeded,
                        std::optional<McpToolErrorCode> toolError = std::nullopt,
                        std::chrono::milliseconds duration = std::chrono::milliseconds{0},
                        std::string summary = {});
    // A request refused before the core (identical ones within a second fold into `repeat`).
    void recordRejection(McpHttpRejection rejection,
                         std::optional<std::uint64_t> client = std::nullopt);
    // While Running: the listener fails, so the next pump goes Running -> Stopping -> Failed with
    // McpServerListenerFailed and the failure reason.
    void failListener();
    // Scripts the token: Loading, Regenerating, Unavailable (with `reason`) or Ready (with
    // `token`, which must be well formed).
    void setToken(McpTokenState state, std::string token = {}, std::string reason = {});

    // Holds 127.0.0.1:<port> so McpServerService::start() is refused with PortInUse naming it.
    // Null when the port could not be bound by the test itself.
    [[nodiscard]] static std::unique_ptr<platform::LoopbackListener> occupyPort(std::uint16_t port);
    // A port this process could bind just now, searching from `from` (for tests that start the
    // real server). Zero when none was found.
    [[nodiscard]] static std::uint16_t freePort(std::uint16_t from = 41000);

  private:
    McpServerService& service_;
};

} // namespace bloom::mcp::testing
