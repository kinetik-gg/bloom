// The tools that act on the application window rather than the project (live_ui_port.hpp): project
// lifecycle, editor.reveal, selection, active composition, transport and Viewer settings. They run
// through McpExportAccess::run on the owner thread and hold no session lease, so project.open works
// while the old project is being replaced. Session-state tools never dirty the project.
#include "core_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>

namespace bloom::mcp {
namespace {

yyjson_mut_val* refusal(Json& out, const McpUiRefusal code, const std::string& message) {
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(false));
    auto* diagnostic = out.object();
    out.set(diagnostic, "code", out.text(uiRefusalCode(code)));
    out.set(diagnostic, "message", out.text(message));
    out.set(value, "diagnostic", diagnostic);
    return value;
}

yyjson_mut_val* uiResult(Json& out, const McpUiResult& result) {
    if (!result.ok())
        return refusal(out, result.refusal,
                       result.message.empty() ? "Bloom refused the request." : result.message);
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(true));
    if (!result.message.empty())
        out.set(value, "message", out.text(result.message));
    return value;
}

// Runs `work` on the owner thread; false when the request stopped or the server is stopping.
template <typename Work>
bool onOwner(McpServerCore::State& core, const McpRequestControl& control, Work work) {
    return core.dependencies.exports.run(
        [&work](host::ExportQueueService*, std::uint64_t) { work(); }, control.stop);
}

[[noreturn]] void cancelled() {
    throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
}

std::uint64_t compositionArgument(yyjson_val* arguments) {
    const auto id = integer(member(arguments, "composition"));
    if (id == 0)
        throw InvalidInput("composition must be a composition id (query kind=compositions)");
    return id;
}

// The current snapshot and context composition, read through a short lease on the live host or
// from the headless session.
struct Current final {
    document::Snapshot snapshot;
    std::optional<std::uint64_t> composition;
};

Current current(McpServerCore::State& core, const McpRequestControl& control) {
    if (auto* live = core.liveHost()) {
        auto lease =
            live->acquire({.stop = control.stop,
                           .deadline = std::chrono::steady_clock::now() + kMcpCommitDeadline});
        if (!lease)
            throw ToolFailure(McpToolErrorCode::ProjectUnavailable, "A project is opening.");
        return {lease.session().snapshot(), lease.operationContext().composition};
    }
    auto& session =
        std::get<std::reference_wrapper<scripting::Session>>(core.dependencies.session).get();
    return {session.snapshot(), session.operationContext().composition};
}

} // namespace

// The time contract every client shares: an integer is a whole frame of `composition` at its frame
// rate, and [numerator, denominator] is an exact time in seconds.
core::RationalTime frameOrSecondsArgument(yyjson_val* value, const document::Snapshot& snapshot,
                                          const std::uint64_t compositionId) {
    if (yyjson_is_arr(value) && yyjson_arr_size(value) == 2 &&
        yyjson_is_int(yyjson_arr_get(value, 0)) && yyjson_is_int(yyjson_arr_get(value, 1))) {
        const auto time = core::RationalTime::create(yyjson_get_sint(yyjson_arr_get(value, 0)),
                                                     yyjson_get_sint(yyjson_arr_get(value, 1)));
        if (!time)
            throw InvalidInput("time [numerator, denominator] needs a non-zero denominator");
        return *time;
    }
    if (!yyjson_is_int(value))
        throw InvalidInput("time is a whole frame (integer) or [numerator, denominator] seconds");
    const auto* composition =
        snapshot.project().findComposition(document::CompositionId::fromRaw(compositionId));
    if (composition == nullptr)
        throw InvalidInput("Composition " + std::to_string(compositionId) +
                           " does not exist; query kind=compositions lists them");
    const auto rate = composition->format().frameRate();
    const auto frames = yyjson_get_sint(value);
    constexpr auto limit = std::numeric_limits<std::int64_t>::max() / 4294967296LL;
    const auto time =
        frames > limit || frames < -limit
            ? std::nullopt
            : core::RationalTime::create(frames * static_cast<std::int64_t>(rate.denominator()),
                                         static_cast<std::int64_t>(rate.numerator()));
    if (!time)
        throw InvalidInput("time is too large for the composition's frame rate");
    return *time;
}

