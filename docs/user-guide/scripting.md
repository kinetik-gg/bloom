# Headless scripting

`bloom-cli` is Bloom's Qt-free v1 host client. It uses the same typed operations, project-session
ownership, render compiler, output analysis, approval, and publication chain as the desktop UI.

```text
bloom-cli new <file>
bloom-cli open <file>
bloom-cli run <script.json>
bloom-cli render [--project <file>] (--frame N | --range A-B) \
  --preset PngRgba8SrgbV1|FlatExrRgba32fLinRec709SceneV1 --out <dir>
```

`run` accepts a JSON array of `{ "op": "<stable-id>", "args": { ... } }` transactions. Each
transaction is validated against the operation registry and receives an expected current revision.
Arguments the registry marks contextual -- `composition`, `time`, `layer`, `selection` -- may be
omitted; the session fills them exactly as the Python and MCP clients do, so a headless step that
names no composition edits the project's first one.
Malformed arguments, unknown IDs, stale revisions, and command rejection produce a typed diagnostic
and a non-zero exit status. The run starts from a new in-memory session; use `new` or the facade's
Save As operation when a durable project is required.

`render` selects the first composition when no project is supplied. A frame writes `frame.png` or
`frame.exr`; a range writes zero-padded `frame.XXXX` sequence files. The command prints the output
preservation report and only exits successfully after publication completes.

Example scripts live in [`examples/scripting`](../../examples/scripting/).

Python scripts and the interactive interpreter are described in [Scripting Bloom with Python](python.md).
For stdio clients, see [Using Bloom from an agent (MCP)](mcp.md).
