#pragma once

#include <bloom/mcp/live_ui_port.hpp>
#include <bloom/mcp/protocol.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::host {
class ExportQueueService;
class GpuExportProvider;
} // namespace bloom::host
namespace bloom::runtime {
class TaskScheduler;
} // namespace bloom::runtime
namespace bloom::scripting {
class LiveSessionHost;
class Session;
} // namespace bloom::scripting

// The one MCP server core both hosts share (ADR 0034, section 2). Qt-free; owned by src/mcp.
//
// It owns JSON-RPC 2.0 framing and validation, the MCP handshake, tool schemas, tool handlers,
// projections and the command-event history -- everything apps/bloom-mcp implements today. The
// stdio host (apps/bloom-mcp) and the in-app Streamable HTTP host (McpServerService) are thin
// transports over it; neither forks a handler.
//
// The core knows nothing about sockets or stdio. A transport hands it one message at a time with
// that message's protocol session and request control, and writes back what handle() returns.
//
// Threading: handle() runs on the calling transport worker, never on the UI thread. Several calls
// may run concurrently (different clients, or parallel requests from one client after initialize).
// Document reads come from immutable snapshots; commits go through the session source (directly for
// stdio, through the live commit marshal for the desktop); render, export and capture submit export
// queue jobs and wait on the queue's worker-thread subscription.
namespace bloom::mcp {

// Where the core's tools read and commit. Exactly one of:
// - a headless session the stdio host opened and owns (bloom-mcp, `--project`);
// - the desktop application's live session, through scripting::LiveSessionHost, so commits land in
//   the artist's undo history and are visible in every panel immediately.
using McpSessionSource = std::variant<std::reference_wrapper<scripting::Session>,
                                      std::reference_wrapper<scripting::LiveSessionHost>>;

// How the core reaches the one export queue (ADR 0033). Export queue commands are owner-thread
// calls, so the core never calls the queue directly from a request worker: it hands `run` a
// function that executes on the queue's owner thread with the current queue (null while none is
// bound, for example while File > New replaces it) and that queue's generation, which advances
// whenever the queue is replaced.
// - stdio: the calling thread owns its headless queue, so `run` calls the function inline and
//   `callerIsOwner` is set; a waiting task tool then pumps the queue itself.
// - desktop: McpServerService marshals the function to the UI thread through its pump(), and a
//   waiting task tool sleeps on the queue's ExportQueueSubscription.
struct McpExportAccess final {
    using Work = std::function<void(host::ExportQueueService*, std::uint64_t generation)>;
    // Returns false when the function could not run (the request stopped, or the server is
    // stopping); the function then never runs.
    std::function<bool(const Work&, const std::stop_token&)> run;
    bool callerIsOwner = false;
};

// Borrowed collaborators; all must outlive the core.
struct McpCoreDependencies final {
    McpSessionSource session;
    // The export.* tools are the queue's session calls; on the live host render, export and
    // capture are transient queue jobs (ADR 0033 section 13). The stdio host's render and export
    // keep the scripting render path (scripting::Render::run and the sequence runner) and its
    // capture renders the still route there.
    McpExportAccess exports;
    runtime::TaskScheduler& scheduler;
    // stdio: the server-lifetime GPU final-render provider that render and capture reuse (the
    // composition root shares it with its headless export queue and retires it after the core).
    // The live host renders through the export queue, which owns its provider; leave it null.
    std::shared_ptr<host::GpuExportProvider> gpuProvider;
    // Where capture publishes its PNG before embedding it: a per-user cache folder (desktop:
    // <user cache>/mcp-captures; stdio: the same). Empty makes capture answer ExportUnavailable
    // with the reason "Bloom can't find a folder for captures." At most kMcpRetainedCaptures files
    // are kept.
    std::filesystem::path captureDirectory;
    // Session and UI seams, called through `exports.run` on the owner thread. The live host
    // passes the application's bindings; the stdio host binds only projectLifecycle (over its
    // headless session) and every other live-only tool answers bloom.mcp.not-live.
    McpLiveUiPort ui{};
    // serverInfo.version in initialize.
    std::string serverVersion;
    McpMessageLimits limits = kMcpMessageLimits;
};

inline constexpr std::size_t kMcpRetainedCaptures = 16;

// clientInfo from initialize, bounded and sanitised (control characters removed, at most 256
// bytes each). Shown in the MCP Server panel; never trusted for anything else.
struct McpClientInfo final {
    std::string name;
    std::string version;
    std::string title;
    std::string protocolVersion;
};

// One MCP session's protocol state: initialize / notifications/initialized ordering, the client's
// identity and its paging cursors' binding. stdio has exactly one; the HTTP host has one per
// Mcp-Session-Id. Internally synchronised: concurrent handle() calls on one session are allowed.
class McpProtocolSession final {
  public:
    McpProtocolSession(const McpProtocolSession&) = delete;
    McpProtocolSession& operator=(const McpProtocolSession&) = delete;
    ~McpProtocolSession();

    [[nodiscard]] bool initialized() const noexcept;
    // True after notifications/initialized; tools/list and tools/call require it.
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::optional<McpClientInfo> client() const;

