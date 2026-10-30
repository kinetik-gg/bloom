# ADR 0034: In-App MCP Server And The MCP Server Panel

Status: accepted (owner and UI review of 2026-10-30; the supervisor may renumber the ADR at
integration)

Date: 2026-10-30

Amendment (2026-10-31): [ADR 0035](0035-media-provider-fallback-chain.md) made export work on every
platform. The `PlatformNotSupported` codes (`ExportAvailabilityCode` 7, `ExportUnavailableCode` 23)
and `ExportEncoderStatus::exportPlatform` no longer exist, and render, export, capture and
`export.start*` are no longer refused on macOS and Windows. The statements below that say
otherwise are kept as history.

## Context

`apps/bloom-mcp` is a Qt-free MCP 2025-11-25 server on stdio. It opens its own headless session
and offers five tools: `query`, `transact`, `render`, `export` and `events`. Every edit goes
through the descriptor-driven operation registry, so new operations reach MCP without MCP changes.
An agent connected to it edits a separate copy of the project: its transactions never reach the
artist's open project, its undo history or its panels.

The owner asked:

> yes, we need to start MCP work. I also want a dedicated panel for MCP Server to start or stop it
> and maybe choose port. default port is 6767

The design must keep Qt in `src/ui` and `apps`, keep module dependencies declared and acyclic,
keep request handling off the UI thread, treat Linux, macOS and Windows as first-class, follow the
MCP specification's security rules for local HTTP servers, and use the export queue that is being
rebuilt concurrently (ADR 0033 section 13: MCP `render` and `export` become `bloom.export.*` queue
operations) instead of a render path of its own. The UI lead, who builds the panel, needs six
guarantees stated in the contract: a non-blocking snapshot with main-thread change events from one
application-lifetime adapter, typed command refusals, a token that never appears in text, an
incrementally readable bounded activity log, host-generated client configuration, and port, token
and auto-start held as preferences.

## Decision

### 1. Ownership and modules

| Module | Owns |
| --- | --- |
| `src/platform` | `LoopbackListener` and `LoopbackConnection` (POSIX sockets on Linux and macOS, Winsock 2 on Windows; `bloom/platform/loopback_listener.hpp`); the CSPRNG and owner-only secret files (`bloom/platform/owner_only_secret.hpp`) |
| `src/scripting` | `LiveSessionHost`: the live session mirror, the bounded commit marshal, generations and leases (`bloom/scripting/live_session_host.hpp`), moved out of the Script editor's private runtime |
| **`src/mcp`** (new) | **the shared server core and the in-app server**: `McpServerCore` (JSON-RPC, handshake, tool schemas, tool handlers, projections, command-event history), the stdio transport, Streamable HTTP framing and admission, and `McpServerService` (lifecycle, client sessions, request workers, activity log, events, token, client configuration, snapshots) (`bloom/mcp/protocol.hpp`, `server_core.hpp`, `streamable_http.hpp`, `server_service.hpp`) |
| `src/host` | the export queue's transient-job entry that render, export and capture use (section 5; requested of the export lead) |
| `src/ui` | the MCP Server editor panel; the application-lifetime `McpServerAdapter`; the application-lifetime live session adapter that feeds `LiveSessionHost` and drains commits (the Script editor uses it too); persistence of the port and auto-start preferences; the shutdown participant |
| `apps/bloom` | wiring: constructs `LiveSessionHost`, `McpServerService` and the adapters, calls `requestAutoStart()`, registers shutdown |
| `apps/bloom-mcp` | a thin `main` over `src/mcp`: open the session, construct the headless export queue and the core, `serveStdio()` |
| `tools/quality/repository_checks` | `mcp` in the known modules and its declared edges |

`src/mcp` depends on `scripting`, `host`, `output`, `runtime`, `commands`, `document`, `platform`
and `core`. Nothing in `src` depends on `src/mcp`; only `src/ui` and the two apps link it, so the
graph stays acyclic. The edges are declared in `repository_checks.cpp` with the ADR reference.

A new module rather than more of `src/scripting`: `src/scripting` is the language-neutral facade
that Python embeds (ADR 0022). A wire protocol, an HTTP parser, a session table and a server
lifecycle are a separate capability with their own owner, consumed by two transports and the
desktop panel; folding them into the facade would make every Python build link the HTTP server.
ADR 0033 rejected a `src/export` module because it had no owner independent of `src/host`; this
module does.

### 2. One server core, two transports

Everything `apps/bloom-mcp` implements today moves into `src/mcp`: `json.cpp`, `server.cpp`,
`tools.cpp`, `projections.cpp`, `tool_schemas.cpp` and `transport.cpp` (the stdio transport becomes
`mcp::serveStdio`). `render.cpp`'s direct `Render::run` route is replaced by export queue jobs
(section 5). `apps/bloom-mcp/main.cpp` remains as the stdio composition root.

- **Session source.** The core reads and commits through `McpSessionSource`, a variant of either a
  headless `scripting::Session` the stdio host owns or the desktop's `scripting::LiveSessionHost`.
  It is a typed choice between two concrete owners, not an interface.
- **Protocol sessions.** Handshake state, client identity and cursor binding move into
  `McpProtocolSession`. stdio has exactly one; the HTTP host has one per `Mcp-Session-Id`.