namespace {

yyjson_mut_val* projectTool(McpServerCore::State& core, Json& out, const McpTool tool,
                            yyjson_val* arguments, const McpRequestControl& control,
                            std::string& summary) {
    McpProjectRequest request;
    switch (tool) {
    case McpTool::ProjectSave:
        members(arguments, {});
        request.kind = McpProjectRequestKind::Save;
        break;
    case McpTool::ProjectSaveAs:
        members(arguments, {"path", "replace"}, {"path"});
        request.kind = McpProjectRequestKind::SaveAs;
        request.path = string(member(arguments, "path"));
        request.replace = member(arguments, "replace") && boolean(member(arguments, "replace"));
        break;
    case McpTool::ProjectOpen:
        members(arguments, {"path", "discardChanges", "stopJobs"}, {"path"});
        request.kind = McpProjectRequestKind::Open;
        request.path = string(member(arguments, "path"));
        break;
    case McpTool::ProjectNew:
        members(arguments, {"discardChanges", "stopJobs"});
        request.kind = McpProjectRequestKind::New;
        break;
    default:
        members(arguments, {"discardChanges", "stopJobs"});
        request.kind = McpProjectRequestKind::Close;
        break;
    }
    if (tool == McpTool::ProjectOpen || tool == McpTool::ProjectNew ||
        tool == McpTool::ProjectClose) {
        request.discardChanges =
            member(arguments, "discardChanges") && boolean(member(arguments, "discardChanges"));
        request.stopJobs = member(arguments, "stopJobs") && boolean(member(arguments, "stopJobs"));
    }
    if ((request.kind == McpProjectRequestKind::SaveAs ||
         request.kind == McpProjectRequestKind::Open) &&
        (request.path.empty() || !request.path.is_absolute()))
        return refusal(out, McpUiRefusal::PathInvalid,
                       "The path must be an absolute path to a .bloom file.");
    const auto& lifecycle = core.dependencies.ui.projectLifecycle;
    if (!lifecycle)
        return refusal(out, McpUiRefusal::Unbound,
                       "This Bloom window cannot open or save projects for agents yet.");

    struct Pending {
        std::mutex mutex;
        std::condition_variable_any changed;
        std::optional<McpProjectResult> result;
    };
    const auto pending = std::make_shared<Pending>();
    const auto ran = onOwner(core, control, [&] {
        lifecycle(request, [pending](McpProjectResult result) {
            {
                const std::lock_guard lock(pending->mutex);
                if (!pending->result)
                    pending->result = std::move(result);
            }
            pending->changed.notify_all();
        });
    });
    if (!ran)
        cancelled();
    std::unique_lock lock(pending->mutex);
    pending->changed.wait(lock, control.stop, [&] { return pending->result.has_value(); });
    if (!pending->result)
        throw ToolFailure(McpToolErrorCode::Cancelled,
                          "The request was cancelled; the project operation may still finish. "
                          "Query the project to see where it ended.");
    const auto result = *pending->result;
    lock.unlock();
    // The activity log reads the project's name after the tool name ("project.save SC020").
    summary = result.ok() && !result.projectName.empty() ? result.projectName
                                                         : std::string(toolName(tool));
    if (!result.ok())
        return refusal(out, result.refusal,
                       result.message.empty() ? "Bloom refused the request." : result.message);
    constexpr std::array<std::string_view, 5> kinds{
        "project.saved", "project.saved", "project.opened", "project.created", "project.closed"};
    McpNotice notice{.kind = std::string(kinds.at(static_cast<std::size_t>(request.kind))),
                     .message = result.message,
                     .revision = 0,
                     .composition = std::nullopt,
                     .node = std::nullopt,
                     .parameter = std::nullopt,
                     .path = result.path.generic_string(),
                     .projectName = result.projectName};
    try {
        notice.revision = current(core, control).snapshot.revision().value();
    } catch (const std::exception&) {
        // The new project is still binding; the notice keeps revision 0.
    }
    core.notice(notice);
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(true));
    out.set(value, "event", out.text(notice.kind));
    out.set(value, "path", result.path.empty() ? out.null() : out.text(notice.path));
    out.set(value, "projectName", out.text(result.projectName));
    if (!result.message.empty())
        out.set(value, "message", out.text(result.message));
    return value;
}

