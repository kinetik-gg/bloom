// Compile-and-value checks for the in-app MCP server contract headers (ADR 0034): value types,
// defaults, limits, names, codes and the constexpr helpers. The behaviour is covered by
// bloom.mcp.http-parser, bloom.mcp.server-service, bloom.mcp.http-conformance,
// bloom.platform.loopback-listener, bloom.platform.owner-only-secret and
// bloom.scripting.live-session-host.

#include <bloom/mcp/protocol.hpp>
#include <bloom/mcp/server_core.hpp>
#include <bloom/mcp/server_service.hpp>
#include <bloom/mcp/streamable_http.hpp>
#include <bloom/platform/loopback_listener.hpp>
#include <bloom/platform/owner_only_secret.hpp>
#include <bloom/scripting/live_session_host.hpp>

#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using namespace bloom;

// Ownership shapes: services and listeners are not copyable; subscriptions, leases and connections
// move.
static_assert(!std::is_copy_constructible_v<mcp::McpServerService>);
static_assert(!std::is_copy_constructible_v<mcp::McpServerCore>);
static_assert(!std::is_copy_constructible_v<mcp::McpProtocolSession>);
static_assert(!std::is_copy_constructible_v<mcp::McpServerSubscription>);
static_assert(std::is_nothrow_move_constructible_v<mcp::McpServerSubscription>);
static_assert(!std::is_copy_constructible_v<scripting::LiveSessionHost>);
static_assert(!std::is_copy_constructible_v<scripting::LiveSessionLease>);
static_assert(std::is_nothrow_move_constructible_v<scripting::LiveSessionLease>);
static_assert(std::is_nothrow_move_constructible_v<scripting::LiveCommitSubscription>);
static_assert(!std::is_copy_constructible_v<platform::LoopbackListener>);
static_assert(!std::is_move_constructible_v<platform::LoopbackListener>);
static_assert(!std::is_copy_constructible_v<platform::LoopbackConnection>);
static_assert(std::is_nothrow_move_constructible_v<platform::LoopbackConnection>);

// The owner's defaults: port 6767, inside the accepted range, off until started.
static_assert(mcp::kMcpDefaultPort == 6767);
static_assert(mcp::portInRange(mcp::kMcpDefaultPort));
static_assert(!mcp::portInRange(0));
static_assert(!mcp::portInRange(1023));
static_assert(mcp::portInRange(1024));
static_assert(mcp::portInRange(65535));
static_assert(!mcp::portInRange(65536));
static_assert(mcp::kMcpEndpointPath == "/mcp");
static_assert(platform::kLoopbackAddress == "127.0.0.1");
static_assert(mcp::kMcpProtocolVersion == "2025-11-25");

// The existing bloom-mcp limits are reused unchanged on both transports.
static_assert(mcp::kMcpMessageLimits.requestBytes == std::size_t{1024} * 1024U);
static_assert(mcp::kMcpMessageLimits.responseBytes == std::size_t{4} * 1024U * 1024U);
static_assert(mcp::kMcpMessageLimits.jsonValues == 65536);
static_assert(mcp::kMcpMessageLimits.jsonDepth == 32);
static_assert(mcp::kMcpMessageLimits.pendingMessages == 16);
static_assert(mcp::kMcpMessageLimits.renderFrames == 10000);
static_assert(mcp::kMcpMessageLimits.retainedCommandEvents == 1024);
// The live commit queue keeps the Script editor's existing bound.
static_assert(scripting::kLiveCommitQueueCapacity == 16);

