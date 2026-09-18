# Using Bloom from an agent (MCP)

Start `bloom-mcp`, optionally with `--project project.bloom`. The server is Qt-free and uses
JSON-RPC 2.0 over newline-delimited UTF-8 stdin/stdout. It implements the MCP 2025-11-25 handshake
and tool protocol; it has no HTTP listener or Python-evaluation tool. Logs go to stderr.

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

## Connect and discover

Send `initialize` with `protocolVersion`, `capabilities` and `clientInfo`, then send the
`notifications/initialized` notification. `initialize` returns Bloom's version, facade version,
headless status, tool support and request limit. `tools/list` returns complete tool input schemas.
Use `tools/call` with `name` and `arguments`:

| Tool | Arguments and result |
| --- | --- |
| `query` | `kind`: project, compositions, nodes, parameters, assets or operations; nodes/parameters also need `composition`. Returns a revision and read projection, including constant parameter values, curve/driver references, or registry descriptors. An operations record carries every argument's `kind`, `required`, `contextual`, `context` and `summary`, plus a pasteable `example` call. |
| `transact` | `expectedRevision`, an `operations` array of `{op,args}`, and optional `label`. Returns one atomic command result, created IDs and structured diagnostics. Arguments the descriptor marks `contextual` may be omitted and are filled from the session. |
| `render` | `composition`, `frame` or inclusive `first`/`last`, `preset`, `destination`. Returns published frame count, preservation report, paths and SHA-256 file digests. |
| `export` | Render arguments plus optional `profile`, `audio` and `sampleRate`. Uses the existing MEDIA-4 composition runner for ProRes preview, DNxHR and PCM output. |
| `events` | `sinceRevision`, optionally `afterSequence`. Returns revision-inclusive events, a sequence cursor and `resetRequired` when retained history cannot cover the cursor. |

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

## Output and cancellation

Still presets are `PngRgba8SrgbV1`, `FlatExrRgba32fLinRec709SceneV1` and `TiffRgba16SrgbV1`. MEDIA-4
presets are `ProResMovV1`, `DnxhrMxfV1` and `PcmWavV1`. ProRes output retains its existing
non-authorized preview status. Worker exports currently have a Linux implementation; macOS and
Windows return the host provider's unavailable diagnostic. See [Exporting video](exporting-video.md)
for profiles and preservation limits. The destination directory must exist.

Send `notifications/cancelled` with `params.requestId` matching the active tool request to cancel
its native tasks. The bounded input reader continues receiving notifications during output work.
Completed image-sequence frames remain published; media publication retains the runner's staged
verification and cancellation behavior. Export uses the requested preset and approves the resulting
host analysis digest; the returned report preserves implementation notes.

Each request is at most 1 MiB, each response at most 4 MiB, arrays at most 4096 items, and JSON depth
at most 32. Parsing and writing use fixed 32 MiB allocation arenas. The pending request queue holds
at most 16 messages. Duplicate decoded keys, malformed Unicode, non-finite JSON numbers, invalid ID
types and unknown schema members are rejected. A render request is limited to 10,000 frames; event
history retains 1024 records. Clients should advance `afterSequence` and re-query the snapshot when
`resetRequired` is true. This local process uses its launcher's filesystem permissions.

Protocol references: [stdio transport](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
and [tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools).
