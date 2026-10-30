// The shared MCP server core (ADR 0034, section 2): JSON-RPC 2.0 framing and validation, the MCP
// handshake and tools/call dispatch for both hosts. Moved from apps/bloom-mcp/server.cpp.
#include "core_internal.hpp"

#include <bloom/platform/owner_only_secret.hpp>

#include <algorithm>
#include <array>
#include <chrono>

namespace bloom::mcp {
namespace {

// clang-format off
constexpr std::array<McpTool, kMcpToolCount> kTools{
    McpTool::Query,
    McpTool::Transact,
    McpTool::History,
    McpTool::Events,
    McpTool::Render,
    McpTool::Export,
    McpTool::Capture,
    McpTool::ExportStart,
    McpTool::ExportStartAll,
    McpTool::ExportPause,
    McpTool::ExportResume,
    McpTool::ExportStop,
    McpTool::ExportStopAll,
    McpTool::ExportRetry,
    McpTool::ExportConfirmReplace,
    McpTool::ExportStatus,
    McpTool::ExportEvents,
    McpTool::ExportLog,
    McpTool::ExportWait,
    McpTool::ExportPresets,
    McpTool::ExportScript,
    McpTool::ProjectSave,
    McpTool::ProjectSaveAs,
    McpTool::ProjectOpen,
    McpTool::ProjectNew,
    McpTool::ProjectClose,
    McpTool::EditorReveal,
    McpTool::SelectionSet,
    McpTool::CompositionActivate,
    McpTool::Transport,
    McpTool::ViewerSet,
    McpTool::ProjectSetColorSettings,
};
// clang-format on

std::optional<McpTool> toolFromName(const std::string_view name) {
    for (const auto tool : kTools) {
        if (toolName(tool) == name)
            return tool;
    }
    return std::nullopt;
}

std::string requestIdentity(yyjson_val* id) {
    if (yyjson_is_str(id))
        return "s:" + string(id, 128);
    if (yyjson_is_sint(id))
        return "n:" + std::to_string(yyjson_get_sint(id));
    if (yyjson_is_uint(id))
        return "n:" + std::to_string(yyjson_get_uint(id));
    throw InvalidInput("Invalid request ID");
}

// Client identity strings shown in the panel: control characters removed, at most 256 bytes cut
// on a UTF-8 boundary.
std::string sanitised(yyjson_val* value) {
    if (!yyjson_is_str(value))
        return {};
    std::string text;
    const std::string_view raw(yyjson_get_str(value), yyjson_get_len(value));
    for (const char c : raw) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x20U && byte != 0x7fU)
            text.push_back(c);
    }
    if (text.size() > 256) {
        auto end = std::size_t{256};
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U)
            --end;
        text.resize(end);
    }
    return text;
}

yyjson_mut_val* failureValue(Json& out, const ToolFailure& failure) {
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(false));
    auto* diagnostic = out.object();
    out.set(diagnostic, "code", out.text(toolErrorCode(failure.code)));
    out.set(diagnostic, "message", out.text(failure.what()));
    if (!failure.detail.empty())
        out.set(diagnostic, "detail", out.text(failure.detail));
    out.set(value, "diagnostic", diagnostic);
    if (failure.job)
        out.set(value, "job", out.number(*failure.job));
    if (failure.log)
        out.set(value, "log", logEntries(out, *failure.log));
    return value;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Protocol sessions

McpProtocolSession::McpProtocolSession(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}
McpProtocolSession::~McpProtocolSession() = default;

bool McpProtocolSession::initialized() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->initialized;
}

bool McpProtocolSession::ready() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->ready;
}

std::optional<McpClientInfo> McpProtocolSession::client() const {
    const std::lock_guard lock(state_->mutex);
    return state_->client;
}

// ---------------------------------------------------------------------------------------------
// Core state

McpServerCore::State::State(McpCoreDependencies value) : dependencies(std::move(value)) {
    std::array<std::byte, 8> random{};
    if (!platform::fillSecureRandom(random))
        throw std::runtime_error("The system random source is unavailable");
    for (const auto byte : random)
        cursorToken = (cursorToken << 8U) | std::to_integer<std::uint64_t>(byte);
    // Cursor text is decimal without leading zeros; keep the value non-zero.
    cursorToken |= 1U;
}