std::vector<std::uint64_t> idList(yyjson_val* value, const char* name) {
    std::vector<std::uint64_t> ids;
    if (value == nullptr)
        return ids;
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) > 4096)
        throw InvalidInput(std::string(name) + " is a list of at most 4096 ids");
    yyjson_arr_iter iterator = yyjson_arr_iter_with(value);
    while (auto* item = yyjson_arr_iter_next(&iterator)) {
        const auto id = integer(item);
        if (id == 0)
            throw InvalidInput(std::string(name) + " ids are positive");
        ids.push_back(id);
    }
    return ids;
}

} // namespace

yyjson_mut_val* sessionTool(McpServerCore::State& core, Json& out, const McpTool tool,
                            yyjson_val* arguments, const McpRequestControl& control,
                            std::string& summary) {
    if (tool >= McpTool::ProjectSave && tool <= McpTool::ProjectClose)
        return projectTool(core, out, tool, arguments, control, summary);
    const bool live = core.liveHost() != nullptr;
    const auto& ui = core.dependencies.ui;
    const auto notLive = [&] {
        return refusal(out, McpUiRefusal::NotLive,
                       "This server has no application window; " + std::string(toolName(tool)) +
                           " works only with the in-app MCP server.");
    };
    const auto unbound = [&] {
        return refusal(out, McpUiRefusal::Unbound,
                       "This Bloom window does not offer " + std::string(toolName(tool)) +
                           " to agents yet.");
    };
    switch (tool) {
    case McpTool::EditorReveal: {
        members(arguments, {"composition", "time"}, {"composition"});
        const auto composition = compositionArgument(arguments);
        if (!live)
            return notLive();
        if (!ui.revealComposition)
            return unbound();
        std::optional<core::RationalTime> time;
        if (auto* value = member(arguments, "time"))
            time = frameOrSecondsArgument(value, current(core, control).snapshot, composition);
        bool shown = false;
        if (!onOwner(core, control, [&] {
                shown = ui.revealComposition(document::CompositionId::fromRaw(composition), time);
            }))
            cancelled();
        summary = "editor.reveal";
        if (!shown)
            return refusal(out, McpUiRefusal::UnknownTarget,
                           "Composition " + std::to_string(composition) +
                               " could not be shown; query kind=compositions lists the "
                               "compositions this project has.");
        return uiResult(out, {});
    }
    case McpTool::SelectionSet: {
        members(arguments, {"composition", "layers", "nodes", "mode"}, {"composition"});
        McpSelectionRequest request;
        request.composition = document::CompositionId::fromRaw(compositionArgument(arguments));
        for (const auto id : idList(member(arguments, "layers"), "layers"))
            request.layers.push_back(document::LayerId::fromRaw(id));
        for (const auto id : idList(member(arguments, "nodes"), "nodes"))
            request.nodes.push_back(document::NodeId::fromRaw(id));
        if (auto* mode = member(arguments, "mode")) {
            const auto name = string(mode, 16);
            constexpr std::array<std::string_view, 4> modes{"set", "add", "remove", "clear"};
            const auto found = std::ranges::find(modes, name);
            if (found == modes.end())
                throw InvalidInput("mode is set, add, remove or clear");
            request.mode = static_cast<McpSelectionMode>(std::distance(modes.begin(), found));
        }
        if (!live)
            return notLive();
        if (!ui.setSelection)
            return unbound();
        McpUiResult result;
        if (!onOwner(core, control, [&] { result = ui.setSelection(request); }))
            cancelled();
        summary = "selection.set";
        return uiResult(out, result);
    }
    case McpTool::CompositionActivate: {
        members(arguments, {"composition"}, {"composition"});
        const auto composition = compositionArgument(arguments);
        if (!live)
            return notLive();
        if (!ui.activateComposition)
            return unbound();
        McpUiResult result;
        if (!onOwner(core, control, [&] {
                result = ui.activateComposition(document::CompositionId::fromRaw(composition));
            }))
            cancelled();
        summary = "composition.activate";
        return uiResult(out, result);
    }
    case McpTool::Transport: {
        members(arguments, {"action", "composition", "time"}, {"action"});
        const auto action = string(member(arguments, "action"), 16);
        constexpr std::array<std::string_view, 7> actions{
            "seek", "play", "stop", "step-forward", "step-backward", "go-to-start", "go-to-end"};
        const auto found = std::ranges::find(actions, action);
        if (found == actions.end())
            throw InvalidInput("action is seek, play, stop, step-forward, step-backward, "
                               "go-to-start or go-to-end");
        McpTransportRequest request;
        request.action = static_cast<McpTransportAction>(std::distance(actions.begin(), found));
        if (request.action == McpTransportAction::Seek && !member(arguments, "time"))
            throw InvalidInput("seek needs time: a whole frame, or [numerator, denominator] "
                               "seconds");
        if (!live)
            return notLive();
        if (!ui.transport)
            return unbound();
        if (auto* value = member(arguments, "time")) {
            const auto now = current(core, control);
            const auto composition = member(arguments, "composition")
                                         ? compositionArgument(arguments)
                                         : now.composition.value_or(0);
            if (composition == 0 && yyjson_is_int(value))
                throw InvalidInput("a time in frames needs a composition: pass composition, or "
                                   "give the time as [numerator, denominator] seconds");
            request.time = frameOrSecondsArgument(value, now.snapshot, composition);
        }
        McpUiResult result;
        if (!onOwner(core, control, [&] { result = ui.transport(request); }))
            cancelled();
        summary = "transport " + action;
        return uiResult(out, result);
    }
    case McpTool::ViewerSet: {
        members(arguments, {"display", "view", "exposure", "channel", "zoom"});
        McpViewerRequest request;
        if (auto* display = member(arguments, "display"))
            request.display = string(display, 256);
        if (auto* view = member(arguments, "view"))
            request.view = string(view, 256);
        if (auto* exposure = member(arguments, "exposure")) {
            if (!yyjson_is_num(exposure) || !std::isfinite(yyjson_get_num(exposure)) ||
                std::abs(yyjson_get_num(exposure)) > 64.0)
                throw InvalidInput("exposure is a number of stops in -64..64");
            request.exposure = yyjson_get_num(exposure);
        }
        if (auto* channel = member(arguments, "channel")) {
            const auto name = string(channel, 16);
            constexpr std::array<std::string_view, 6> channels{"rgba",  "rgb",  "red",
                                                               "green", "blue", "alpha"};
            const auto found = std::ranges::find(channels, name);
            if (found == channels.end())
                throw InvalidInput("channel is rgba, rgb, red, green, blue or alpha");
            request.channel = static_cast<McpViewerChannel>(std::distance(channels.begin(), found));
        }
        if (auto* zoom = member(arguments, "zoom")) {
            if (yyjson_is_str(zoom)) {
                const auto name = string(zoom, 16);
                if (name == "fit")
                    request.zoom = McpViewerZoom::Fit;
                else if (name == "actual-size")
                    request.zoom = McpViewerZoom::ActualSize;
                else
                    throw InvalidInput(
                        "zoom is \"fit\", \"actual-size\" or a factor (1.0 is 100%)");
            } else if (yyjson_is_num(zoom) && std::isfinite(yyjson_get_num(zoom)) &&
                       yyjson_get_num(zoom) > 0.0 && yyjson_get_num(zoom) <= 64.0) {
                request.zoom = McpViewerZoom::Factor;
                request.zoomFactor = yyjson_get_num(zoom);
            } else {
                throw InvalidInput("zoom is \"fit\", \"actual-size\" or a factor in (0, 64]");
            }
        }
        if (!live)
            return notLive();
        if (!ui.setViewer)
            return unbound();
        McpUiResult result;
        if (!onOwner(core, control, [&] { result = ui.setViewer(request); }))
            cancelled();
        summary = "viewer.set";
        return uiResult(out, result);
    }
    default:
        throw InvalidInput("Unknown tool");
    }
}

} // namespace bloom::mcp
