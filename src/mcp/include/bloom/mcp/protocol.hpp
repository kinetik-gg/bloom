#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

// MCP protocol vocabulary shared by both Bloom MCP hosts (ADR 0034): the stdio server (bloom-mcp)
// and the in-app server (Streamable HTTP inside the desktop application). Qt-free; owned by
// src/mcp. Values here are the existing bloom-mcp limits, unchanged, so a client sees the same
// bounds on both transports.
namespace bloom::mcp {

// The one MCP revision Bloom implements, for both transports.
inline constexpr std::string_view kMcpProtocolVersion = "2025-11-25";

// ---------------------------------------------------------------------------------------------
// Message limits (unchanged from bloom-mcp; docs/user-guide/mcp.md)

struct McpMessageLimits final {
    // One JSON-RPC message (stdio line or HTTP request body).
    std::size_t requestBytes = std::size_t{1024} * 1024U;
    // One JSON-RPC response (JSON body or one SSE `data:` payload).
    std::size_t responseBytes = std::size_t{4} * 1024U * 1024U;
    std::size_t jsonValues = 65536;
    std::size_t jsonDepth = 32;
    // Each parse and write uses a fixed allocation arena of this size.
    std::size_t arenaBytes = std::size_t{32} * 1024U * 1024U;
    // Messages accepted but not yet executing, across every client of one server.
    std::size_t pendingMessages = 16;
    // Frames one render or export call may request.
    std::uint32_t renderFrames = 10000;
    // Command events retained for the `events` tool.
    std::size_t retainedCommandEvents = 1024;
};

inline constexpr McpMessageLimits kMcpMessageLimits{};

// ---------------------------------------------------------------------------------------------
// Tools

// Every tool both hosts list. The stdio host and the in-app host offer the same set with the same
// schemas; a host that cannot honour a call answers with a typed tool error, never by hiding the
// tool. Render, export and capture run through the export queue's one runner (ADR 0033 sections
// 6 and 13); the export.* tools are the queue's session calls.
enum class McpTool : std::uint8_t {
    Query = 0,
    Transact = 1,
    History = 2,
    Events = 3,
    Render = 4,
    Export = 5,
    Capture = 6,
    ExportStart = 7,
    ExportStartAll = 8,
    ExportPause = 9,
    ExportResume = 10,
    ExportStop = 11,
    ExportStopAll = 12,
    ExportRetry = 13,
    ExportConfirmReplace = 14,
    ExportStatus = 15,
    ExportEvents = 16,
    ExportLog = 17,
    ExportWait = 18,
    ExportPresets = 19,
    ExportScript = 20,
    // Project lifecycle (ADR 0034, owner decision item 9 as amended 2026-10-30): the File menu's
    // Save, Save As, Open, New and Close. Never part of the undo history.
    ProjectSave = 21,
    ProjectSaveAs = 22,
    ProjectOpen = 23,
    ProjectNew = 24,
    ProjectClose = 25,
    // Live-only session and UI state (live_ui_port.hpp); never dirties the project.
    EditorReveal = 26,
    SelectionSet = 27,
    CompositionActivate = 28,
    Transport = 29,
    ViewerSet = 30,
    // Project Settings > Colour (not undoable; marks the project dirty).
    ProjectSetColorSettings = 31,
};

inline constexpr std::size_t kMcpToolCount = 32;

// The wire name. MCP tool names allow [A-Za-z0-9_.-]; the export.* names are ADR 0033's session
// call names verbatim.
[[nodiscard]] constexpr std::string_view toolName(const McpTool tool) {
    // clang-format off
    constexpr std::array<std::string_view, kMcpToolCount> names{
        "query",
        "transact",
        "history",
        "events",
        "render",
        "export",
        "capture",
        "export.start",
        "export.start-all",
        "export.pause",
        "export.resume",
        "export.stop",
        "export.stop-all",
        "export.retry",
        "export.confirm-replace",
        "export.status",
        "export.events",
        "export.log",
        "export.wait",
        "export.presets",
        "export.script",
        "project.save",
        "project.save-as",
        "project.open",
        "project.new",
        "project.close",
        "editor.reveal",
        "selection.set",
        "composition.activate",
        "transport",
        "viewer.set",
        "project.set-color-settings",
    };
    // clang-format on
    return names[static_cast<std::size_t>(tool)];
}

// How a tool's work runs. Inline tools finish on the request worker under bounded cost (paged
// queries, one queued commit, a cursor read). Task tools submit scheduler work (export runner
// jobs) and report progress when the request carried a progress token; their duration is
// unbounded but cancellable.
enum class McpToolExecution : std::uint8_t { Inline = 0, Task = 1 };

[[nodiscard]] constexpr McpToolExecution toolExecution(const McpTool tool) {
    switch (tool) {
    case McpTool::Render:
    case McpTool::Export:
    case McpTool::Capture:
    case McpTool::ExportWait:
        return McpToolExecution::Task;
    default:
        return McpToolExecution::Inline;
    }
}

// Whether a successful call changes the live document (and therefore the undo history). Only
// these tools ever reach the commit marshal.
[[nodiscard]] constexpr bool toolMutatesDocument(const McpTool tool) {
    return tool == McpTool::Transact || tool == McpTool::History;
}

// Whether a tool works on the application window rather than the project: it runs on the owner
// thread through McpLiveUiPort without a session lease, so it works while a project opens.
[[nodiscard]] constexpr bool toolUsesLiveUi(const McpTool tool) {
    return static_cast<std::uint8_t>(tool) >= static_cast<std::uint8_t>(McpTool::ProjectSave) &&
           static_cast<std::uint8_t>(tool) <= static_cast<std::uint8_t>(McpTool::ViewerSet);
}

// ---------------------------------------------------------------------------------------------
// Errors

// JSON-RPC protocol errors. Tool failures are not protocol errors: they are successful
// tools/call responses with isError = true and a structured diagnostic (McpToolErrorCode).
enum class McpProtocolError : std::int32_t {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    // Server-defined range.
    ServerBusy = -32000,
    ShuttingDown = -32001,
    ClientLimitReached = -32002,
};

// Stable tool diagnostic codes ("diagnostic.code" in structuredContent).
enum class McpToolErrorCode : std::uint8_t {
    // A project is opening, closing or being replaced; retry after it binds.
    ProjectUnavailable = 0,
    // The project was replaced while the call ran; nothing was applied.
    ProjectReplaced = 1,
    // expectedRevision did not match; nothing was applied.
    RevisionConflict = 2,
    // The commit queue is full; nothing was queued.
    CommitQueueFull = 3,
    // An interactive gesture stayed open until the call's deadline; nothing was applied.
    GestureOpen = 4,
    // notifications/cancelled, server stop or shutdown.
    Cancelled = 5,
    DeadlineExceeded = 6,
    // The export queue refused the job (its availability code is attached).
    ExportUnavailable = 7,
    // The export runner failed (its ExportFailure is attached).
    ExportFailed = 8,
    // A capture's PNG is larger than the inline image limit; the published file is linked instead.
    CaptureTooLargeForInline = 9,
    // The document is read-only.
    ReadOnly = 10,
    // Operation validation or adapter diagnostics (the registry's own codes are attached).
    OperationRejected = 11,
    // The render or export request itself cannot run as given (a destination the format cannot
    // write, an existing file without overwrite); the message says what to change.
    PreflightBlocked = 12,
};

[[nodiscard]] constexpr std::string_view toolErrorCode(const McpToolErrorCode code) {
    constexpr std::array<std::string_view, 13> names{
        "bloom.mcp.project-unavailable", "bloom.mcp.project-replaced",
        "bloom.mcp.revision-conflict",   "bloom.mcp.commit-queue-full",
        "bloom.mcp.gesture-open",        "bloom.mcp.cancelled",
        "bloom.mcp.deadline-exceeded",   "bloom.mcp.export-unavailable",
        "bloom.mcp.export-failed",       "bloom.mcp.capture-too-large-for-inline",
        "bloom.mcp.read-only",           "bloom.mcp.operation-rejected",
        "bloom.mcp.preflight-blocked",
    };
    return names[static_cast<std::size_t>(code)];
}

// ---------------------------------------------------------------------------------------------
// Deadlines

// How long a commit may wait in the marshal (queue position plus an open gesture) before it is
// withdrawn with GestureOpen or DeadlineExceeded. Task tools have no deadline; they are cancelled
// explicitly.
inline constexpr std::chrono::seconds kMcpCommitDeadline{30};

// Inline capture images: PNG bytes above this are linked, not embedded, so the base64 image plus
// the JSON envelope stays within responseBytes.
inline constexpr std::size_t kMcpInlineImageBytes = std::size_t{3} * 1024U * 1024U - 64U * 1024U;

static_assert(kMcpInlineImageBytes / 3U * 4U + 64U * 1024U <= kMcpMessageLimits.responseBytes);

} // namespace bloom::mcp