McpServerCore::State::~State() {
    const std::lock_guard lock(subscriptionMutex);
    subscription.reset();
}

scripting::LiveSessionHost* McpServerCore::State::liveHost() const noexcept {
    const auto* live =
        std::get_if<std::reference_wrapper<scripting::LiveSessionHost>>(&dependencies.session);
    return live ? &live->get() : nullptr;
}

void McpServerCore::State::record(const commands::CommandEvent& event) {
    const std::lock_guard lock(eventsMutex);
    if (events.size() >= dependencies.limits.retainedCommandEvents)
        events.pop_front();
    events.emplace_back(++sequence, event);
}

void McpServerCore::State::notice(McpNotice value) {
    const std::lock_guard lock(eventsMutex);
    if (events.size() >= dependencies.limits.retainedCommandEvents)
        events.pop_front();
    events.emplace_back(++sequence, std::move(value));
}

void McpServerCore::State::syncEvents() {
    auto* live = liveHost();
    if (live == nullptr)
        return;
    const std::lock_guard lock(subscriptionMutex);
    const auto generation = live->generation();
    if (eventGeneration == generation)
        return;
    // Release the old subscription without holding eventsMutex: the stream holds its own lock
    // while it delivers, and delivery takes eventsMutex.
    subscription.reset();
    {
        const std::lock_guard guard(eventsMutex);
        if (eventGeneration.has_value()) {
            events.clear();
            resetSequence = sequence;
            replaced = true;
        }
        eventGeneration = generation;
    }
    if (const auto stream = live->events())
        subscription =
            stream->subscribe([this](const commands::CommandEvent& event) { record(event); });
}

// ---------------------------------------------------------------------------------------------
// Core

McpServerCore::McpServerCore(McpCoreDependencies dependencies)
    : state_(std::make_unique<State>(std::move(dependencies))) {
    auto& state = *state_;
    state.gpuExportProvider = state.dependencies.gpuProvider;
    if (auto* headless =
            std::get_if<std::reference_wrapper<scripting::Session>>(&state.dependencies.session)) {
        scripting::Events events(headless->get());
        state.subscription = events.subscribe(
            [raw = &state](const commands::CommandEvent& event) { raw->record(event); });
    } else {
        state.syncEvents();
    }
}

McpServerCore::~McpServerCore() = default;

void McpServerCore::followLiveSession() { state_->syncEvents(); }

void McpServerCore::replaceHeadlessProject(const std::function<void()>& replace) {
    auto& state = *state_;
    auto* headless =
        std::get_if<std::reference_wrapper<scripting::Session>>(&state.dependencies.session);
    if (headless == nullptr) {
        replace();
        return;
    }
    const std::lock_guard lock(state.subscriptionMutex);
    // The subscription borrows the old project's event stream, which `replace` destroys.
    state.subscription.reset();
    replace();
    {
        const std::lock_guard guard(state.eventsMutex);
        state.events.clear();
        state.resetSequence = state.sequence;
        state.replaced = true;
    }
    scripting::Events events(headless->get());
    state.subscription = events.subscribe(
        [raw = &state](const commands::CommandEvent& event) { raw->record(event); });
}

std::span<const McpTool> McpServerCore::tools() const noexcept { return kTools; }

std::unique_ptr<McpProtocolSession> McpServerCore::openSession() {
    return std::unique_ptr<McpProtocolSession>(
        new McpProtocolSession(std::make_unique<McpProtocolSession::State>()));
}

std::string McpServerCore::errorResponse(const std::optional<std::string_view> requestKey,
                                         const McpProtocolError code,
                                         const std::string_view message) {
    Json out;
    auto* root = out.object();
    out.set(root, "jsonrpc", out.text("2.0"));
    if (!requestKey || requestKey->size() < 2) {
        out.set(root, "id", out.null());
    } else if (requestKey->starts_with("s:")) {
        out.set(root, "id", out.text(requestKey->substr(2)));
    } else {
        const std::string digits(requestKey->substr(2));
        Parsed number(digits);
        out.set(root, "id", out.copy(number.root()));
    }
    auto* detail = out.object();
    out.set(detail, "code", out.signedNumber(static_cast<std::int64_t>(code)));
    out.set(detail, "message", out.text(message));
    out.set(root, "error", detail);
    return out.encode(root);
}