- **Concurrency.** `handle()` may run concurrently for different clients and for parallel requests
  from one client after initialize. Reads use immutable snapshots; commits serialise in the
  marshal; jobs serialise in the export runner.
- **Cancellation.** stdio cancellation today cancels every scheduler task while a call runs. Both
  hosts now give each request its own `std::stop_token`, so `notifications/cancelled` cancels only
  that request's work and never an unrelated application task.
- **Compatibility.** The stdio host keeps its command line, its handshake, its limits and every
  existing tool's request and result shape. The existing `bloom.scripting.mcp-pipes` conformance
  test passes unchanged; the new tools are additive.

### 3. Transport: Streamable HTTP on loopback

- **Address.** `127.0.0.1` only, literal, never a wildcard or a resolved name, so the server is
  unreachable from other machines by construction. The default port is **6767**; the port is a
  preference in 1024–65535.
- **Endpoint.** One path, `/mcp`. `POST` carries exactly one JSON-RPC message (batches were
  removed in 2025-11-25 and are refused with 400). A request is answered with `application/json`;
  a task tool call (render, export, capture, `export.wait`) that carries `_meta.progressToken` is
  answered with `text/event-stream`: `notifications/progress` events, then the response, then the
  stream closes. A notification or client response is answered 202 with no body. `DELETE` ends the
  caller's session (204). `GET` is 405: Bloom sends no server-initiated messages, so it offers no
  standalone stream. Every other path, including OAuth discovery under `/.well-known/`, is 404.
- **Sessions.** `initialize` creates a protocol session and returns `Mcp-Session-Id`: 128 bits from
  the platform CSPRNG as 32 hex digits. Every subsequent request must carry it: missing is 400; unknown,
  ended or expired is 404, and the client starts a new session as the specification requires. The
  session id is not a credential and is never shown in the panel or the log.
- **Protocol version.** `MCP-Protocol-Version`, when present, must equal the negotiated version
  (`2025-11-25`), otherwise 400. When absent the request is accepted, as the specification allows
  for compatibility.
- **Admission order.** Each check runs before the next, so a browser page learns nothing beyond
  the first check it fails:
  1. **Host** must be exactly `127.0.0.1:<port>` or `localhost:<port>`, otherwise 403. This defeats
     DNS rebinding even if a browser omits `Origin`.
  2. **Origin**, when present, must be on the allow-list, otherwise 403. The allow-list is empty:
     Bloom serves no web page, so no browser origin is trusted, and `null` is never accepted.
     Non-browser MCP clients send no `Origin`. No CORS headers are ever sent, so preflights fail.
  3. **Path** must be `/mcp`, otherwise 404.
  4. **Authorization** must be `Bearer <token>`, compared in constant time, otherwise 401 with
     `WWW-Authenticate: Bearer realm="bloom"`.
  5. **Method** must be POST or DELETE, otherwise 405 with `Allow: POST, DELETE`.
  6. A POST needs `Content-Type: application/json` (otherwise 415) and an `Accept` that permits
     both `application/json` and `text/event-stream` (otherwise 406).
  7. Session and protocol-version checks.
- **HTTP subset.** HTTP/1.1 only (others are 505). Request bodies use `Content-Length` or chunked
  coding, never both and never another coding. JSON responses carry `Content-Length` and keep the
  connection alive; event streams are close-delimited. No compression, ranges or upgrades.
- **Bearer token: required, with no off switch.** Any local process can reach loopback, including
  processes of other users on a shared computer and malware running as the artist. Host and Origin
  checks stop browsers, not local processes, and this server can edit the open project and write
  files through export. The token is 256 bits from the platform CSPRNG, written as `bloom_` plus 43
  base64url characters. It is created on first use and stored in an owner-only file in the
  platform user-data directory (`mcp/token`: mode 0600 in a 0700 folder on Linux and macOS, a
  protected DACL for the current user on Windows), read and written on the blocking-I/O executor.
  A token file that other users can read is replaced. **Regenerate** writes a new token and then
  disconnects every client. Bloom does not implement MCP OAuth: OAuth serves remote servers with
  third-party identity, and a local single-user server has no authorization server to delegate to.
  A static bearer header is what common MCP clients accept for HTTP servers.
- **How a client gets the token.** The panel's **Copy client config** puts ready-to-paste text on
  the clipboard for the chosen client, with the URL and the Authorization header. The host
  generates the text (`McpServerService::clientConfig`, pure `clientConfigText`), not the UI:
  - Claude Code: `claude mcp add --transport http bloom http://127.0.0.1:6767/mcp --header
    "Authorization: Bearer <token>"`;
  - MCP JSON (`.mcp.json` and clients that read it):
    `{"mcpServers":{"bloom":{"type":"http","url":"http://127.0.0.1:6767/mcp","headers":{"Authorization":"Bearer <token>"}}}}`;
  - VS Code (`mcp.json`):
    `{"servers":{"bloom":{"type":"http","url":"http://127.0.0.1:6767/mcp","headers":{"Authorization":"Bearer <token>"}}}}`;
  - Other client: the URL and the header as two plain lines.

  Each kind can also be copied with `${BLOOM_MCP_TOKEN}` in place of the token, for configuration
  that is shared or committed. **Reveal** and **Copy** token are explicit actions as well.
