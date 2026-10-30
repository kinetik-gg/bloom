# Using Bloom from an agent (MCP)

Bloom offers the Model Context Protocol (MCP 2025-11-25) in two ways. Both serve the same tools
with the same schemas from one server core (ADR 0034):

- **The in-app server** runs inside the desktop application and works on the project you have
  open. An agent's edits land in your undo history, show in every panel at once, and can be
  undone with Edit ▸ Undo. Start it from the **MCP Server** editor.
- **`bloom-mcp`** is a separate process on stdin/stdout with its own headless session, for
  scripts, CI and agents that should not touch an open project.

## The in-app server

Open an **MCP Server** editor (any editor area's editor menu) and press **Start**. The server is
off until you start it. It listens on `http://127.0.0.1:6767/mcp`, on this computer only: it binds
the loopback address `127.0.0.1` and nothing else, so other computers cannot reach it, and it
trusts no web page. Change the **Port** (1024–65535) while the server is stopped. If another
program already uses the port, Start says so ("Port 6767 is in use. Choose another port or stop
the other program."); Bloom never moves to another port by itself, because a client configured
for one port must never reach a server that silently moved. **Start when Bloom opens** starts the
server at launch. The port and this choice are preferences, kept outside your projects; nothing
the MCP Server editor does changes a project or its undo history.

### Connecting a client

Every request needs the access token, sent as `Authorization: Bearer <token>`. The token is
created on first use and kept in an owner-only file in Bloom's user-data folder (readable only by
you; a file other users could read is replaced). **Copy client config** puts ready-to-paste text
on the clipboard for:

- **Claude Code**: `claude mcp add --transport http bloom http://127.0.0.1:6767/mcp --header "Authorization: Bearer <token>"`
- **MCP JSON** (a project's `.mcp.json` and clients that read it):
  `{"mcpServers":{"bloom":{"type":"http","url":"http://127.0.0.1:6767/mcp","headers":{"Authorization":"Bearer <token>"}}}}`
- **VS Code** (`.vscode/mcp.json`):
  `{"servers":{"bloom":{"type":"http","url":"http://127.0.0.1:6767/mcp","headers":{"Authorization":"Bearer <token>"}}}}`
- **Other client**: the address and the header as two lines.

Each kind can also be copied with `${BLOOM_MCP_TOKEN}` in place of the token, for configuration
that is shared or committed; set that environment variable in the client's environment. The
editor shows the token masked; **Reveal** and **Copy** show or copy it, and **Regenerate…**
replaces it and disconnects every client (they must use the new token). The token never appears
in the activity list or in diagnostics.

### What the editor shows

The status line (Stopped, Running on 127.0.0.1:6767 with the number of clients, Stopping, or the
reason the server could not start), the connected clients (name and version from the client,
connected since, last activity, requests in flight, the last tool), and the activity list: every
tool call with its client, outcome, error and duration, and every refused request (a wrong token,
a browser origin, too many clients). Identical refusals within a second are counted on one line.
Up to 512 lines are kept; "Earlier activity was trimmed." marks a gap.

### Limits

Up to 8 clients at once. When a ninth connects, the least recently used client that has been idle
for 5 minutes with nothing in flight is disconnected; otherwise the new client is refused. A client
idle for 60 minutes is ended and reconnects on its next request. At most 16 connections, 4 requests
in progress per client, 4 executing at once and 16 waiting. Every request is at most 1 MiB and every
response at most 4 MiB, as on stdio.

### Edits, undo and project changes

A `transact` call is one atomic transaction and one undo step, labelled with its `label` or
"Agent edit". It applies on Bloom's main thread between your own edits: never while you are in the
middle of a drag, a scrub or a text edit (it waits until you finish, up to 30 seconds), and a
revision conflict leaves the project unchanged. A queued edit that is cancelled, times out or
outlives its project is never applied, and the result names why with a stable code:
`bloom.mcp.cancelled`, `bloom.mcp.deadline-exceeded`, `bloom.mcp.gesture-open`,
`bloom.mcp.project-replaced`, `bloom.mcp.commit-queue-full`. The Script editor and MCP share one
queue of 16 edits.

Clients stay connected when you open or create a project. While a project is opening, calls answer
`bloom.mcp.project-unavailable`; an `events` cursor from the old project reports `resetRequired`.

### Projects and the application window

An agent can save, open, create and close projects as you can from the File menu, and can act on
the window the way you do: what is selected, which composition is active, the playhead and the
Viewer. None of these is an undo step. The project tools never throw work away silently: Open,
New and Close over unsaved changes answer `bloom.mcp.unsaved-changes` (naming the project) unless
the call passes `discardChanges:true`, and while export jobs run they answer
`bloom.mcp.jobs-running` unless it passes `stopJobs:true`.

| Tool | Arguments | Notes |
| --- | --- | --- |
| `project.save` | none | `bloom.mcp.no-path` when the project was never saved; `bloom.mcp.read-only` for a read-only project. |
| `project.save-as` | `path` (absolute, `.bloom`), optional `replace` | `bloom.mcp.destination-exists` unless `replace` is true; `bloom.mcp.path-invalid` for a relative path or a missing folder. |
| `project.open` | `path`, optional `discardChanges`, `stopJobs` | Replaces the open project. |
| `project.new` | optional `discardChanges`, `stopJobs` | An untitled project. |
| `project.close` | optional `discardChanges`, `stopJobs` | The window shows an empty project. |
| `editor.reveal` | `composition`, optional `time` | Shows the composition in the Viewer and moves the playhead. |
| `selection.set` | `composition`, optional `layers`, `nodes`, `mode` (`set`, `add`, `remove`, `clear`) | Contextual `layer`, `layers` and `selection` arguments then default to it. |
| `composition.activate` | `composition` | The Viewer, Timeline and contextual `composition` follow it. |
| `transport` | `action` (`seek`, `play`, `stop`, `step-forward`, `step-backward`, `go-to-start`, `go-to-end`), `time` for `seek`, optional `composition` | `time` counts in the given composition, or the active one. |
| `viewer.set` | any of `display`, `view`, `exposure` (stops), `channel` (`rgba`, `rgb`, `red`, `green`, `blue`, `alpha`), `zoom` (`"fit"`, `"actual-size"` or a factor, 1.0 is 100%) | Display settings only: they never reach a render or the project. |

A successful project tool answers `{succeeded, event, path, projectName}` and records the event
(`project.saved`, `project.opened`, `project.created` or `project.closed`) for the `events` tool and
the MCP Server editor's activity list. The window tools work only with the in-app server:
`bloom-mcp` implements the five project tools on its own session and answers `bloom.mcp.not-live`
for the others. A window tool the application has not connected answers `bloom.mcp.ui-unbound`.
Other refusals are `bloom.mcp.unknown-target`, `bloom.mcp.invalid-argument`, `bloom.mcp.busy` and
`bloom.mcp.failed`, each with a message that says what to do.

`render`, `export` and `capture` run as transient jobs of the export queue, with the same checks,
verification and Viewer priority as the Export editor; they are never saved. Nothing replaces an
existing file silently: pass `"overwrite":"replace"` or confirm with `export.confirm-replace`.
Stopping the server or quitting stops the jobs an agent started this way, never jobs started with
`export.start`, which belong to the Export editor. On macOS and Windows render, export, capture and
`export.start*` answer `bloom.mcp.export-unavailable` with `PlatformNotSupported`, as the Export
editor does; every other tool works on all three systems.

### Streamable HTTP

The endpoint is one path, `/mcp`. `POST` carries one JSON-RPC message (batches are refused with 400)
and needs `Content-Type: application/json` and an `Accept` that allows both `application/json` and
`text/event-stream`. `initialize` returns an `Mcp-Session-Id` that every later request carries;
`DELETE /mcp` ends the session; an ended or expired session answers 404 and the client initializes
again. A task tool (`render`, `export`, `capture`, `export.wait`) that carries
`_meta.progressToken` is answered as `text/event-stream`: `notifications/progress` events, then the
result. `GET` is 405: Bloom sends no server-initiated messages. Requests from a browser `Origin`, a
`Host` other than `127.0.0.1:<port>` or `localhost:<port>`, or without the token are refused (403
or 401) before anything else is read. Disconnecting does not cancel a request; send
`notifications/cancelled`.

## `bloom-mcp` on stdio

Start `bloom-mcp`, optionally with `--project project.bloom`. The server is Qt-free and uses
JSON-RPC 2.0 over newline-delimited UTF-8 stdin/stdout. It implements the MCP 2025-11-25 handshake
and tool protocol. Logs go to stderr.

A client configuration can launch it as:

```json
{
  "mcpServers": {
    "bloom": {
      "command": "bloom-mcp",
      "args": ["--project", "project.bloom"]
    }
  }
}
```

The client's working directory resolves relative project and output paths. The server opens one
process-local session at startup. Transactions change that session's command history; they do not
automatically save the `.bloom` document. Render/export publication writes the requested outputs.

The running application can also host the server itself; start it and manage its port and token
from the [MCP Server panel](mcp-server-panel.md).

## Connect and discover

Send `initialize` with `protocolVersion`, `capabilities` and `clientInfo`, then send the
`notifications/initialized` notification. `initialize` returns Bloom's version, facade version,
headless status, tool support and request limit. `tools/list` returns complete tool input schemas.
Use `tools/call` with `name` and `arguments`:

| Tool | Arguments and result |
| --- | --- |
| `query` | `kind`: project, compositions, nodes, parameters, parameter, parameter-value, assets, operations or context; nodes and parameter kinds also need `composition`. The legacy `parameters` query returns full records. `parameters` with `pageSize` (1–64) and optional `cursor` returns bounded summaries and `nextCursor`; constant values are explicitly marked `valueOmitted`. `parameter` with `parameter` ID reads one full record, except a string over 128 KiB returns a summary. `parameter-value` with `parameter` ID and `expectedRevision` reads UTF-8 string chunks using optional byte `offset` and `limit` (4–131072). An operations record carries a `description` (purpose, side effects, outputs, errors with remedies and the undo step; empty for an operation not yet described), every argument's `kind`, `required`, `contextual`, `context` and `summary`, plus a pasteable `example` call. `context` returns the editor context contextual arguments fall back to: project, composition, selected nodes and layers, time, the Viewer's display and view, read-only state and `live` (false on stdio, which reports the session defaults). |
| `transact` | `expectedRevision`, an `operations` array of `{op,args}`, and optional `label`. Returns one atomic command result, created IDs, structured diagnostics and `compileFailures` (see [Edits and diagnostics](#edits-and-diagnostics)). Arguments the descriptor marks `contextual` may be omitted and are filled from the session (the live editor context in the desktop application). Coded failures carry `structuredContent.diagnostic` `{code, message}` with a `bloom.mcp.*` code. |
| `render` | `composition`, `frame` or inclusive `first`/`last`, `preset`, `destination`. Returns published frame count, preservation report, paths and SHA-256 file digests. |
| `export` | Render arguments plus optional `profile`, `audio` and `sampleRate`. Uses the existing MEDIA-4 composition runner for ProRes preview, DNxHR and PCM output. |
| `events` | `sinceRevision`, optionally `afterSequence`. Returns revision-inclusive events, a sequence cursor and `resetRequired` when retained history cannot cover the cursor. |
| `history` | `action` (`undo` or `redo`) and `expectedRevision`. One step of the project's history, refused with a stale-revision result when the revision moved. In the desktop application it is queued like a `transact`. |
| `capture` | Optional `composition` and `frame` (defaults: the context composition at the context time; on stdio the session's composition at frame 0). Renders one PNG at composition resolution through the still export route, with the Viewer's display and view in the desktop application, and returns it as MCP `image` content. A PNG larger than about 2.9 MiB is returned as a `resource_link` to the file with the notice `bloom.mcp.capture-too-large-for-inline`. Captures go to an `mcp-captures` folder in Bloom's per-user cache, which keeps the newest 16. |
| `export.start`, `export.start-all`, `export.pause`, `export.resume`, `export.stop`, `export.stop-all`, `export.retry`, `export.confirm-replace` | The export queue's session commands (`jobs`, `job`, `output`). Each returns `accepted`, the queue revision and, when refused, the availability code and reason. |
| `export.status`, `export.events`, `export.log`, `export.presets`, `export.script` | The queue snapshot (optionally `sinceRevision`), terminal events after `afterSequence`, a job's run log, the presets available on this computer, and a script that recreates a job (`format` `cli-json` or `python`). |
| `project.set-color-settings` | `config` (`neutral` Bloom Neutral v1, `aces` ACES 1.3 CG, or `current` to keep the project's own, the only choice for an external configuration) and optional `workingSpace` (neutral: `lin_rec709_scene`; aces: `ACEScg`, `ACES2065-1`, `Linear Rec.709 (sRGB)`, `Linear P3-D65`, `Linear Rec.2020`; omitted keeps the project's when the configuration stays, else the first). Project Settings > Colour as the dialog applies it. Not undoable and not in the history: it marks the project dirty and changes how every composition renders. Returns `code` `updated` or `no-change` with the resulting `config` and `workingSpace`; `invalid-config` and `invalid-settings` (a working space the configuration does not offer) return `succeeded: false`; a read-only project answers `bloom.mcp.read-only`. In the desktop application the window applies it (`bloom.mcp.ui-unbound` if it does not offer it). |
| `export.wait` | Optional `job` and `timeoutMs`: waits until the job (or the whole queue) is finished, with progress when the request carries a progress token. |

For example:

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"query","arguments":{"kind":"operations"}}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"transact","arguments":{"expectedRevision":0,"label":"Name study","operations":[{"op":"bloom.project.set-name","args":{"name":"Study"}}]}}}
```

Operation IDs and argument schemas come directly from the facade registry. Listing an ID does not
promise that this build implements its adapter. Eleven factories are available, including parameter animation, component keyframes and node removal;
other registered IDs return their structured unsupported diagnostic. A revision conflict leaves
the document unchanged. Tool results include both `structuredContent` and a JSON text content item;
`isError` distinguishes failed operations from successful tools. Invalid JSON-RPC or tool arguments
produce protocol errors with the request ID where valid.

`bloom.layer.align-layers` and `bloom.layer.distribute-layers` measure the layers' evaluated bounds
on the server before the transaction runs (the CPU reference evaluation of one frame), so a
`transact` call with them can take longer than other edits. Their `layers` argument is contextual
(`context` `"layers"`: the selected layers) and `time` defaults to the session time. Skipped layers
arrive as `bloom.scripting.operation-warning` diagnostics; when no layer can move, the transaction
fails with the reasons. See [Scripting Bloom with Python](python.md#align-and-distribute) for the
arguments.

## Edits and diagnostics

**Times.** Every time argument follows one contract, in `transact`, `editor.reveal`, `transport`,
JSON scripts and Python alike: an integer is a whole frame of the composition the call works on, at
its frame rate, and `[numerator, denominator]` is an exact time in seconds. At 24 fps,
`{"duration": 48}` is two seconds and `{"time": [1, 2]}` is frame 12. When a transaction changes a
composition's frame rate, later frame counts in it use the new rate.

**Parameter values.** `bloom.parameter.set-source` takes `value` in the parameter's own kind: a
number, `true`/`false` for a Boolean, text for a String, `[x, y]`, `[x, y, z]` or `[r, g, b, a]`,
and for an Integer or enumeration a whole number or the member's name (`"screen"`, `"Ping-pong"`;
case, spaces and hyphens do not matter). Reading an enumeration parameter back
(`query` `kind` `parameter`) returns both its stored `value` and its `label`. A value out of range is refused with what the parameter
accepts, for example "Parameter 31 (points of node 7, bloom.shape) cannot take 2; it takes a whole
number in 3..64." A Boolean parameter such as a shape's `fillEnabled` or `strokeEnabled` can be
driven by a Boolean value node: `{"driverNode": <id>, "driverPort": "result"}` with
`bloom.scalar-to-boolean`, for example. A driver naming an output the node does not have, or one of
the wrong kind, is refused with the node's outputs. `bloom.animation.set-keyframe-at-time-for-parameter`
on a constant parameter makes it animated in the same undo step, its constant becoming the first
key.

**Diagnostics.** A refused transaction applies nothing and lists why. Each diagnostic is
`{code, operationIndex, operation?, path?, message}`: `operationIndex` is the 0-based place in
`operations` of the operation that caused it (`null` for a stale revision or a history limit,
which belong to the whole transaction). Codes are `bloom.scripting.command-rejected` (an operation
refused), `bloom.scripting.validation-failed` (the edit would leave the project invalid; `path`
names where), `bloom.scripting.stale-revision`, `bloom.scripting.history-limit` and
`bloom.scripting.invalid-argument` (the registry refused an argument). An argument of the wrong
JSON type is a protocol error whose message starts with `operations[<index>] (<op>)` and gives a
working example.

**Compile failures.** After a successful edit, every composition it touched is compiled (never
rendered). `compileFailures` lists those that cannot render now:
`[{composition, code, detail, subject: {composition, node, parameter}}]`, where `detail` is one
plain sentence and `node`/`parameter` are `null` when the failure has no narrower subject. Each one
is also recorded as a `composition.compile-failed` event. An empty list means every touched
composition compiles.

**Events.** Besides command events (`kind` `revision`, `history` or `rejected`), `events` reports
`{kind, revision, message, composition?, subject?, path?, projectName?, sequence}` records for
`composition.compile-failed`, `project.saved`, `project.opened`, `project.created` and
`project.closed`.

For a large parameter set, start with `{"kind":"parameters","composition":1,"pageSize":64}`.
Repeat with its `nextCursor` until it is `null`. Each page includes `revision` and `totalRecords`;
the cursor binds the server session, project, composition, revision and next record index. A
malformed cursor or any intervening edit is an error, so restart at the first page after an edit.
Summaries identify omitted constants with `valueOmitted`; string summaries also report
`valueBytes`. Read one parameter with `{"kind":"parameter","composition":1,"parameter":42,
"expectedRevision":3}`. For an oversized constant string, use `kind:"parameter-value"` with
the same IDs and revision, starting at `offset:0` and advancing to each `nextOffset` until
`complete:true`. Chunks report `totalBytes` and always end on a UTF-8 boundary; no value is
silently shortened. An `expectedRevision` mismatch is an error.

## Output and cancellation

`render` and `export` write to `destination`: a file path with the preset's extension (`.png`,
`.exr`, `.tiff` or `.tif`, `.mov`, `.mxf`, `.wav`), a path without an extension (the preset's is
added), or an existing folder (the file is then `frame.<ext>`, or `render.<ext>` for media). A
range writes `<base>.NNNN.<ext>`; a still destination may instead carry one `####` or `{frame}`
where the frame number goes (`seq.####.png` writes `seq.0000.png`, `seq.0001.png`, ...). A wrong
extension is an argument error saying which one the preset needs. A request the export checks
refuse (an existing file without `overwrite:"replace"`, for example) answers
`bloom.mcp.preflight-blocked` with the reason; `bloom.mcp.export-unavailable` means export itself
cannot run here, and "Export isn't available in this window yet." means the window has no export
queue.

`render`, `export` and `capture` run as transient export jobs in the desktop application. Their
results, on success and on failure, carry `job` (the transient job id) and `log` (the run's newest
64 log entries, each `{sequence, sinceStartMs, severity, code, frame, message}` as `export.log`
gives them), so the detail a failure points to is in the answer itself. While such a job runs,
`export.status` lists it with `transient: true` and `export.log` reads it by its id; the job is
retired when the call returns.

Still presets are `PngRgba8SrgbV1`, `FlatExrRgba32fLinRec709SceneV1` and `TiffRgba16SrgbV1`. MEDIA-4
presets are `ProResMovV1`, `DnxhrMxfV1` and `PcmWavV1`. ProRes output retains its existing
non-authorized preview status. Worker exports currently have a Linux implementation; macOS and
Windows return the host provider's unavailable diagnostic. See [Exporting video](exporting-video.md)
for profiles and preservation limits. The destination directory must exist.

Send `notifications/cancelled` with `params.requestId` matching the active tool request to cancel
that request's own work; other work keeps running. The bounded input reader continues receiving notifications during output work.
Completed image-sequence frames remain published; media publication retains the runner's staged
verification and cancellation behavior. Export uses the requested preset and approves the resulting
host analysis digest; the returned report preserves implementation notes.

On both transports each request is at most 1 MiB, each response at most 4 MiB, at most 65,536 JSON
values, and JSON depth at most 32. Arrays have no separate item cap. Parsing and writing use fixed 32 MiB allocation arenas. The pending request queue holds
at most 16 messages. Duplicate decoded keys, malformed Unicode, non-finite JSON numbers, invalid ID
types and unknown schema members are rejected. A render request is limited to 10,000 frames; event
history retains 1024 records. Clients should advance `afterSequence` and re-query the snapshot when
`resetRequired` is true. This local process uses its launcher's filesystem permissions.

Protocol references: [transports (stdio and Streamable HTTP)](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
and [tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools).
