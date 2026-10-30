#pragma once

#include <bloom/mcp/protocol.hpp>
#include <bloom/mcp/server_core.hpp>
#include <bloom/mcp/streamable_http.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::host {
class ExportQueueService;
} // namespace bloom::host
namespace bloom::runtime {
class TaskScheduler;
} // namespace bloom::runtime
namespace bloom::scripting {
class LiveSessionHost;
} // namespace bloom::scripting

// The in-app MCP server's runtime contract (ADR 0034). Qt-free; owned by src/mcp.
//
// McpServerService runs MCP Streamable HTTP on 127.0.0.1:<port> over the shared McpServerCore,
// against the desktop application's live session (scripting::LiveSessionHost). It is the only
// owner of the server's lifecycle, its clients, its activity log and its token. The MCP Server
// editor panel (src/ui) binds to the snapshot and calls the commands below; it holds no server
// state of its own.
//
// Three kinds of state stay separate:
// - Preferences (per user, outside projects, never undoable, never project state): the port, the
//   auto-start choice and the bearer token. The port and auto-start are application preferences the
//   src/ui adapter persists from the snapshot (`mcp/port`, `mcp/autoStart`); the token lives in an
//   owner-only file in the platform user-data directory (`mcp/token`), read and written only on
//   the blocking-I/O executor.
// - Session state (this run of the application, never saved): running or not, the bound endpoint,
//   the last failure, connected clients, in-flight requests, the activity log and the events.
// - Project truth: none. The server changes the document only through the live commit marshal, as
//   ordinary undoable transactions.
//
// Threading guarantee. Every member the panel calls -- snapshot(), events(), activity(),
// availability(), every command, pump(), revealToken() and clientConfig() -- is non-blocking and
// performs no filesystem or network I/O on the calling thread, with one stated exception: start()
// binds the loopback port synchronously (a bounded system call, platform::LoopbackListener) so a
// taken port is refused on the spot with PortInUse naming the port. Accepting, reading, parsing,
// admission and request execution run on the server's own threads; render, export and capture are
// export queue jobs; token I/O is a blocking-I/O scheduler task.
//
// Change notification. Only pump() publishes a new snapshot, so the revision the UI thread sees
// advances in exactly one place and never shows half-applied state. A command records its intent
// and runs pump() before returning; its result carries that pump's revision. Network threads and
// tasks record what happened and advance the subscription serial; they never publish. In the
// desktop application one application-lifetime Qt adapter in src/ui (not a panel: panels are
// replaceable, may be closed and may be duplicated) owns a stop-aware waiter on subscribe(),
// coalesces wakes into at most one queued main-thread drain, calls pump() there, and emits one
// "server changed" signal when the revision moved. It also pumps on TaskUiBridge::resultsAvailable
// (token I/O and capture tasks). This is the export queue's pattern; the waiter exists because
// socket activity is not scheduler work and would otherwise wake no drain.
namespace bloom::mcp {

// ---------------------------------------------------------------------------------------------
// Preferences

struct McpServerPreferences final {
    std::uint16_t port = kMcpDefaultPort;
    // Start the server when the application launches. Off by default: the server is off until the
    // artist starts it.
    bool autoStart = false;
    friend bool operator==(const McpServerPreferences&, const McpServerPreferences&) = default;
};

// ---------------------------------------------------------------------------------------------
// Lifecycle
//
//   Stopped --start (bound)--> Running --stop--> Stopping --quiescent--> Stopped
//   Stopped --start (bind refused)--> (stays Stopped; typed refusal, failure recorded)
//   Running --listener failure--> Failed --start--> Running
//   Failed  --setPort--> Stopped (the failure was about the old port)
//
// There is no Starting state: binding is synchronous. Auto-start runs the same start() from pump()
// once the token has loaded; a refused auto-start leaves the server Stopped with the failure shown.

enum class McpServerState : std::uint8_t {
    Stopped = 0,
    Running = 1,
    // Listener closed; in-flight requests are being cancelled and drained.
    Stopping = 2,
    // The listener failed while running (not a refused start). Start is available again.
    Failed = 3,
};

enum class McpServerFailureCode : std::uint8_t {
    PortInUse = 1,
    PermissionDenied = 2,
    AddressUnavailable = 3,
    ResourceExhausted = 4,
    ListenerFailed = 5,
    TokenUnavailable = 6,
    SystemError = 7,
};

struct McpServerFailure final {
    McpServerFailureCode code = McpServerFailureCode::SystemError;
    std::uint16_t port = 0;
    // Plain language naming the port ("Port 6767 is in use by another program. Choose another port
    // or close the program using it."). Never contains the token.
    std::string reason;
    std::chrono::system_clock::time_point at;
};

enum class McpStopCause : std::uint8_t { User = 0, Shutdown = 1, ListenerFailed = 2 };

// ---------------------------------------------------------------------------------------------
// Commands and availability

enum class McpServerCommand : std::uint8_t {
    Start = 0,
    Stop = 1,
    SetPort = 2,
    SetAutoStart = 3,
    RegenerateToken = 4,
    RevealToken = 5,
    CopyClientConfig = 6,
};

inline constexpr std::size_t kMcpServerCommandCount = 7;

// Every refusal is typed so the panel shows it inline at the field it is about; nothing is
// disabled silently.
enum class McpUnavailableCode : std::uint8_t {
    Available = 0,
    NotApplicable = 1,
    // Start while Running.
    AlreadyRunning = 2,
    // Stop while Stopped or Failed.
    NotRunning = 3,
    // Start, Stop or SetPort while Stopping.
    Stopping = 4,
    // SetPort outside kMcpMinimumPort..kMcpMaximumPort (field: port).
    PortOutOfRange = 5,
    // Start: another program holds 127.0.0.1:<port> (field: port). `port` names it.
    PortInUse = 6,
    // Start: the operating system refused the port (field: port).
    PermissionDenied = 7,
    // Start: no loopback interface (field: status).
    AddressUnavailable = 8,
    // Start: out of sockets or file descriptors (field: status).
    ResourceExhausted = 9,
    // SetPort while Running: "Stop the server to change the port" (field: port).
    ServerRunning = 10,
    // Start, RevealToken, CopyClientConfig while the token file is being read or written.
    TokenLoading = 11,
    // The token could not be read or written (no user-data folder, permissions). RegenerateToken
    // stays available to retry (field: token).
    TokenUnavailable = 12,
    // Every command after beginShutdown().
    ShuttingDown = 13,
    SystemError = 14,
};

// Which panel field a refusal belongs to.
enum class McpField : std::uint8_t { Status = 0, Port = 1, AutoStart = 2, Token = 3, Clients = 4 };

struct McpCommandAvailability final {
    bool available = false;
    McpUnavailableCode code = McpUnavailableCode::NotApplicable;
    McpField field = McpField::Status;
    // The port a PortInUse/PermissionDenied/PortOutOfRange refusal is about.
    std::optional<std::uint32_t> port;
    // Tooltip-ready; empty when available, never empty when not.
    std::string reason;
    [[nodiscard]] explicit operator bool() const noexcept { return available; }
};

struct McpCommandAvailabilitySet final {
    std::array<McpCommandAvailability, kMcpServerCommandCount> entries{};
    [[nodiscard]] const McpCommandAvailability& operator[](const McpServerCommand command) const {
        return entries[static_cast<std::size_t>(command)];
    }
};

struct McpCommandResult final {
    bool accepted = false;
    // When refused: the refusal. A refusal the snapshot could not predict (a bind result:
    // PortInUse, PermissionDenied, AddressUnavailable, ResourceExhausted) is also recorded as
    // McpServerSnapshot::failure, so the status line keeps showing it after the command returns.
    McpCommandAvailability availability;
    // The snapshot revision published by the pump() the command ran before returning.
    std::uint64_t revision = 0;
};

// ---------------------------------------------------------------------------------------------
// Snapshot

struct McpEndpoint final {
    // Always kLoopbackAddress; never a wildcard.
    std::string host = "127.0.0.1";
    std::uint16_t port = kMcpDefaultPort;
    std::string path = std::string(kMcpEndpointPath);
    // "http://127.0.0.1:6767/mcp".
    std::string url;
};

enum class McpTokenState : std::uint8_t {
    Loading = 0,
    Ready = 1,
    Regenerating = 2,
    Unavailable = 3,
};

// The token as the snapshot shows it: masked only. The full value is returned by revealToken() and
// embedded by clientConfig(), and by nothing else.
struct McpTokenView final {
    McpTokenState state = McpTokenState::Loading;
    // maskToken(token) when Ready; empty otherwise.
    std::string masked;
    // Advances on every regeneration (clients authorised with an older token are disconnected).
    std::uint64_t generation = 0;
    // Why the token is Unavailable; empty otherwise.
    std::string reason;
};

// One connected MCP client (one Mcp-Session-Id). The session id itself is never exposed.
struct McpClientView final {
    // Display id, monotonic per run, starting at 1; not the Mcp-Session-Id.
    std::uint64_t id = 0;
    // From initialize (sanitised).
    McpClientInfo info;
    std::chrono::system_clock::time_point connectedAt;
    std::chrono::system_clock::time_point lastActivityAt;
    std::uint32_t inFlight = 0;
    std::uint64_t requests = 0;
    std::optional<McpTool> lastTool;
};

// Why a client went away.
enum class McpDisconnectCause : std::uint8_t {
    // DELETE /mcp from the client.
    Ended = 0,
    // No request for McpServerLimits::sessionIdle.
    Expired = 1,
    // Evicted, least recently used and idle, to admit a new client at the client limit.
    Evicted = 2,
    TokenRegenerated = 3,
    ServerStopped = 4,
};

// The live project as the server sees it.
struct McpLiveProjectView final {
    bool bound = false;
    std::uint64_t generation = 0;
    // "A project is opening", "Bloom is closing"; empty when bound.
    std::string reason;
};

struct McpServerSnapshot final {
    // Monotonic per service; advanced only by pump().
    std::uint64_t revision = 0;
    McpServerState state = McpServerState::Stopped;
    // The configured preferences. The src/ui adapter persists them when preferencesRevision moves.
    McpServerPreferences preferences;
    std::uint64_t preferencesRevision = 0;
    // Present while Running or Stopping.
    std::optional<McpEndpoint> endpoint;
    // The last refused start or listener failure; cleared by a successful start or by SetPort.
    std::optional<McpServerFailure> failure;
    McpTokenView token;
    // Connection order.
    std::vector<McpClientView> clients;
    std::uint32_t activeRequests = 0;
    std::uint64_t totalRequests = 0;
    // The activity log is read with activity(afterId); the snapshot carries its newest id and
    // entry.
    std::uint64_t lastActivityId = 0;
    McpLiveProjectView project;
    McpCommandAvailabilitySet commands;
};

// ---------------------------------------------------------------------------------------------
// Activity log

enum class McpActivityKind : std::uint8_t {
    ServerStarted = 0,
    ServerStopped = 1,
    StartRefused = 2,
    ListenerFailed = 3,
    ClientConnected = 4,
    ClientDisconnected = 5,
    ToolCall = 6,
    // A request refused before the protocol core (McpHttpRejection), e.g. a wrong token or a
    // browser Origin. Repeated identical rejections within one second are counted in one entry.
    RequestRejected = 7,
    ProjectReplaced = 8,
    TokenRegenerated = 9,
    TokenRevealed = 10,
};

// One line of the activity log. `summary` is built by the service from typed facts (tool name,
// operation ids, outcome, rejection kind); it never contains argument values, request or response
// bodies, headers, the token or the session id.
struct McpActivityEntry final {
    // Monotonic per service, starting at 1, never reused.
    std::uint64_t id = 0;
    std::chrono::system_clock::time_point at;
    McpActivityKind kind = McpActivityKind::ToolCall;
    // McpClientView::id, when the entry belongs to a client.
    std::optional<std::uint64_t> client;
    std::optional<McpTool> tool;
    std::optional<McpCallOutcome> outcome;
    std::optional<McpToolErrorCode> toolError;
    std::optional<McpHttpRejection> rejection;
    // Rejections folded into this entry (RequestRejected only).
    std::uint32_t repeat = 1;
    std::chrono::milliseconds duration{0};
    // At most 256 bytes.
    std::string summary;
};

struct McpActivityBatch final {
    std::vector<McpActivityEntry> entries;
    // Entries after the requested id were dropped from the bounded ring before being read.
    bool gap = false;
    std::uint64_t lastId = 0;
};

// ---------------------------------------------------------------------------------------------
// Typed events

struct McpServerStarted final {
    McpEndpoint endpoint;
};
struct McpServerStopped final {
    McpStopCause cause = McpStopCause::User;
};
struct McpServerStartRefused final {
    McpServerFailure failure;
};
struct McpServerListenerFailed final {
    McpServerFailure failure;
};
struct McpClientConnected final {
    McpClientView client;
};
struct McpClientDisconnected final {
    std::uint64_t client = 0;
    McpDisconnectCause cause = McpDisconnectCause::Ended;
};
struct McpTokenRegenerated final {
    std::uint64_t generation = 0;
};

using McpServerEventPayload =
    std::variant<McpServerStarted, McpServerStopped, McpServerStartRefused, McpServerListenerFailed,
                 McpClientConnected, McpClientDisconnected, McpTokenRegenerated>;

struct McpServerEvent final {
    std::uint64_t sequence = 0;
    // The snapshot revision whose pump() published it.
    std::uint64_t revision = 0;
    McpServerEventPayload payload;
};

struct McpServerEventBatch final {
    std::vector<McpServerEvent> events;
    bool gap = false;
    std::uint64_t lastSequence = 0;
};

// ---------------------------------------------------------------------------------------------
// Client configuration (generated by the host, never by the UI)

enum class McpClientKind : std::uint8_t {
    // A `claude mcp add --transport http ...` command line.
    ClaudeCode = 0,
    // An `mcpServers` JSON object with type "http", url and headers (project .mcp.json files and
    // clients that read the same shape).
    McpJson = 1,
    // A VS Code `mcp.json` `servers` entry with type "http".
    VsCode = 2,
    // The URL and the Authorization header as two plain lines, for any other client.
    Generic = 3,
};

inline constexpr std::size_t kMcpClientKindCount = 4;

enum class McpTokenDisclosure : std::uint8_t {
    // The text carries the token itself (what "Copy client config" uses).
    IncludeToken = 0,
    // The text carries "${BLOOM_MCP_TOKEN}" instead, for configs that are shared or committed.
    EnvironmentPlaceholder = 1,
};

struct McpClientConfig final {
    McpCommandAvailability availability;
    McpClientKind kind = McpClientKind::Generic;
    // "Claude Code", "MCP JSON", "VS Code", "Other client".
    std::string title;
    // Ready to paste.
    std::string text;
    // One plain-language line on where to paste it.
    std::string hint;
};

// Pure: the text for `kind` against `endpoint`, with `credential` (the token or the placeholder)
// in the Authorization header. Deterministic, so the contract tests pin every kind's text.
[[nodiscard]] std::string clientConfigText(McpClientKind kind, const McpEndpoint& endpoint,
                                           std::string_view credential);

struct McpTokenReveal final {
    McpCommandAvailability availability;
    // The full token when available; empty otherwise. The caller must not log it.
    std::string token;
};

// ---------------------------------------------------------------------------------------------
// Subscription (worker threads and the src/ui waiter)

// Same shape as runtime::TaskCompletionSubscription: the serial advances whenever anything a pump
// would publish has been recorded. No callback, payload or service lifetime is retained. close()
// is concurrent and idempotent; shutdown wakes waiters.
class McpServerSubscription final {
  public:
    McpServerSubscription() = default;
    McpServerSubscription(McpServerSubscription&&) noexcept;
    McpServerSubscription& operator=(McpServerSubscription&&) noexcept;
    McpServerSubscription(const McpServerSubscription&) = delete;
    McpServerSubscription& operator=(const McpServerSubscription&) = delete;
    ~McpServerSubscription();

