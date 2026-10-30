// project.set-color-settings: Project Settings > Colour for agents. It is session state of the
// project (host::ProjectSession), not a document command, so it is a tool beside transact rather
// than an operation: not undoable, never in the history, and it marks the project dirty, exactly as
// the dialog does. Headless it changes the stdio session's project; in the desktop application it
// goes through McpLiveUiPort::setColorSettings on the application's thread.

#include "core_internal.hpp"

#include <bloom/host/project_color_choice.hpp>

namespace bloom::mcp {

namespace {

yyjson_mut_val* colorResult(Json& out, const std::string& code, const std::string& message,
                            const std::optional<document::ColorSettings>& settings,
                            const bool succeeded) {
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(succeeded));
    out.set(value, "code", out.text(code));
    out.set(value, "message", out.text(message));
    if (settings) {
        out.set(value, "config",
                out.text(host::projectColorConfigName(host::projectColorConfigOf(*settings))));
        out.set(value, "workingSpace", out.text(settings->processColorSpaceId));
    }
    return value;
}

// The desktop application: the settings are resolved here, against the live project's current
// ones, and applied by the application on its own thread through McpLiveUiPort::setColorSettings.
yyjson_mut_val* liveColorSettings(McpServerCore::State& core, Json& out, McpCall& call,
                                  const std::string& config, std::optional<std::string> working) {
    const auto& apply = core.dependencies.ui.setColorSettings;
    if (!apply) {
        auto* value = out.object();
        out.set(value, "succeeded", out.boolean(false));
        auto* diagnostic = out.object();
        out.set(diagnostic, "code", out.text(uiRefusalCode(McpUiRefusal::Unbound)));
        out.set(diagnostic, "message",
                out.text("This Bloom window does not offer project.set-color-settings to agents "
                         "yet."));
        out.set(value, "diagnostic", diagnostic);
        return value;
    }
    const auto choice = host::parseProjectColorConfig(config);
    if (!choice)
        return colorResult(out, "invalid-config",
                           R"(Expected config "neutral", "aces" or "current")", std::nullopt,
                           false);
    const auto current = call.session.renderSourceSnapshot().colorSettings;
    if (!current)
        return colorResult(out, "unavailable", "This project has no colour settings", std::nullopt,
                           false);
    auto resolved = host::resolveProjectColorSettings(*current, *choice, std::move(working));
    if (!resolved.settings)
        return colorResult(out, "invalid-settings", resolved.reason, current, false);
    if (*resolved.settings == *current)
        return colorResult(out, "no-change", "The project already uses these colour settings",
                           current, true);
    McpUiResult applied;
    if (!core.dependencies.exports.run(
            [&](host::ExportQueueService*, std::uint64_t) { applied = apply(*resolved.settings); },
            call.control.stop))
        throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
    if (applied.refusal == McpUiRefusal::ReadOnly)
        throw ToolFailure(McpToolErrorCode::ReadOnly,
                          applied.message.empty() ? "The project is read-only." : applied.message,
                          "read-only");
    if (!applied.ok()) {
        auto* value = out.object();
        out.set(value, "succeeded", out.boolean(false));
        auto* diagnostic = out.object();
        out.set(diagnostic, "code", out.text(uiRefusalCode(applied.refusal)));
        out.set(diagnostic, "message",
                out.text(applied.message.empty() ? "Bloom refused the colour settings."
                                                 : applied.message));
        out.set(value, "diagnostic", diagnostic);
        return value;
    }
    return colorResult(out, "updated", "The project colour settings changed", resolved.settings,
                       true);
}

} // namespace

yyjson_mut_val* colorSettingsTool(McpServerCore::State& core, Json& out, McpCall& call,
                                  yyjson_val* arguments) {
    members(arguments, {"config", "workingSpace"}, {"config"});
    const auto config = string(member(arguments, "config"), 16);
    std::optional<std::string> working;
    if (auto* value = member(arguments, "workingSpace"))
        working = string(value, document::kMaxWorkingColorSpaceIdBytes);
    call.summary = config;
    if (call.live != nullptr)
        return liveColorSettings(core, out, call, config, std::move(working));
    const auto result = call.session.setColorSettings(config, std::move(working));
    if (result.code == "unavailable")
        throw ToolFailure(McpToolErrorCode::OperationRejected, result.message, result.code);
    if (result.code == "read-only")
        throw ToolFailure(McpToolErrorCode::ReadOnly, result.message, result.code);
    return colorResult(out, result.code, result.message, result.settings, result.succeeded());
}

} // namespace bloom::mcp