- **HTTP implementation: a small in-tree server.** The dependency intake contract (ADR 0019,
  `architecture/dependency-intake.md`) gives every library a lock entry, an offline superbuild
  recipe, license review and qualification on three platforms. The candidates each bring more than
  this server can use:
  - cpp-httplib and civetweb: TLS, file serving and threading models Bloom does not control;
  - libmicrohttpd: LGPL obligations;
  - Boost.Beast: Boost.Asio;
  - Qt HTTP Server: GPL or commercial terms only, and it would put the server in `src/ui`, so the
    stdio host could not share it.

  The required subset is small and bounded: HTTP/1.1, two methods, bodies of at most 1 MiB,
  close-delimited event streams, and one loopback address. The socket calls live in `src/platform`,
  split by backend like the staged-artifact provider. Framing and admission are pure functions in
  `src/mcp` that are tested with byte strings and a hostile-input corpus.

### 4. The live session, threading and backpressure

**The marshal.** Today `ScriptPanelRuntime` (`src/ui`) privately mirrors the live snapshot and
context, and queues authoring onto the UI thread, one transaction per event-loop slice, with at
most 16 queued and a generation that revokes on project replacement. The in-app server needs the
same mechanism. It moves, Qt-free, into `scripting::LiveSessionHost`, so the Script editor and MCP
share one mirror, one queue, one generation counter and one revocation rule. The application-
lifetime adapter in `src/ui` publishes the mirror on every snapshot, composition, selection, time,
colour-settings, display/view and save change. Its drain applies at most one commit per event-loop
slice. It wakes on `LiveCommitSubscription` instead of the 16 ms poll the Script editor uses today.

**The request path.**

```text
connection thread    accept, read with deadlines, parse, admit (Host, Origin, token, ...)
      | pending queue (16, shared with stdio's bound)
request worker (4)   McpServerCore::handle -> LiveSessionHost::acquire -> lease
      | reads: the lease's immutable snapshot (worker thread)
      | commits: queued to the owner thread, the worker waits
UI thread            adapter drain: one commit per slice, through CompositionSession
      | export jobs: scheduler tasks owned by the export queue
worker -> connection thread   response or event stream
```

The UI thread never parses, admits, handles a request or waits on one. Its only part is the bounded
commit drain the live session already uses.

**Commits.** One `transact` is one atomic transaction and one undo step. It is labelled with the
transaction's `label`, or "Agent edit" when there is none, and it is visible in every panel when it
applies. `expectedRevision` is required, as today; a conflict leaves the document unchanged. A
commit never applies while the artist holds an open interactive gesture (ADR 0017): it waits in
the queue until the gesture closes. A commit that is still queued when its request is cancelled,
when its deadline passes (`kMcpCommitDeadline`, 30 s) or when its generation is revoked is
withdrawn and never applied. Its result names why, with a stable code: `bloom.live.gesture-open`,
`bloom.live.cancelled`, `bloom.live.deadline-exceeded` or `bloom.live.project-replaced`. A commit
that has started applying finishes atomically. The `history` tool queues undo and redo the same
way.

**Contextual arguments** fill from the live context (project, composition, selected nodes,
selected layers, time), exactly as the Python console does. Align and distribute measure geometry
on the request worker, never the UI thread.

**Project close and open.** Clients stay connected: their session belongs to the application, not
to the project. Replacement revokes the generation. Queued commits are withdrawn with
`ProjectReplaced`, calls in flight fail with `bloom.mcp.project-replaced`, and calls made while a
project is opening fail with `bloom.mcp.project-unavailable` and the reason. The `events` cursor
reports `resetRequired`, and paging cursors, which already bind the session, project and revision,
stop matching. The activity log records the replacement.

**No project-lifecycle tools.** The server does not open, close, save or create projects. Those
flows ask the artist about unsaved changes and destinations; the artist owns them.

**Limits and backpressure.**

| Bound | Value | Refusal |
| --- | --- | --- |
| Request body | 1 MiB (unchanged) | 413 |
| Response | 4 MiB (unchanged) | capture links instead of embedding (section 5) |
| JSON values / depth / arena | 65,536 / 32 / 32 MiB (unchanged) | -32602 |
| Request line plus headers | 16 KiB, 64 fields | 431 and close |
| Header / body / write deadlines | 10 s / 30 s / 30 s | close |
| Idle keep-alive | 60 s | close |
| Open connections | 16 | 503 and close |
| Clients (protocol sessions) | 8 (see section 6) | 503, -32002 |
| Request workers | 4 | wait in the pending queue |
| Pending messages | 16 (unchanged) | 503 with Retry-After: 1, -32000 |
| Requests per client, executing or pending | 4 | 503, -32000 |
| Queued commits, all clients and the Script editor | 16 (unchanged) | `bloom.mcp.commit-queue-full` |
| Render request | 10,000 frames (unchanged) | -32602 |
| Command events retained | 1,024 (unchanged) | `resetRequired` |

Client disconnection is not cancellation, as the specification requires. A request finishes and
its response is dropped. Cancellation is `notifications/cancelled` (scoped to the caller's own
session), server stop or shutdown.

### 5. Tools on the live application

Both hosts list the same tools with the same schemas.