// Tools: the stdio five keep their names; render/export/capture are task tools; only transact and
// history mutate the document; the export.* names are ADR 0033's session calls verbatim.
static_assert(mcp::toolName(mcp::McpTool::Query) == "query");
static_assert(mcp::toolName(mcp::McpTool::Transact) == "transact");
static_assert(mcp::toolName(mcp::McpTool::Render) == "render");
static_assert(mcp::toolName(mcp::McpTool::Export) == "export");
static_assert(mcp::toolName(mcp::McpTool::Events) == "events");
static_assert(mcp::toolName(mcp::McpTool::Capture) == "capture");
static_assert(mcp::toolName(mcp::McpTool::ExportConfirmReplace) == "export.confirm-replace");
static_assert(mcp::toolName(mcp::McpTool::ExportScript) == "export.script");
static_assert(static_cast<std::size_t>(mcp::McpTool::ExportScript) + 1 == mcp::kMcpToolCount);
static_assert(mcp::toolExecution(mcp::McpTool::Render) == mcp::McpToolExecution::Task);
static_assert(mcp::toolExecution(mcp::McpTool::Capture) == mcp::McpToolExecution::Task);
static_assert(mcp::toolExecution(mcp::McpTool::ExportWait) == mcp::McpToolExecution::Task);
static_assert(mcp::toolExecution(mcp::McpTool::Transact) == mcp::McpToolExecution::Inline);
static_assert(mcp::toolExecution(mcp::McpTool::ExportStart) == mcp::McpToolExecution::Inline);
static_assert(mcp::toolMutatesDocument(mcp::McpTool::Transact));
static_assert(mcp::toolMutatesDocument(mcp::McpTool::History));
static_assert(!mcp::toolMutatesDocument(mcp::McpTool::Render));
static_assert(!mcp::toolMutatesDocument(mcp::McpTool::ExportStart));

// An inline capture image always fits the unchanged response limit.
static_assert(mcp::kMcpInlineImageBytes / 3U * 4U < mcp::kMcpMessageLimits.responseBytes);

// HTTP admission maps to the statuses the specification and RFC 9110 call for.
static_assert(mcp::httpStatus(mcp::McpHttpRejection::HostNotAllowed) == 403);
static_assert(mcp::httpStatus(mcp::McpHttpRejection::OriginNotAllowed) == 403);
static_assert(mcp::httpStatus(mcp::McpHttpRejection::Unauthorized) == 401);
static_assert(mcp::httpStatus(mcp::McpHttpRejection::MethodNotAllowed) == 405);
static_assert(mcp::httpStatus(mcp::McpHttpRejection::MissingSession) == 400);
static_assert(mcp::httpStatus(mcp::McpHttpRejection::UnknownSession) == 404);
static_assert(mcp::httpStatus(mcp::McpHttpRejection::UnsupportedProtocolVersion) == 400);
static_assert(mcp::httpStatus(mcp::McpHttpRejection::PayloadTooLarge) == 413);
static_assert(mcp::httpStatus(mcp::McpHttpRejection::Busy) == 503);

// Token and session id sizes: 256-bit token, 128-bit session id.
static_assert(mcp::kMcpTokenRandomBytes == 32);
static_assert(mcp::kMcpTokenLength == mcp::kMcpTokenPrefix.size() + 43);
static_assert(mcp::kMcpSessionIdBytes == 16);

static_assert(static_cast<std::size_t>(mcp::McpServerCommand::CopyClientConfig) + 1 ==
              mcp::kMcpServerCommandCount);
static_assert(static_cast<std::size_t>(mcp::McpClientKind::Generic) + 1 ==
              mcp::kMcpClientKindCount);