    [[nodiscard]] std::uint64_t serial() const noexcept;
    // Blocks the calling waiter thread; never call it on the UI thread.
    [[nodiscard]] std::optional<std::uint64_t> waitForChange(std::uint64_t observed,
                                                             const std::stop_token& stop) const;
    void close() noexcept;

  private:
    struct State;
    friend class McpServerService;
    explicit McpServerSubscription(std::shared_ptr<State> state) noexcept;
    std::shared_ptr<State> state_;
};

// ---------------------------------------------------------------------------------------------
// Service

struct McpServerLimits final {
    // Concurrent MCP sessions (clients). At the limit, initialize evicts the least recently used
    // client that has no request in flight and has been idle for clientEvictionIdle; otherwise it
    // is refused with ClientLimitReached (HTTP 503).
    std::size_t clients = 8;
    std::chrono::minutes clientEvictionIdle{5};
    // A client with no request for this long is expired (its next request gets 404 and it
    // re-initializes, as the specification requires).
    std::chrono::minutes sessionIdle{60};
    // Open HTTP connections; more are answered 503 and closed.
    std::size_t connections = 16;
    // Request workers executing handle(); requests beyond them wait in the pending queue
    // (McpMessageLimits::pendingMessages, 16) and beyond that are refused with ServerBusy.
    std::size_t requestWorkers = 4;
    // Requests one client may have executing or pending at once.
    std::size_t inFlightPerClient = 4;
    std::size_t activityEntries = 512;
    std::size_t retainedEvents = 128;
    McpHttpLimits http;
};

// Borrowed collaborators; all must outlive the service. The composition root passes the same
// application-wide instances the rest of the application uses.
struct McpServerServiceDependencies final {
    scripting::LiveSessionHost& live;
    host::ExportQueueService& exports;
    runtime::TaskScheduler& scheduler;
    // platform::userDataDirectory() / "mcp" / "token"; empty makes the token Unavailable with the
    // reason "No user data folder".
    std::filesystem::path tokenFile;
    // platform::userCacheDirectory() / "mcp-captures"; see McpCoreDependencies::captureDirectory.
    std::filesystem::path captureDirectory;
    std::string serverVersion;
    // Read by the src/ui adapter from the application preferences at launch.
    McpServerPreferences preferences;
    McpServerLimits limits;
};

class McpServerService final {
  public:
    explicit McpServerService(McpServerServiceDependencies dependencies);
    ~McpServerService();
    McpServerService(const McpServerService&) = delete;
    McpServerService& operator=(const McpServerService&) = delete;

