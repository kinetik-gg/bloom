#pragma once

// The desktop application's session and UI seams for MCP (ADR 0034, "Owner decisions" item 9):
// the things an artist does in the application that are not project-truth edits -- opening and
// saving projects, choosing what is selected and which composition is active, moving the
// playhead, and the Viewer's display settings -- offered to agents as live-only tools.
//
// One struct of std::function members so the composition root binds them in one place and a test
// fakes them as a group. Every member is called on the OWNER thread (the UI thread), marshalled
// there by McpServerService; a member left empty answers a typed refusal (McpUiRefusal::Unbound)
// instead of hiding the tool. Session-state members never dirty the project or touch its undo
// history.
//
// Qt-free: the bindings live in apps/bloom over the UI's own controllers.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::mcp {

// Why a session or UI call was refused. Each has a stable diagnostic code (uiRefusalCode).
enum class McpUiRefusal : std::uint8_t {
    None = 0,
    // stdio: there is no application window to act on.
    NotLive = 1,
    // The application did not bind this seam.
    Unbound = 2,
    // A composition, layer or node id that the open project does not have.
    UnknownTarget = 3,
    // An argument the application cannot honour (a display/view this configuration lacks, a
    // zoom outside its range, a time outside the composition).
    InvalidArgument = 4,
    // The application is in the middle of something that must finish first (a modal dialog, a
    // project opening, another lifecycle request).
    Busy = 5,
    // Open, New or Close over unsaved changes without discardChanges.
    UnsavedChanges = 6,
    // Open, New or Close while export jobs run, without stopJobs.
    JobsRunning = 7,
    // Save As onto an existing file without replace.
    DestinationExists = 8,
    // A path that is empty, relative where an absolute one is needed, not a .bloom file, or in a
    // folder that does not exist.
    PathInvalid = 9,
    // Save of a project that has never been saved: use Save As with a path.
    NoPath = 10,
    // The project is read-only (a newer or preserved document).
    ReadOnly = 11,
    // The operation ran and failed (an I/O error, a document that would not open); the message
    // says why.
    Failed = 12,
};

inline constexpr std::size_t kMcpUiRefusalCount = 13;

[[nodiscard]] constexpr std::string_view uiRefusalCode(const McpUiRefusal refusal) {
    constexpr std::array<std::string_view, kMcpUiRefusalCount> codes{
        "",
        "bloom.mcp.not-live",
        "bloom.mcp.ui-unbound",
        "bloom.mcp.unknown-target",
        "bloom.mcp.invalid-argument",
        "bloom.mcp.busy",
        "bloom.mcp.unsaved-changes",
        "bloom.mcp.jobs-running",
        "bloom.mcp.destination-exists",
        "bloom.mcp.path-invalid",
        "bloom.mcp.no-path",
        "bloom.mcp.read-only",
        "bloom.mcp.failed",
    };
    return codes[static_cast<std::size_t>(refusal)];
}

// A synchronous seam's answer. `message` is plain text for the agent and the activity log; for a
// refusal it says why and what to do instead.
struct McpUiResult final {
    McpUiRefusal refusal = McpUiRefusal::None;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return refusal == McpUiRefusal::None; }
};

// ---- Project lifecycle (project.save, project.save-as, project.open, project.new, project.close)

enum class McpProjectRequestKind : std::uint8_t {
    Save = 0,
    SaveAs = 1,
    Open = 2,
    New = 3,
    Close = 4,
};

struct McpProjectRequest final {
    McpProjectRequestKind kind = McpProjectRequestKind::Save;
    // SaveAs and Open: an absolute path to a .bloom file.
    std::filesystem::path path;
    // SaveAs: replace an existing file.
    bool replace = false;
    // Open, New and Close: discard unsaved changes instead of refusing with UnsavedChanges.
    bool discardChanges = false;
    // Open, New and Close: stop running export jobs instead of refusing with JobsRunning.
    bool stopJobs = false;
};

struct McpProjectResult final {
    McpUiRefusal refusal = McpUiRefusal::None;
    // Plain text; a refusal names the project ("Untitled has unsaved changes ...").
    std::string message;
    // The saved or opened file; empty after New, Close or an untitled project.
    std::filesystem::path path;
    // The project now open (after Close: the empty project the application shows).
    std::string projectName;

    [[nodiscard]] bool ok() const noexcept { return refusal == McpUiRefusal::None; }
};

// Asynchronous: the binding starts the request on the owner thread and calls `done` exactly once,
// on the owner thread, when it has finished or been refused. It must not call `done` from inside
// the call when it can avoid it, but the service copes either way.
using McpProjectLifecycle =
    std::function<void(McpProjectRequest, std::function<void(McpProjectResult)> done)>;

// ---- Selection (selection.set)

enum class McpSelectionMode : std::uint8_t {
    // Replace the selection with the given layers and nodes.
    Set = 0,
    Add = 1,
    Remove = 2,
    // Select nothing (layers and nodes are ignored).
    Clear = 3,
};

struct McpSelectionRequest final {
    document::CompositionId composition;
    std::vector<document::LayerId> layers;
    std::vector<document::NodeId> nodes;
    McpSelectionMode mode = McpSelectionMode::Set;
};

// ---- Transport (transport)

enum class McpTransportAction : std::uint8_t {
    // Move the playhead to `time`.
    Seek = 0,
    Play = 1,
    Stop = 2,
    StepForward = 3,
    StepBackward = 4,
    GoToStart = 5,
    GoToEnd = 6,
};

struct McpTransportRequest final {
    McpTransportAction action = McpTransportAction::Seek;
    // Seek: composition time in seconds (MCP converts a whole frame at the composition's rate).
    std::optional<core::RationalTime> time;
};

// ---- Viewer (viewer.set)

enum class McpViewerChannel : std::uint8_t {
    Rgba = 0,
    Rgb = 1,
    Red = 2,
    Green = 3,
    Blue = 4,
    Alpha = 5,
};

enum class McpViewerZoom : std::uint8_t {
    // Fit the composition in the Viewer.
    Fit = 0,
    // 100 percent.
    ActualSize = 1,
    // `zoomFactor`.
    Factor = 2,
};

// Only the fields that are present change.
struct McpViewerRequest final {
    // OCIO display and view names; an empty string returns to the project default.
    std::optional<std::string> display;
    std::optional<std::string> view;
    // Viewer exposure in stops (display-only, never rendered).
    std::optional<double> exposure;
    std::optional<McpViewerChannel> channel;
    std::optional<McpViewerZoom> zoom;
    // With zoom Factor: 1.0 is 100 percent.
    std::optional<double> zoomFactor;
};

struct McpLiveUiPort final {
    // editor.reveal: show the composition in the Viewer (making it active) and, when given, move
    // the playhead to `time` (seconds). False when the composition or time cannot be shown.
    std::function<bool(document::CompositionId, std::optional<core::RationalTime>)>
        revealComposition;
    McpProjectLifecycle projectLifecycle;
    // selection.set
    std::function<McpUiResult(const McpSelectionRequest&)> setSelection;
    // composition.activate: the composition the Viewer, Timeline and contextual arguments use.
    std::function<McpUiResult(document::CompositionId)> activateComposition;
    // transport
    std::function<McpUiResult(const McpTransportRequest&)> transport;
    // viewer.set
    std::function<McpUiResult(const McpViewerRequest&)> setViewer;
};

} // namespace bloom::mcp
