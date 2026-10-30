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
The server never opens, closes, saves or creates projects: those questions are yours.

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
| `query` | `kind`: project, compositions, nodes, parameters, parameter, parameter-value, assets, operations or context; nodes and parameter kinds also need `composition`. The legacy `parameters` query returns full records. `parameters` with `pageSize` (1–64) and optional `cursor` returns bounded summaries and `nextCursor`; constant values are explicitly marked `valueOmitted`. `parameter` with `parameter` ID reads one full record, except a string over 128 KiB returns a summary. `parameter-value` with `parameter` ID and `expectedRevision` reads UTF-8 string chunks using optional byte `offset` and `limit` (4–131072). An operations record carries every argument's `kind`, `required`, `contextual`, `context` and `summary`, plus a pasteable `example` call. `context` returns the editor context contextual arguments fall back to: project, composition, selected nodes and layers, time, the Viewer's display and view, read-only state and `live` (false on stdio, which reports the session defaults). |
| `transact` | `expectedRevision`, an `operations` array of `{op,args}`, and optional `label`. Returns one atomic command result, created IDs and structured diagnostics. Arguments the descriptor marks `contextual` may be omitted and are filled from the session (the live editor context in the desktop application). Coded failures carry `structuredContent.diagnostic` `{code, message}` with a `bloom.mcp.*` code. |
| `render` | `composition`, `frame` or inclusive `first`/`last`, `preset`, `destination`. Returns published frame count, preservation report, paths and SHA-256 file digests. |
| `export` | Render arguments plus optional `profile`, `audio` and `sampleRate`. Uses the existing MEDIA-4 composition runner for ProRes preview, DNxHR and PCM output. |
| `events` | `sinceRevision`, optionally `afterSequence`. Returns revision-inclusive events, a sequence cursor and `resetRequired` when retained history cannot cover the cursor. |
| `history` | `action` (`undo` or `redo`) and `expectedRevision`. One step of the project's history, refused with a stale-revision result when the revision moved. In the desktop application it is queued like a `transact`. |
| `capture` | Optional `composition` and `frame` (defaults: the context composition at the context time; on stdio the session's composition at frame 0). Renders one PNG at composition resolution through the still export route, with the Viewer's display and view in the desktop application, and returns it as MCP `image` content. A PNG larger than about 2.9 MiB is returned as a `resource_link` to the file with the notice `bloom.mcp.capture-too-large-for-inline`. Captures go to an `mcp-captures` folder in Bloom's per-user cache, which keeps the newest 16. |
| `export.start`, `export.start-all`, `export.pause`, `export.resume`, `export.stop`, `export.stop-all`, `export.retry`, `export.confirm-replace` | The export queue's session commands (`jobs`, `job`, `output`). Each returns `accepted`, the queue revision and, when refused, the availability code and reason. |
| `export.status`, `export.events`, `export.log`, `export.presets`, `export.script` | The queue snapshot (optionally `sinceRevision`), terminal events after `afterSequence`, a job's run log, the presets available on this computer, and a script that recreates a job (`format` `cli-json` or `python`). |
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