    // ---- Any thread. Non-blocking, no I/O.
    [[nodiscard]] std::shared_ptr<const McpServerSnapshot> snapshot() const;
    [[nodiscard]] McpServerEventBatch events(std::uint64_t afterSequence) const;
    // Entries after `afterId`, oldest first, from the bounded ring (limits.activityEntries).
    [[nodiscard]] McpActivityBatch activity(std::uint64_t afterId) const;
    [[nodiscard]] McpServerSubscription subscribe();

    // ---- Owner thread (desktop: UI thread). Non-blocking, no I/O except start()'s bind.

    // Consumes recorded network, request and task facts, loads or stores the token through tasks,
    // runs a pending auto-start once the token is Ready, expires idle clients, and publishes a new
    // snapshot when anything changed. The only publisher.
    void pump();

    [[nodiscard]] McpCommandAvailability availability(McpServerCommand command) const;

    // Binds 127.0.0.1:<preferences.port> and starts serving. Refusals: AlreadyRunning, Stopping,
    // TokenLoading, TokenUnavailable, PortInUse (naming the port), PermissionDenied,
    // AddressUnavailable, ResourceExhausted, ShuttingDown. Never tries another port.
    McpCommandResult start();
    // Closes the listener at once, ends every client session, cancels in-flight requests (queued
    // commits are withdrawn; a commit already applying finishes; transient render/export/capture
    // jobs are stopped; queue jobs started with export.start keep running -- they belong to the
    // Export panel), and reaches Stopped when the request workers are idle. Refusals: NotRunning,
    // Stopping, ShuttingDown.
    McpCommandResult stop();
    // Refusals: PortOutOfRange (naming the value), ServerRunning, Stopping, ShuttingDown. The value
    // is remembered as a preference.
    McpCommandResult setPort(std::uint32_t port);
    McpCommandResult setAutoStart(bool enabled);
    // Generates a new token (platform CSPRNG) and writes it on the blocking-I/O executor. When the
    // write lands, every connected client is disconnected (TokenRegenerated) and must use the new
    // token. Refusals: TokenLoading, ShuttingDown.
    McpCommandResult regenerateToken();
    // The full token for an explicit Reveal or Copy action, and an activity entry saying it was
    // revealed (without the value). Refusals: TokenLoading, TokenUnavailable.
    [[nodiscard]] McpTokenReveal revealToken();
    // Client configuration text for `kind`, using the configured port (valid while Stopped too).
    // Refusals as revealToken() when `disclosure` is IncludeToken.
    [[nodiscard]] McpClientConfig clientConfig(McpClientKind kind,
                                               McpTokenDisclosure disclosure) const;

    // Launch edge: starts the server from pump() once the token is Ready, when
    // preferences.autoStart is set. Called once by the composition root.
    void requestAutoStart();

    // Shutdown edge: as stop() with cause Shutdown, and refuses every subsequent command.
    // Non-blocking; the owner keeps pumping until quiescent(). No prompt: an agent's request is not
    // artist work that would be lost (commits are atomic; transient jobs only ever wrote staged
    // files).
    void beginShutdown();
    [[nodiscard]] bool quiescent() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace bloom::mcp