| Tool | Live application | stdio |
| --- | --- | --- |
| `query` | unchanged kinds, plus `context`: project, composition, selected nodes and layers, time, the Viewer's display and view, read-only state, `live: true` | `context` returns the session defaults with `live: false` |
| `transact` | queued commit into the artist's undo history (section 4) | direct on the headless session, as today |
| `history` | `action` `undo` or `redo`, with `expectedRevision`; queued like a commit | direct |
| `events` | the live command events; resets on project replacement | as today |
| `render`, `export` | unchanged request shapes; a transient export job (ADR 0033 section 13) | the same, on a headless export queue |
| `capture` | one PNG of the context composition at the context time, using the Viewer's display and view, returned as an MCP `image` | the session composition at time 0, with the project's default display and view |
| `export.start`, `export.start-all`, `export.pause`, `export.resume`, `export.stop`, `export.stop-all`, `export.retry`, `export.confirm-replace`, `export.status`, `export.events`, `export.log`, `export.wait`, `export.presets`, `export.script` | the `ExportQueueService` session calls on the artist's queue, which the Export panel shows | the same on the headless queue |

The project-truth `bloom.export.*` edits are ordinary operations, so `transact` reaches them through
the registry with no MCP change.

- **One render path.** `render`, `export` and `capture` submit transient, unsaved jobs to the
  `ExportJobRunner` through `ExportQueueService`. They get the same pipeline, preflight, verification,
  strict Viewer GPU priority (ADR 0033 section 9) and publication as every other export. MCP keeps
  no `Render::run` route and no GPU provider of its own. The queue owns the provider.
- **What the export lead is asked for** (the only MCP need in `src/host`):
  - a transient-job entry on `ExportQueueService` that takes a composition, a range, one output's
    settings and a destination, and returns the job id or the refusing availability;
  - the job is never saved;
  - it is marked transient in the snapshot;
  - its tasks are owned and grouped as any job's are;
  - it can be stopped by id;
  - it is removed from the snapshot once terminal and read.
- **Progress.** A task tool reads its job's progress from the export queue snapshot. It waits on
  `ExportQueueSubscription` on the request worker, never the UI thread. When the request carried a
  progress token, it emits `notifications/progress` (frames completed of total, with the step
  message, including "Waiting for the Viewer" with no ETA).
- **Cancellation** of a task tool stops its transient job. Queue jobs started with `export.start`
  belong to the Export panel and are not stopped by MCP request cancellation, server stop or a
  client disconnect.
- **Unattended replacement.** Nothing replaces a file silently (ADR 0033 section 13). An agent sets
  the overwrite policy to Replace or calls `export.confirm-replace`.
- **Capture.** Capture publishes through the still route into a per-user capture folder
  (`<user cache>/mcp-captures`, newest 16 kept). When the PNG fits `kMcpInlineImageBytes` (about
  2.9 MiB, so the base64 image and the envelope stay within the unchanged 4 MiB response limit), it
  is embedded as `image` content. Otherwise the result carries a `resource_link` to the published
  file and `bloom.mcp.capture-too-large-for-inline`. Capture renders final pixels at composition
  resolution, not the Viewer's preview resolution. It adds no resample operation: a downscaled
  capture would be a new pixel operation with its own GPU coverage. During playback or RAM preview
  a capture waits for the Viewer, like any export.
- **GPU coverage is unchanged.** MCP adds no operation, kernel, lowering or route. Capture is the
  still-export route, with its one readback at the PNG encoder boundary. No opt-out is created.
- **Platforms.** Render, export, capture and `export.start*` follow export availability
  (ADR 0033 section 15). On macOS and Windows they answered `bloom.mcp.export-unavailable` carrying
  `PlatformNotSupported`; superseded by ADR 0035, see the amendment note below. Every other tool
  behaves identically on all three platforms.

### 6. Lifecycle and state ownership

```text
Stopped --start (bound)--> Running --stop--> Stopping --quiescent--> Stopped
Stopped --start (bind refused)--> Stopped (typed refusal; failure shown)
Running --listener failure--> Failed --start--> Running
Failed  --setPort--> Stopped
```

