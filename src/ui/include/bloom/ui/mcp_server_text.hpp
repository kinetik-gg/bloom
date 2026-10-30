#pragma once

// The MCP Server panel's wording, built from the service's typed facts (ADR 0034, section 7).
// Pure functions: nothing here reads a widget or the service.

#include <bloom/mcp/server_service.hpp>
#include <bloom/ui/mcp_server_adapter.hpp>

#include <QString>

namespace bloom::ui::mcppanel {

[[nodiscard]] inline QString qs(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// "Stopped", "Running on 127.0.0.1:6767 · 2 clients", "Stopping…" or the failure's reason.
[[nodiscard]] QString statusText(const mcp::McpServerSnapshot& snapshot);
// "HH:mm:ss" in local time; empty for an unset time.
[[nodiscard]] QString timeText(std::chrono::system_clock::time_point at);
// The client's title, else its name.
[[nodiscard]] QString clientName(const mcp::McpClientInfo& info);
[[nodiscard]] QString toolText(const std::optional<mcp::McpTool>& tool);
// One activity line from the entry's typed facts; `summary` only when nothing typed explains it.
[[nodiscard]] QString activityLine(const mcp::McpActivityEntry& entry);
// "<time>  <client> · transact · left '<composition>' unable to render: <detail>".
[[nodiscard]] QString compileFailureLine(const McpCompileFailureNote& note);
[[nodiscard]] QString clientKindText(mcp::McpClientKind kind);

} // namespace bloom::ui::mcppanel