McpEnvelope inspectMessage(const std::string_view message, const McpMessageLimits& limits) {
    McpEnvelope envelope;
    if (message.empty() || message.size() > limits.requestBytes)
        return envelope;
    try {
        Parsed parsed(message);
        auto* root = parsed.root();
        if (!yyjson_is_obj(root))
            return envelope;
        auto* method = member(root, "method");
        auto* id = member(root, "id");
        if (method == nullptr) {
            if (id != nullptr && (member(root, "result") || member(root, "error")))
                envelope.kind = McpMessageKind::Response;
            return envelope;
        }
        envelope.method = string(method, 128);
        if (id != nullptr && (yyjson_is_int(id) || yyjson_is_str(id))) {
            envelope.kind = McpMessageKind::Request;
            envelope.requestKey = requestIdentity(id);
        } else if (id == nullptr) {
            envelope.kind = McpMessageKind::Notification;
        } else {
            return envelope;
        }
        auto* params = member(root, "params");
        if (envelope.method == "tools/call" && yyjson_is_obj(params)) {
            if (auto* name = member(params, "name"); yyjson_is_str(name))
                envelope.tool =
                    toolFromName(std::string_view(yyjson_get_str(name), yyjson_get_len(name)));
            if (auto* meta = member(params, "_meta"); yyjson_is_obj(meta)) {
                if (auto* token = member(meta, "progressToken");
                    yyjson_is_str(token) || yyjson_is_int(token)) {
                    Json out;
                    envelope.progressToken = out.encode(out.copy(token));
                }
            }
        }
        if (envelope.method == "notifications/cancelled" && yyjson_is_obj(params)) {
            if (auto* cancelled = member(params, "requestId"))
                envelope.cancels = requestIdentity(cancelled);
        }
    } catch (const std::exception&) {
        envelope = {};
    }
    return envelope;
}