int expect(const bool condition, const char* message) {
    if (condition)
        return 0;
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    int failures = 0;

    const mcp::McpServerPreferences preferences;
    failures += expect(preferences.port == 6767 && !preferences.autoStart,
                       "the server defaults to port 6767 and does not start on launch");

    const mcp::McpServerSnapshot snapshot;
    failures +=
        expect(snapshot.state == mcp::McpServerState::Stopped, "the server is off until started");
    failures += expect(!snapshot.endpoint && !snapshot.failure && snapshot.clients.empty(),
                       "a stopped server has no endpoint, failure or clients");
    failures +=
        expect(snapshot.token.state == mcp::McpTokenState::Loading && snapshot.token.masked.empty(),
               "the snapshot never carries a token before it loads, and then only masked");
    failures += expect(!snapshot.commands[mcp::McpServerCommand::Start].available &&
                           snapshot.commands[mcp::McpServerCommand::Start].code ==
                               mcp::McpUnavailableCode::NotApplicable,
                       "a default availability set refuses with NotApplicable");

    mcp::McpCommandAvailability inUse;
    inUse.code = mcp::McpUnavailableCode::PortInUse;
    inUse.field = mcp::McpField::Port;
    inUse.port = 6767;
    inUse.reason = "Port 6767 is in use. Choose another port or stop the other program.";
    failures += expect(!inUse && inUse.port == 6767u && inUse.field == mcp::McpField::Port,
                       "a port refusal names the port and the field it belongs to");

    // The Port field's live check is the PortOutOfRange refusal setPort() returns: the same code,
    // field, value and reason. This pins the inline helper setPort() returns verbatim;
    // bloom.mcp.server-service compares the two directly.
    for (const std::uint32_t valid : {1024u, 6767u, 65535u})
        failures += expect(!mcp::validateMcpPort(valid).has_value(), "an in-range port is valid");
    for (const std::uint32_t invalid : {0u, 80u, 1023u, 65536u, 4294967295u}) {
        const auto refusal = mcp::validateMcpPort(invalid);
        failures += expect(refusal.has_value() && !refusal->available &&
                               refusal->code == mcp::McpUnavailableCode::PortOutOfRange &&
                               refusal->field == mcp::McpField::Port && refusal->port == invalid,
                           "an out-of-range port is refused at the Port field naming the value");
        failures += expect(refusal && refusal->reason == "Port " + std::to_string(invalid) +
                                                             " can't be used. Choose a port from "
                                                             "1024 to 65535.",
                           "the out-of-range reason is a plain sentence naming the value");
    }
    static_assert(noexcept(mcp::validateMcpPort(0)));

    const mcp::McpEndpoint endpoint;
    failures += expect(endpoint.host == "127.0.0.1" && endpoint.path == "/mcp",
                       "the endpoint is loopback only");

    mcp::McpActivityEntry entry;
    entry.id = 1;
    entry.kind = mcp::McpActivityKind::RequestRejected;
    entry.rejection = mcp::McpHttpRejection::Unauthorized;
    failures += expect(entry.repeat == 1 && entry.summary.empty() && !entry.client &&
                           entry.clientLabel.empty(),
                       "an activity entry starts with no client, label or text");

    mcp::McpServerEvent event;
    event.sequence = 1;
    event.payload =
        mcp::McpClientDisconnected{.client = 3, .cause = mcp::McpDisconnectCause::TokenRegenerated};
    failures += expect(std::holds_alternative<mcp::McpClientDisconnected>(event.payload),
                       "client disconnection is a typed event");

    const mcp::McpServerLimits limits;
    failures += expect(limits.clients == 8 && limits.connections == 16 &&
                           limits.requestWorkers == 4 && limits.activityEntries == 512,
                       "the documented concurrency and log bounds");

    std::set<std::string_view> codes;
    for (std::uint8_t code = 0;
         code <= static_cast<std::uint8_t>(mcp::McpToolErrorCode::OperationRejected); ++code)
        codes.insert(mcp::toolErrorCode(static_cast<mcp::McpToolErrorCode>(code)));
    failures += expect(codes.size() == 12, "every tool error has a distinct stable code");

    failures +=
        expect(scripting::liveCommitDiagnosticCode(scripting::LiveCommitStatus::Completed).empty(),
               "a completed commit carries no diagnostic");
    failures += expect(scripting::liveCommitDiagnosticCode(
                           scripting::LiveCommitStatus::GestureOpen) == "bloom.live.gesture-open",
                       "a commit withdrawn behind an open gesture says so");

    const platform::LoopbackBindResult refused{.status = platform::LoopbackBindStatus::PortInUse,
                                               .port = 6767,
                                               .systemCode = 0,
                                               .message = {}};
    failures += expect(!refused.bound() && refused.port == 6767,
                       "a refused bind names the port it was asked for");

    const scripting::LiveCallControl control;
    failures += expect(control.deadline == std::chrono::steady_clock::time_point::max(),
                       "a call without a deadline waits until cancelled");

    return failures == 0 ? 0 : 1;
}
