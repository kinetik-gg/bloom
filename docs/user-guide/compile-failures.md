# When a composition cannot render

If Bloom cannot build a composition for preview or export, the Viewer keeps its last good frame and
the status bar says so in one sentence, for example "Validated graph could not be lowered". A
**Details** button appears beside it.

Details opens a popup above the status bar with:

- the same headline;
- a plain explanation of what went wrong;
- where it happened, in words: "Layer 'Title' · Blending Mode".

**Select** selects that layer (or node) so Properties and Nodes show it. It is unavailable when the
layer or node no longer exists. **Copy details** copies the full diagnostic, including its code and
internal ids, for a bug report; the codes appear nowhere else.

The button goes away as soon as the composition renders again.

## Edits from an agent

When an agent's edit through the [MCP server](mcp.md) leaves a composition unable to render, the
[MCP Server panel](mcp-server-panel.md) adds a red Activity line:
`<client> · transact · left 'Main' unable to render: <what went wrong>`. Open the composition and
use Details to see where.