McpHandled McpServerCore::handle(McpProtocolSession& protocol, const std::string_view message,
                                 const McpRequestControl& control) {
    auto& state = *state_;
    auto& session = *protocol.state_;
    McpHandled handled;
    const auto protocolError = [&](yyjson_val* id, const McpProtocolError code,
                                   const std::string_view text) {
        handled.outcome = McpCallOutcome::ProtocolError;
        std::optional<std::string> key;
        try {
            if (id != nullptr)
                key = requestIdentity(id);
        } catch (const std::exception&) {
            key.reset();
        }
        handled.response = errorResponse(key, code, text);
        return handled;
    };
    std::unique_ptr<Parsed> parsed;
    try {
        parsed = std::make_unique<Parsed>(message);
    } catch (const std::exception& exception) {
        return protocolError(nullptr, McpProtocolError::ParseError, exception.what());
    }
    auto* request = parsed->root();
    yyjson_val* id = nullptr;
    try {
        // A JSON-RPC response from the client: Bloom sends no requests, so acknowledge and
        // ignore it.
        if (yyjson_is_obj(request) && !member(request, "method") && member(request, "id") &&
            (member(request, "result") || member(request, "error"))) {
            handled.kind = McpMessageKind::Response;
            return handled;
        }
        members(request, {"jsonrpc", "id", "method", "params"}, {"jsonrpc", "method"});
        if (string(member(request, "jsonrpc")) != "2.0")
            throw InvalidInput("Expected JSON-RPC 2.0");
        handled.method = string(member(request, "method"), 128);
        id = member(request, "id");
        if (id && !(yyjson_is_int(id) || yyjson_is_str(id))) {
            id = nullptr;
            throw InvalidInput("Request ID must be an integer or string");
        }
        if (yyjson_is_str(id))
            static_cast<void>(string(id, 128));
    } catch (const std::exception& exception) {
        handled.kind = McpMessageKind::Invalid;
        return protocolError(nullptr, McpProtocolError::InvalidRequest, exception.what());
    }
    auto* params = member(request, "params");
    if (!id) {
        handled.kind = McpMessageKind::Notification;
        handled.summary = handled.method;
        if (handled.method == "notifications/initialized") {
            const std::lock_guard lock(session.mutex);
            if (session.initialized) {
                try {
                    if (params)
                        members(params, {});
                    session.ready = true;
                } catch (const InvalidInput&) {
                    // JSON-RPC notifications never receive a response.
                }
            }
        }
        return handled;
    }
    handled.kind = McpMessageKind::Request;
    handled.summary = handled.method;
    Json out;
    auto* root = out.object();
    out.set(root, "jsonrpc", out.text("2.0"));
    out.set(root, "id", out.copy(id));
    try {
        yyjson_mut_val* result = nullptr;
        const auto& method = handled.method;
        if (method == "initialize") {
            members(params, {"protocolVersion", "capabilities", "clientInfo"},
                    {"protocolVersion", "capabilities", "clientInfo"});
            const auto version = string(member(params, "protocolVersion"), 32);
            if (!yyjson_is_obj(member(params, "capabilities")))
                throw InvalidInput("Invalid capabilities");
            auto* client = member(params, "clientInfo");
            members(client, {"name", "version", "title", "description", "websiteUrl", "icons"},
                    {"name", "version"});
            static_cast<void>(string(member(client, "name"), 256));
            static_cast<void>(string(member(client, "version"), 256));
            {
                const std::lock_guard lock(session.mutex);
                if (session.initialized)
                    throw InvalidInput("Already initialized");
                session.initialized = true;
                session.client = McpClientInfo{sanitised(member(client, "name")),
                                               sanitised(member(client, "version")),
                                               sanitised(member(client, "title")), version};
            }
            handled.initializedClient = true;
            result = out.object();
            out.set(result, "protocolVersion", out.text(kMcpProtocolVersion));
            auto* info = out.object();
            out.set(info, "name", out.text("Bloom"));
            out.set(info, "version", out.text(state.dependencies.serverVersion));
            out.set(result, "serverInfo", info);
            auto* capabilities = out.object();
            auto* toolCapabilities = out.object();
            out.set(toolCapabilities, "listChanged", out.boolean(false));
            out.set(capabilities, "tools", toolCapabilities);
            auto* experimental = out.object();
            auto* bloom = out.object();
            out.set(bloom, "facadeVersion", out.text("1.0"));
            out.set(bloom, "headless", out.boolean(state.liveHost() == nullptr));
            out.set(bloom, "requestByteLimit", out.number(state.dependencies.limits.requestBytes));
            out.set(experimental, "bloom", bloom);
            out.set(capabilities, "experimental", experimental);
            out.set(result, "capabilities", capabilities);
        } else if (method == "ping") {
            if (params)
                members(params, {});
            result = out.object();
        } else if (!protocol.ready()) {
            throw InvalidInput("Complete initialize and notifications/initialized first");
        } else if (method == "tools/list") {
            if (params)
                members(params, {});
            result = toolList(out);
        } else if (method == "tools/call") {
            members(params, {"name", "arguments", "_meta"}, {"name", "arguments"});
            const auto name = string(member(params, "name"), 64);
            const auto tool = toolFromName(name);
            if (!tool)
                throw InvalidInput("Unknown tool");
            handled.tool = tool;
            handled.summary = std::string(toolName(*tool));
            result = out.object();
            yyjson_mut_val* value = nullptr;
            bool failed = false;
            std::vector<yyjson_mut_val*> content;
            try {
                if (toolUsesLiveUi(*tool)) {
                    // Window and project-lifecycle tools hold no lease: project.open must work
                    // while the old project is being replaced.
                    std::string summary;
                    value = sessionTool(state, out, *tool, member(params, "arguments"), control,
                                        summary);
                    if (!summary.empty() && summary != handled.summary)
                        handled.summary += " " + summary;
                } else {
                    // Read and commit through the live lease or the headless session.
                    std::optional<scripting::LiveSessionLease> lease;
                    scripting::Session* target = nullptr;
                    scripting::OperationContext context;
                    std::uint64_t generation = 0;
                    if (auto* live = state.liveHost()) {
                        state.syncEvents();
                        lease.emplace(live->acquire(
                            {.stop = control.stop,
                             .deadline = std::chrono::steady_clock::now() + kMcpCommitDeadline}));
                        if (!*lease)
                            throw ToolFailure(
                                McpToolErrorCode::ProjectUnavailable,
                                lease->availability() ==
                                        scripting::LiveSessionAvailability::ShuttingDown
                                    ? "Bloom is closing."
                                    : "A project is opening.");
                        target = &lease->session();
                        context = lease->operationContext();
                        generation = lease->generation();
                    } else {
                        auto& headless = std::get<std::reference_wrapper<scripting::Session>>(
                                             state.dependencies.session)
                                             .get();
                        target = &headless;
                        context = headless.operationContext();
                    }
                    McpCall call{.session = *target,
                                 .context = std::move(context),
                                 .live = lease ? &lease->state() : nullptr,
                                 .lease = lease ? &*lease : nullptr,
                                 .generation = generation,
                                 .control = control,
                                 .content = {},
                                 .summary = {},
                                 .notice = std::nullopt};
                    auto* arguments = member(params, "arguments");
                    switch (*tool) {
                    case McpTool::Query:
                        value = queryTool(state, out, call, arguments);
                        break;
                    case McpTool::Transact:
                        value = transactTool(state, out, call, arguments);
                        break;
                    case McpTool::History:
                        value = historyTool(state, out, call, arguments);
                        break;
                    case McpTool::Events:
                        value = eventsTool(state, out, call, arguments);
                        break;
                    case McpTool::Render:
                    case McpTool::Export:
                        value = renderTool(state, out, call, arguments, *tool == McpTool::Export);
                        break;
                    case McpTool::Capture:
                        value = captureTool(state, out, call, arguments);
                        break;
                    case McpTool::ProjectSetColorSettings:
                        value = colorSettingsTool(state, out, call, arguments);
                        break;
                    default:
                        value = exportSessionTool(state, out, call, *tool, arguments);
                        break;
                    }
                    if (!call.summary.empty())
                        handled.summary += " " + call.summary;
                    content = std::move(call.content);
                    handled.toolError = call.notice;
                }
            } catch (const ToolFailure& failure) {
                failed = true;
                handled.toolError = failure.code;
                value = failureValue(out, failure);
                if (failure.code == McpToolErrorCode::Cancelled)
                    handled.outcome = McpCallOutcome::Cancelled;
            } catch (const std::runtime_error& exception) {
                if (dynamic_cast<const InvalidInput*>(&exception))
                    throw;
                failed = true;
                if (control.stop.stop_requested()) {
                    handled.toolError = McpToolErrorCode::Cancelled;
                    handled.outcome = McpCallOutcome::Cancelled;
                }
                value = out.object();
                out.set(value, "diagnostic", out.text(exception.what()));
            }
            auto* succeeded = yyjson_mut_obj_get(value, "succeeded");
            if (succeeded && yyjson_mut_is_false(succeeded))
                failed = true;
            if (failed && handled.outcome == McpCallOutcome::Succeeded)
                handled.outcome = McpCallOutcome::ToolError;
            out.set(result, "isError", out.boolean(failed));
            out.set(result, "structuredContent", value);
            auto* blocks = out.array();
            for (auto* block : content)
                out.append(blocks, block);
            auto* text = out.object();
            out.set(text, "type", out.text("text"));
            out.set(text, "text", out.text(out.encode(value)));
            out.append(blocks, text);
            out.set(result, "content", blocks);
        } else {
            return protocolError(id, McpProtocolError::MethodNotFound, "Method not found");
        }
        out.set(root, "result", result);
        handled.response = out.encode(root);
        if (handled.summary.size() > 256)
            handled.summary.resize(256);
        return handled;
    } catch (const InvalidInput& exception) {
        return protocolError(id, McpProtocolError::InvalidParams, exception.what());
    } catch (const std::exception& exception) {
        return protocolError(id, McpProtocolError::InternalError, exception.what());
    }
}

} // namespace bloom::mcp