- **Start** binds `127.0.0.1:<port>` synchronously. Binding a loopback port is one bounded system
  call with no filesystem or network I/O, which lets the command refuse a taken port on the spot
  with `PortInUse` naming the port ("Port 6767 is in use. Choose another port or stop the other
  program."). The refusal is also kept as the snapshot's `failure`, so the status
  line keeps showing it. Bloom **never tries another port**: a client configured for 6767 must
  never reach a server that silently moved. Start needs the token loaded (`TokenLoading`,
  `TokenUnavailable`).
- **Stop** closes the listener at once and ends every client session. In-flight requests are
  cancelled:
  - queued commits are withdrawn; a commit already applying finishes;
  - transient jobs stop;
  - queue jobs started with `export.start` keep running.

  The state is Stopped when the request workers are idle.
- **Port** changes only while the server is stopped (`ServerRunning`: "Stop the server to change
  the port."). Restarting silently on a new port would drop clients without telling them. A value
  outside 1024–65535 is `PortOutOfRange` naming the value ("Port 80 can't be used. Choose a port
  from 1024 to 65535."). The free, non-blocking `validateMcpPort(value)` returns exactly that
  refusal (code, field, value and reason), so the Port field can show it while the artist types.
  `setPort()` stays the authority: it checks ShuttingDown, Stopping and ServerRunning from the
  server's state first, then returns `*validateMcpPort(value)` for an out-of-range value. The value
  is remembered.
- **Auto-start** is a preference, off by default. When on, the composition root calls
  `requestAutoStart()` once at launch, and `pump()` starts the server as soon as the token has
  loaded. A refused auto-start (port taken) leaves the server Stopped with the failure shown in
  the panel. The application does not interrupt the artist with a dialog.
- **Clients.** Up to 8 concurrent protocol sessions, which allows several agents and tools at once
  while bounding memory and the panel list. At the limit, initialize evicts the least recently used
  client that has nothing in flight and has been idle for 5 minutes. Otherwise it is refused (503,
  -32002, "Bloom accepts at most 8 MCP clients."). A client idle for 60 minutes expires. Its next
  request gets 404 and it re-initializes.
- **Shutdown.** `ApplicationShutdownCoordinator` gains an MCP participant. `beginShutdown()` acts as
  stop with cause Shutdown and refuses every subsequent command; the coordinator keeps pumping until
  `quiescent()`. There is no prompt: commits are atomic and transient jobs only ever wrote staged
  files, so no artist work is lost.

| State | Kind | Where |
| --- | --- | --- |
| Port, auto-start | global preference (per user, outside projects, not undoable) | application preferences, `mcp/port` and `mcp/autoStart`; persisted by the `src/ui` adapter when `preferencesRevision` moves |
| Token | global preference | owner-only file `mcp/token` in the platform user-data directory; never in the settings store, a project, a log or a snapshot |
| Running or not, endpoint, failure, clients, requests, activity, events | session state (this run only, never saved) | `McpServerService` |
| Project truth | none | the server changes the document only through ordinary undoable commits |

The MCP Server panel is the one place these preferences are edited. The Preferences window
documents them and links to the panel rather than duplicating the controls.

### 7. The MCP Server panel contract

The panel is a registered editor type, id `bloom.mcp-server`, display name **MCP Server**,
registered with the other editors in `registerFoundationEditors`. It is replaceable like every
editor: it may be closed, duplicated or swapped, and it holds no server state.

**What the panel binds to.** One application-lifetime `McpServerAdapter` in `src/ui`, owned by the
composition root, holds the `McpServerService`.

- It owns a stop-aware waiter thread on `McpServerService::subscribe()` and coalesces wakes into at
  most one queued drain on the Qt main thread. It also drains on `TaskUiBridge::resultsAvailable`.
- Each drain calls `pump()` and, when the snapshot revision moved, emits one `changed()` signal
  that every MCP Server panel and the status bar read.
- The waiter exists because socket activity is not scheduler work, so on its own it would wake no
  drain. Otherwise this is the export queue's pattern: one publisher, one drain, panels never pump.

**The six guarantees.**

| # | Guarantee | API | Rule |
| --- | --- | --- | --- |
| 1 | The snapshot is read on the UI thread without blocking; changes arrive as main-thread events from one application-lifetime adapter | `snapshot()`, `pump()`, `McpServerAdapter::changed()` | `pump()` is the only publisher. `snapshot()` returns an immutable `shared_ptr` with no lock held across I/O. The adapter drains on its waiter and on `TaskUiBridge::resultsAvailable`. Panels never pump. |
| 2 | Start, stop and port change are commands with typed refusals shown as inline field reasons | `start()`, `stop()`, `setPort()`, `setAutoStart()`, `regenerateToken()`; `McpCommandResult`, `McpCommandAvailability{code, field, port, reason}`; `validateMcpPort(value)` for the Port field while typing | Codes: `AlreadyRunning`, `NotRunning`, `Stopping`, `PortOutOfRange`, `PortInUse` (naming the port), `PermissionDenied`, `AddressUnavailable`, `ResourceExhausted`, `ServerRunning`, `TokenLoading`, `TokenUnavailable`, `ShuttingDown`, `SystemError`. `field` names the control: Status, Port, AutoStart, Token or Clients. Every refusal has a reason string (below), shown verbatim; nothing is disabled silently. `validateMcpPort` and `setPort` return identical out-of-range refusals. |
| 3 | The token never appears in log, activity or diagnostic text; the snapshot shows it masked; only an explicit call returns it | `McpTokenView::masked`, `revealToken()`, `clientConfig(kind, IncludeToken)` | `maskToken()` shows the prefix and the last four characters. Activity entries carry typed facts; the fallback `summary` is built from the same facts and operation ids, never from bodies, headers or arguments. A reveal is logged without the value. |
| 4 | The activity log is a bounded ring with monotonically increasing ids, read incrementally | `activity(afterId)` → `McpActivityBatch{entries, gap, lastId}`; `snapshot().lastActivityId` | 512 entries; ids start at 1 and are never reused; `gap` reports overflow; repeated identical rejections within one second fold into one entry with `repeat`. Each entry carries `clientLabel` (the client's sanitised title, else its name, at most 64 bytes), captured when the entry is recorded, so lines from a disconnected client stay labelled. The panel builds the line it shows from the typed facts (`tool`, `outcome`, `toolError`, `rejection`, `repeat`, `duration`, `clientLabel`); `summary` is only a fallback and a diagnostic. |
| 5 | The host generates the client configuration text | `clientConfig(kind, disclosure)`, pure `clientConfigText(kind, endpoint, credential)` | Kinds: Claude Code, MCP JSON, VS Code, Other. The token or `${BLOOM_MCP_TOKEN}`. The text uses the configured port, so it can be copied while the server is stopped. |
| 6 | Port, token and auto-start are preferences, never project state | `McpServerPreferences`, `preferencesRevision`, the token file | See the state table in section 6. Nothing the panel does dirties the project or enters the undo history. |

**Reason strings.** Every `reason` the service returns (command refusals, `failure`, the token
view, the project view) is shown verbatim as an inline field reason under the control it belongs
to, so each one follows one style:

- plain sentence case ending with a full stop, naming the value it is about;
- no internal vocabulary: no codes, enum names, system error text, "socket", "bind" or "token
  file";
- a short first sentence saying what is wrong, optionally followed by a second giving the remedy;
- short enough to fit one line under a field (about 80 characters).

Examples: "Port 6767 is in use. Choose another port or stop the other program.", "Port 80 can't be
used. Choose a port from 1024 to 65535.", "Stop the server to change the port.", "A project is
opening.", "Bloom can't find a folder to keep the access token in."

**What the panel shows.**

- **Status line**:
  - "Stopped";
  - "Running on 127.0.0.1:6767" with the number of clients;
  - "Stopping…";
  - after a refused start or a listener failure, `failure.reason` verbatim, for example "Port 6767
    is in use. Choose another port or stop the other program."
- **Start/Stop** as one button, with availability from `commands[Start]` and `commands[Stop]`.
- **Port** field (default 6767). It is editable while the server is stopped. `validateMcpPort`
  gives the out-of-range reason while the artist types; refusals from `setPort` and `start` appear
  inline under the field.
- **Start when Bloom opens** check box (auto-start).
- **Token** row: the masked token, **Reveal**, **Copy** and **Regenerate…** (which confirms that
  every connected client will be disconnected).
- **Copy client config** menu, with the four kinds and a "with placeholder" variant of each.
- **Connected clients** table: name, version (from `initialize`), connected since, last activity,
  in-flight requests and the last tool.
- **Activity** list: time, client (`clientLabel`), tool, outcome, error or rejection, repeat count
  and duration, composed by the panel from the typed facts and appended incrementally with
  `activity(afterId)`. A `gap` shows "Earlier activity was trimmed."
- The project line from `McpLiveProjectView`, such as "A project is opening.", while no project is
  bound.

**Typed events** (`events(afterSequence)`, bounded ring of 128, with `gap`): `McpServerStarted`,
`McpServerStopped` (cause), `McpServerStartRefused` (failure), `McpServerListenerFailed`,
`McpClientConnected`, `McpClientDisconnected` (cause: Ended, Expired, Evicted, TokenRegenerated,
ServerStopped) and `McpTokenRegenerated`. A UI service may react to them, for example with the
status bar indicator or a notification. The engine has no notion of either.

### 8. Contract table

Every command is a session command on `McpServerService`. None is undoable and none touches the
project.

| Command | Available when (otherwise the stated code) | Effect |
| --- | --- | --- |
| `Start` | Stopped or Failed (`AlreadyRunning`, `Stopping`); token Ready (`TokenLoading`, `TokenUnavailable`); not shutting down (`ShuttingDown`) | binds `127.0.0.1:<port>`; a bind refusal returns `PortInUse`, `PermissionDenied`, `AddressUnavailable` or `ResourceExhausted` naming the port and is kept as `failure` |
| `Stop` | Running (`NotRunning`, `Stopping`, `ShuttingDown`) | closes the listener, ends sessions, cancels requests, Stopping then Stopped |
| `SetPort` | Stopped or Failed (`ServerRunning`, `Stopping`); 1024–65535 (`PortOutOfRange`, identical to `validateMcpPort`) | stores the preference and clears a failure about the old port |
| `SetAutoStart` | not shutting down | stores the preference |
| `RegenerateToken` | token not Loading or Regenerating (`TokenLoading`); not shutting down | writes a new token on the blocking-I/O executor, then disconnects every client |
| `RevealToken` | token Ready (`TokenLoading`, `TokenUnavailable`) | returns the token; logs "Token revealed" without it |
| `CopyClientConfig` | token Ready for `IncludeToken`; always for `EnvironmentPlaceholder` | returns the text for the chosen client |

| Read | Thread | Notes |
| --- | --- | --- |
| `snapshot()` | any | immutable; revision advanced only by `pump()` |
| `events(afterSequence)` | any | bounded, `gap` |
| `activity(afterId)` | any | bounded, `gap` |
| `subscribe()` | any; `waitForChange` never on the UI thread | serial wake for the adapter's waiter |
| `availability(command)` | owner | identical to the last published snapshot |
| `pump()` and all commands | owner (UI thread) | non-blocking; no I/O except `start()`'s bind |

### 9. Platforms

| Capability | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Loopback listener | POSIX, `SO_REUSEADDR`, `SOCK_CLOEXEC`, `MSG_NOSIGNAL` | POSIX, `SO_REUSEADDR`, `FD_CLOEXEC`, `SO_NOSIGPIPE` | Winsock 2, `SO_EXCLUSIVEADDRUSE`, non-inheritable sockets |
| Token file | 0600 in a 0700 folder under the XDG data home | 0600 in a 0700 folder under Application Support | protected DACL for the current user under `%LOCALAPPDATA%` |
| CSPRNG | `getrandom` | `getentropy` | `BCryptGenRandom` (system-preferred RNG) |
| Server, panel, clients, token, activity, query, transact, history, events | yes | yes | yes |
| Render, export, capture, `export.start*` | yes | yes (ADR 0035; was `PlatformNotSupported`) | yes (ADR 0035; was `PlatformNotSupported`) |
| Port-in-use detection | `EADDRINUSE` | `EADDRINUSE` | `WSAEADDRINUSE` or `WSAEACCES` under an exclusive bind |

Binding only the loopback address is what keeps the Windows Defender Firewall and the macOS
application firewall from asking the artist to allow incoming connections; the release check confirms
this on each system. Windows' exclusive bind prevents another process from binding the same port
over Bloom's listener.

### 10. Tests the implementation adds

- **Contract** (this change): `bloom.mcp.server-contract` compiles every contract header and pins
  the defaults, limits, tool names, refusal codes and HTTP statuses, and `validateMcpPort`'s
  refusal (code, field, value and reason) for in-range and out-of-range values.
- **HTTP conformance** (`bloom.mcp.http-conformance`, Python over a real socket against a headless
  `McpServerService` on an ephemeral test port):
  - `initialize` → `Mcp-Session-Id` → `notifications/initialized` (202) → `tools/list` →
    `tools/call query` → `DELETE` (204) → a request with the old id (404);
  - a progress-token `render` answered as `text/event-stream` with progress, then the result.
- **Admission:**
  - a wrong `Host` gives 403;
  - `Origin: http://evil.example` gives 403;
  - `Origin: null` gives 403;
  - a missing or wrong token gives 401 with `WWW-Authenticate`;
  - the token compares in constant time (a unit test over equal-length near misses);
  - `GET` gives 405 with `Allow`;
  - a wrong `Content-Type` gives 415;
  - a wrong `Accept` gives 406;
  - a batch gives 400;
  - a mismatched `MCP-Protocol-Version` gives 400;
  - a 1 MiB + 1 body gives 413;
  - a slow header (deadline) closes the connection;
  - the seventeenth connection gets 503;
  - the ninth client with no idle peer gets 503 and -32002.
- **Parser** (`bloom.mcp.http-parser`): a hostile corpus covering split reads, pipelining, chunked
  edge cases, both length headers, duplicate fields, oversized headers, header injection, NUL
  bytes and non-1.1 versions.
- **Port in use** (`bloom.platform.loopback-listener`, all three platforms):
  - a second listener on the same port gets `PortInUse` naming the port, and no other port is
    tried;
  - stop then start rebinds the same port immediately;
  - `wake()` unblocks `accept()`;
  - `close()` from another thread unblocks `read()`;
  - a non-loopback bind is impossible through the API (compile-time shape).
- **Token store** (`bloom.platform.owner-only-secret`):
  - created owner-only;
  - an exposed file is reported and replaced;
  - an atomic replace leaves no partial file;
  - the value never appears in any message.
- **Service** (`bloom.mcp.server-service`):
  - start, stop and restart;
  - refusals for every command state;
  - auto-start waits for the token;
  - regenerate disconnects clients and the old token gets 401;
  - activity ids are monotonic, `gap` works and folding works;
  - `clientLabel` is the title, else the name, capped at 64 bytes, and stays on entries after the
    client disconnects;
  - `setPort` and `validateMcpPort` return identical refusals for every out-of-range value, and
    `setPort` refuses `ServerRunning` before the range;
  - every reason string the service can produce matches the reason-string style (a table-driven
    check over each refusal and failure);
  - no activity summary, event or diagnostic contains the token (a scan with a known token);
  - `clientConfigText` is pinned for all four kinds and both disclosures;
  - shutdown reaches `quiescent()`.
- **Live session** (`bloom.scripting.live-session-host`, extends the existing live-session tests):
  - FIFO, one commit per drain;
  - the queue is bounded at 16;
  - a commit waits behind an open gesture and is withdrawn at its deadline;
  - cancellation withdraws a queued commit and never a started one;
  - revocation withdraws with `ProjectReplaced`;
  - leases survive replacement for reads and refuse commits.
- **Live undo** (`bloom.ui.mcp-live-undo`, a Qt test with the real `CompositionSession`, offscreen):
  - an MCP `transact` over HTTP lands as one undo step with its label;
  - the snapshot and Properties show it;
  - UI Undo reverts it, and `events` reports both;
  - an MCP commit during a simulated drag applies after the drag ends;
  - New/Open while a commit is queued fails it with `project-replaced`.
- **Panel** (`bloom.ui.mcp-server-panel`):
  - it binds to a real service on a free test port;
  - start and stop update the status line;
  - a taken port shows the inline reason naming the port;
  - the masked token is shown and never the full one in any widget text until Reveal;
  - the activity list appends incrementally;
  - two panels stay consistent;
  - closing a panel leaves the server running.
- **stdio**: `bloom.scripting.mcp-pipes` passes unchanged, and gains `context`, `history`, `capture`
  and `export.status` cases.
- **Capture**: inline below the limit and linked above it; the Viewer's display and view are used;
  the result equals the still export of the same frame byte for byte.

## Consequences

- An agent works on the artist's open project. Its edits are undoable, visible at once and guarded
  by revisions, and it can see the result through `capture`.
- One server core serves stdio and HTTP. `apps/bloom-mcp` shrinks to a composition root, and the
  Script editor and MCP share one live-session marshal.
- MCP render, export and capture share the export queue's runner, verification and Viewer
  priority. During playback they wait for the Viewer.
- A new module, `src/mcp`, and two platform services are added. The in-tree HTTP server is
  security-sensitive code that Bloom owns: it is kept small, pure where possible and tested
  against hostile input.
- Agents must be configured with a token. Copy client config makes that one paste.

## Alternatives rejected

- **A second, forked server for the desktop**: two implementations of every tool would drift.
- **Driving the live app through the stdio server** (for example, the app spawning `bloom-mcp`): a
  separate process cannot reach the live undo history without a second IPC protocol.
- **No token, relying on loopback and Origin**: any local process could edit the open project and
  write files.
- **An optional token, off by default**: the convenient setting would be the unsafe one.
- **MCP OAuth**: it needs an authorization server that a local single-user application does not
  have.
- **Binding `0.0.0.0` or `::`**: it exposes the project to the network.
- **Falling back to another port when 6767 is taken**: clients configured for the port would
  silently fail or reach another program.
- **A standalone `GET` event stream**: Bloom has no server-initiated messages, and the stream would
  hold connections open for nothing.
- **A third-party HTTP library or Qt HTTP Server**: see section 3.
- **Running requests on the UI thread, or polling it**: it violates the threading rules. The
  marshal applies one bounded commit per slice and wakes on demand.
- **Restarting the server on a port change**: it drops clients silently.
- **A Viewer-pixel readback for capture**: it would be a new readback route on the preview path
  with no export verification, and it shows preview resolution.
- **A resampled capture**: a new pixel operation with its own GPU coverage, for convenience only.
- **Project open, close and save tools**: those flows ask the artist questions.
- **The panel owning the server**: panels are replaceable and may be closed or duplicated.

## Owner decisions

The owner accepted every default below on 2026-10-30.

1. **Off until started.** The server does not run unless the artist starts it.
2. **Auto-start is an opt-in preference, off by default** (Start when Bloom opens).
3. **Port 6767, remembered.** The port is a preference in 1024–65535, edited in the panel and
   changeable only while the server is stopped. A taken port is refused with its number; there is
   no fallback.
4. **The bearer token is always required, with no off switch.** It is stored owner-only in the
   user-data folder, regenerated on demand, and handed to clients through Copy client config.
5. **Loopback `127.0.0.1` only, with no browser origins trusted.** No LAN access.
6. **At most 8 clients**, with idle eviction after 5 minutes at the limit and expiry after 60
   minutes idle.
7. **Client config kinds: Claude Code, MCP JSON, VS Code and Other**, each also available with a
   `${BLOOM_MCP_TOKEN}` placeholder.
8. **Capture** renders final pixels at composition resolution through the export still route. It
   is embedded up to about 2.9 MiB of PNG and linked above that, with no downscaling.
9. **Project lifecycle and window tools.** Reversed on 2026-10-30. The owner: "mcp should have
   tools users can access ... mcp should be treated like our UI for agents." MCP now offers
   `project.save`, `project.save-as`, `project.open`, `project.new` and `project.close`, which never
   discard unsaved work silently (`bloom.mcp.unsaved-changes` unless the call passes
   `discardChanges:true`, `bloom.mcp.jobs-running` unless it passes `stopJobs:true`), and the
   live-only window tools `editor.reveal`, `selection.set`, `composition.activate`, `transport` and
   `viewer.set`, and the window applies `project.set-color-settings` (`setColorSettings`). The
   desktop binds them through `McpServerServiceDependencies::liveUi`
   (`mcp::McpLiveUiPort`, `bloom/mcp/live_ui_port.hpp`), called on the UI thread; `bloom-mcp`
   implements the project tools on its headless session and answers `bloom.mcp.not-live` for the
   window tools. The original decision was "No project open, close or save tools."
10. **The panel is the only editing surface** for these preferences; the Preferences window links
    to it.
11. **Stopping the server or quitting stops MCP-started transient jobs** but never queue jobs
    started with `export.start`. Quitting does not prompt for MCP work.
12. **Render, export and capture on macOS and Windows** answered `PlatformNotSupported`, exactly as
    the Export panel did (ADR 0033 section 15); superseded by ADR 0035, see the amendment note
    below. Every other MCP capability works on all three platforms.

## Owning documents the implementation updates

- `user-guide/mcp.md`: the in-app server, the panel, client configuration, the token, the new
  tools and both transports.
- `architecture/scripting-and-addons.md`: `LiveSessionHost`, and MCP on the live session.
- `architecture/overview.md`: `src/mcp` in the source boundaries.
- `architecture/workspace-layout.md`: the MCP Server editor.
- `architecture/task-system.md`: the MCP shutdown participant, and the adapter waiter beside the
  completion drain.
- `architecture/platform-support.md`: loopback sockets and owner-only secrets.
- `user-guide/preferences.md`: the MCP preferences and where they are edited.
- `architecture/frame-output.md`: transient jobs from MCP.
