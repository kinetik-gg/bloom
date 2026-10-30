# MCP Server panel

The MCP Server panel starts and stops Bloom's built-in MCP server, so an agent or tool on your
machine can read and edit the open project. Open it by choosing **MCP Server** in any panel's editor
picker, or click the MCP indicator in the status bar. The server is off until you start it. Nothing the panel does changes
your project or enters the undo history.

## Start and stop

The Start button becomes Stop while the server runs. When a control can't be used, it stays visible
and says why: the reason is shown under it and in its tooltip.

The status line reads one of:

- Stopped.
- Running on 127.0.0.1:6767 and the number of connected clients.
- Stopping…
- Couldn't start, followed by the reason, for example when another program already uses the port.

The server only listens on 127.0.0.1, never on the network, and accepts up to 8 clients. Every
request needs the access token.

## Port and auto-start

The port defaults to 6767. It can be changed only while the server is stopped; while it runs the
field is locked and says "Stop the server to change the port." A port from 1024 to 65535 is
accepted. Bloom never picks another port on its own, so a client configured for the port you chose
never reaches a different server.

Start when Bloom opens is off by default. When it is on and the port is taken, Bloom opens normally
and the panel shows the reason. Port and auto-start are edited only in this panel; Preferences links
to it.

## Access token

The token is shown masked. Reveal shows it until the panel loses focus or 30 seconds pass. Copy puts
it on the clipboard without showing it. Regenerate… asks first, because every connected client is
disconnected and needs the new token. The token never appears in the activity list or in logs.

## Copy client config

Copy client config offers Claude Code, MCP JSON, VS Code and Other. Each has a variant that uses a
`${BLOOM_MCP_TOKEN}` placeholder instead of the token, for configuration you commit or share. The
text uses the current port, so you can copy it while the server is stopped.

## Connected clients and activity

Connected clients lists each client's name, version, when it connected, its last activity, how many
requests it has in flight and the last tool it used. Activity lists, newest last, the time, the
client, the tool and its outcome. Repeated identical refusals fold into one line with a count. When
old lines were dropped, the list starts with "Earlier activity was trimmed."

Project calls read as plain lines, for example "claude-code saved 'SC020'", "opened 'SC020'",
"created 'Untitled'" and "closed 'SC020'". When an agent's edit leaves a composition unable to
render, the list adds a red line naming the client, the tool, the composition and what is wrong.

While no project is open, the panel says so under Connected clients.

See [Using Bloom from an agent](mcp.md) for the tools themselves.
