// project.set-color-settings: Project Settings > Colour for agents. It is session state of the
// project (host::ProjectSession), not a document command, so it is a tool beside transact rather
// than an operation: not undoable, never in the history, and it marks the project dirty, exactly as
// the dialog does. Headless it changes the stdio session's project; in the desktop application it
// goes through the live session's colour-settings seam.

#include "core_internal.hpp"

#include <bloom/host/project_color_choice.hpp>

namespace bloom::mcp {

yyjson_mut_val* colorSettingsTool(McpServerCore::State&, Json& out, McpCall& call,
                                  yyjson_val* arguments) {
    members(arguments, {"config", "workingSpace"}, {"config"});
    const auto config = string(member(arguments, "config"), 16);
    std::optional<std::string> working;
    if (auto* value = member(arguments, "workingSpace"))
        working = string(value, document::kMaxWorkingColorSpaceIdBytes);
    call.summary = config;
    const auto result = call.session.setColorSettings(config, std::move(working));
    if (result.code == "unavailable")
        throw ToolFailure(McpToolErrorCode::OperationRejected, result.message, result.code);
    if (result.code == "read-only")
        throw ToolFailure(McpToolErrorCode::ReadOnly, result.message, result.code);
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(result.succeeded()));
    out.set(value, "code", out.text(result.code));
    out.set(value, "message", out.text(result.message));
    if (result.settings) {
        out.set(
            value, "config",
            out.text(host::projectColorConfigName(host::projectColorConfigOf(*result.settings))));
        out.set(value, "workingSpace", out.text(result.settings->processColorSpaceId));
    }
    return value;
}

} // namespace bloom::mcp