  private:
    struct State;
    friend class McpServerCore;
    explicit McpProtocolSession(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

enum class McpMessageKind : std::uint8_t {
    Request = 0,
    Notification = 1,
    // A JSON-RPC response from the client (Bloom sends no server-to-client requests, so these are
    // acknowledged and ignored).
    Response = 2,
    // Not a single JSON-RPC 2.0 object (includes batches, which MCP 2025-11-25 removed).
    Invalid = 3,
};

// What a transport needs to know before it executes a message: which kind it is, which request it
// cancels, whether it should stream progress. Pure; parses under the same limits; any thread.
struct McpEnvelope final {
    McpMessageKind kind = McpMessageKind::Invalid;
    // Canonical request identity ("s:<string>" or "n:<integer>") for requests.
    std::optional<std::string> requestKey;
    // Bounded to 128 bytes.
    std::string method;
    // tools/call only.
    std::optional<McpTool> tool;
    // The raw JSON of params._meta.progressToken, when present on a tools/call.
    std::optional<std::string> progressToken;
    // notifications/cancelled: the canonical identity of the request it cancels.
    std::optional<std::string> cancels;
    [[nodiscard]] bool isInitialize() const noexcept { return method == "initialize"; }
};

[[nodiscard]] McpEnvelope inspectMessage(std::string_view message,
                                         const McpMessageLimits& limits = kMcpMessageLimits);

// Progress for a Task tool, published from the export queue snapshot. The transport turns it into
// notifications/progress against the request's progress token.
struct McpProgress final {
    double progress = 0.0;
    std::optional<double> total;
    // "Rendering frame 12 of 48", "Waiting for the Viewer" (ADR 0033 WaitingForViewer).
    std::string message;
};

// The calling transport's control over one request.
struct McpRequestControl final {
    // Fired by notifications/cancelled for this request, by server stop, or by shutdown. A Task
    // tool stops its export job; a commit still queued is withdrawn; a commit already applying
    // finishes atomically.
    std::stop_token stop;
    // Set only when the request carried a progress token and the transport can stream (Streamable
    // HTTP answering with text/event-stream, or stdio). Called on the request worker.
    std::function<void(const McpProgress&)> progress;
};

enum class McpCallOutcome : std::uint8_t {
    Succeeded = 0,
    // tools/call answered with isError = true.
    ToolError = 1,
    // A JSON-RPC error response.
    ProtocolError = 2,
    Cancelled = 3,
};

// A composition an agent's edit left uncompilable (transact's compileFailures, the
// composition.compile-failed event), carried to the activity log so the application can show it.
struct McpCompileFailureFact final {
    std::uint64_t composition = 0;
    // Plain sentence, no codes or ids.
    std::string detail{};
    std::optional<std::uint64_t> node{};
    std::optional<std::uint64_t> parameter{};
};

// What handle() produced, plus the facts the activity log records. `summary` is built from the
// method, the tool name and, for transact, the operation ids and label (at most 256 bytes). It
// never contains argument values, file contents, headers or any secret.
struct McpHandled final {
    McpMessageKind kind = McpMessageKind::Invalid;
    // The serialized JSON-RPC response; absent for notifications and client responses.
    std::optional<std::string> response;
    std::string method;
    std::optional<McpTool> tool;
    McpCallOutcome outcome = McpCallOutcome::Succeeded;
    std::optional<McpToolErrorCode> toolError;
    std::string summary;
    // transact: the compositions the edit left unable to compile.
    std::vector<McpCompileFailureFact> compileFailures;
    // This message completed initialize; the protocol session now has a client.
    bool initializedClient = false;
};

class McpServerCore final {
  public:
    explicit McpServerCore(McpCoreDependencies dependencies);
    ~McpServerCore();
    McpServerCore(const McpServerCore&) = delete;
    McpServerCore& operator=(const McpServerCore&) = delete;

    // A fresh protocol session (before initialize). Any thread.
    [[nodiscard]] std::unique_ptr<McpProtocolSession> openSession();

    // Handles one JSON-RPC message for `session`. Never call it on the UI thread: a commit waits
    // for the owner thread and a Task tool waits for its export job.
    [[nodiscard]] McpHandled handle(McpProtocolSession& session, std::string_view message,
                                    const McpRequestControl& control);

    // A JSON-RPC error response for a message the transport refused before handle() (oversized,
    // unterminated, busy). `requestKey` is the inspected id when one was readable.
    [[nodiscard]] static std::string errorResponse(std::optional<std::string_view> requestKey,
                                                   McpProtocolError code, std::string_view message);

    // The tools/list payload's tool names in listing order (schemas come from the registry and
    // the export queue's vocabularies). Any thread.
    [[nodiscard]] std::span<const McpTool> tools() const noexcept;

    // The live host: follow the live session's current generation's command events (the service
    // calls it from pump() so events after a replacement are kept from the start). Any thread.
    void followLiveSession();

    // The stdio host: replaces the headless session's project in place (project.open, project.new
    // and project.close through scripting::Session::adopt inside `replace`). The core lets go of
    // the old project's event stream before `replace` runs and follows the new one after it; older
    // events cursors report resetRequired. Call it from the thread that owns the session.
    void replaceHeadlessProject(const std::function<void()>& replace);

    struct State;

  private:
    std::unique_ptr<State> state_;
};

// The stdio transport (newline-delimited UTF-8 JSON-RPC on the given streams), moved unchanged from
// apps/bloom-mcp: a bounded reader thread, the 16-message inbox, cancellation intercepted while a
// call runs, one protocol session. Returns at end of input or when `stop` fires.
void serveStdio(McpServerCore& core, std::istream& input, std::ostream& output,
                const std::stop_token& stop);

} // namespace bloom::mcp
