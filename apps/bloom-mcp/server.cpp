#include "server.hpp"

#include <array>
#include <chrono>
#include <limits>
#include <thread>

namespace bloom::mcp {

namespace {
std::string requestIdentity(yyjson_val* id) {
    if (yyjson_is_str(id))
        return "s:" + string(id, 128);
    if (yyjson_is_sint(id))
        return "n:" + std::to_string(yyjson_get_sint(id));
    if (yyjson_is_uint(id))
        return "n:" + std::to_string(yyjson_get_uint(id));
    throw InvalidInput("Invalid cancellation request ID");
}
} // namespace

class Server::ActiveRequest final {
  public:
    ActiveRequest(Server& server, yyjson_val* id) : server_(server) {
        {
            const std::lock_guard lock(server_.activeMutex_);
            server_.activeId_ = requestIdentity(id);
            server_.cancellationRequested_.store(false);
        }
        monitor_ = std::jthread([&server](const std::stop_token& stop) {
            while (!stop.stop_requested()) {
                if (server.cancellationRequested_.load()) {
                    for (const auto& task : server.scheduler_.snapshots()) {
                        if (!runtime::isTerminal(task.state))
                            static_cast<void>(server.scheduler_.cancel(task.id));
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }
    ~ActiveRequest() {
        monitor_.request_stop();
        monitor_.join();
        const std::lock_guard lock(server_.activeMutex_);
        server_.activeId_.clear();
    }
    ActiveRequest(const ActiveRequest&) = delete;
    ActiveRequest& operator=(const ActiveRequest&) = delete;

  private:
    Server& server_;
    std::jthread monitor_;
};

bool Server::interceptCancellation(const std::string_view message) {
    try {
        Parsed parsed(message);
        auto* root = parsed.root();
        if (member(root, "id") || !member(root, "method") ||
            string(member(root, "method"), 128) != "notifications/cancelled")
            return false;
        members(root, {"jsonrpc", "method", "params"}, {"jsonrpc", "method", "params"});
        if (string(member(root, "jsonrpc")) != "2.0")
            return false;
        auto* params = member(root, "params");
        members(params, {"requestId", "reason", "_meta"}, {"requestId"});
        const auto identity = requestIdentity(member(params, "requestId"));
        const std::lock_guard lock(activeMutex_);
        if (identity == activeId_)
            cancellationRequested_.store(true);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

Server::Server(std::unique_ptr<scripting::Session> session)
    : session_(std::move(session)), facade_(*session_) {
    subscription_ = facade_.events.subscribe([this](const commands::CommandEvent& event) {
        if (events_.size() == 1024) {
            events_.pop_front();
        }
        events_.emplace_back(++sequence_, event);
    });
}

std::string Server::error(yyjson_val* id, const int code, const std::string_view message) {
    Json out;
    auto* root = out.object();
    out.set(root, "jsonrpc", out.text("2.0"));
    out.set(root, "id", out.copy(id));
    auto* detail = out.object();
    Parsed number(std::to_string(code));
    out.set(detail, "code", out.copy(number.root()));
    out.set(detail, "message", out.text(message));
    out.set(root, "error", detail);
    return out.encode(root);
}

std::optional<std::string> Server::handle(const std::string_view message) {
    std::unique_ptr<Parsed> parsed;
    try {
        parsed = std::make_unique<Parsed>(message);
    } catch (const std::exception& exception) {
        return error(nullptr, -32700, exception.what());
    }
    auto* request = parsed->root();
    yyjson_val* id = nullptr;
    std::string method;
    try {
        members(request, {"jsonrpc", "id", "method", "params"}, {"jsonrpc", "method"});
        if (string(member(request, "jsonrpc")) != "2.0") {
            throw InvalidInput("Expected JSON-RPC 2.0");
        }
        method = string(member(request, "method"), 128);
        id = member(request, "id");
        if (id && !(yyjson_is_int(id) || yyjson_is_str(id))) {
            id = nullptr;
            throw InvalidInput("Request ID must be an integer or string");
        }
        if (yyjson_is_str(id)) {
            static_cast<void>(string(id, 128));
        }
    } catch (const std::exception& exception) {
        return error(nullptr, -32600, exception.what());
    }
    auto* params = member(request, "params");
    if (!id) {
        if (method == "notifications/initialized" && initialized_) {
            try {
                if (params)
                    members(params, {});
                ready_ = true;
            } catch (const InvalidInput&) {
                // JSON-RPC notifications never receive a response.
                return std::nullopt;
            }
        }
        return std::nullopt;
    }
    Json out;
    auto* root = out.object();
    out.set(root, "jsonrpc", out.text("2.0"));
    out.set(root, "id", out.copy(id));
    try {
        yyjson_mut_val* result = nullptr;
        if (method == "initialize") {
            if (initialized_)
                throw InvalidInput("Already initialized");
            members(params, {"protocolVersion", "capabilities", "clientInfo"},
                    {"protocolVersion", "capabilities", "clientInfo"});
            static_cast<void>(string(member(params, "protocolVersion"), 32));
            if (!yyjson_is_obj(member(params, "capabilities")))
                throw InvalidInput("Invalid capabilities");
            auto* client = member(params, "clientInfo");
            members(client, {"name", "version", "title", "description", "websiteUrl", "icons"},
                    {"name", "version"});
            static_cast<void>(string(member(client, "name"), 256));
            static_cast<void>(string(member(client, "version"), 256));
            result = out.object();
            out.set(result, "protocolVersion", out.text("2025-11-25"));
            auto* info = out.object();
            out.set(info, "name", out.text("Bloom"));
            out.set(info, "version", out.text(BLOOM_VERSION));
            out.set(result, "serverInfo", info);
            auto* capabilities = out.object();
            auto* toolCapabilities = out.object();
            out.set(toolCapabilities, "listChanged", out.boolean(false));
            out.set(capabilities, "tools", toolCapabilities);
            auto* experimental = out.object();
            auto* bloom = out.object();
            out.set(bloom, "facadeVersion", out.text("1.0"));
            out.set(bloom, "headless", out.boolean(true));
            out.set(bloom, "requestByteLimit", out.number(kRequestLimit));
            out.set(experimental, "bloom", bloom);
            out.set(capabilities, "experimental", experimental);
            out.set(result, "capabilities", capabilities);
            initialized_ = true;
        } else if (method == "ping") {
            if (params)
                members(params, {});
            result = out.object();
        } else if (!ready_) {
            throw InvalidInput("Complete initialize and notifications/initialized first");
        } else if (method == "tools/list") {
            if (params)
                members(params, {});
            result = tools(out);
        } else if (method == "tools/call") {
            members(params, {"name", "arguments", "_meta"}, {"name", "arguments"});
            const auto name = string(member(params, "name"), 64);
            ActiveRequest active(*this, id);
            result = out.object();
            yyjson_mut_val* value = nullptr;
            bool failed = false;
            try {
                value = call(out, name, member(params, "arguments"));
            } catch (const std::runtime_error& exception) {
                if (dynamic_cast<const InvalidInput*>(&exception))
                    throw;
                failed = true;
                value = out.object();
                out.set(value, "diagnostic", out.text(exception.what()));
            }
            auto* succeeded = yyjson_mut_obj_get(value, "succeeded");
            if (succeeded && yyjson_mut_is_false(succeeded))
                failed = true;
            out.set(result, "isError", out.boolean(failed));
            out.set(result, "structuredContent", value);
            auto* content = out.array();
            auto* text = out.object();
            out.set(text, "type", out.text("text"));
            out.set(text, "text", out.text(out.encode(value)));
            out.append(content, text);
            out.set(result, "content", content);
        } else {
            return error(id, -32601, "Method not found");
        }
        out.set(root, "result", result);
        return out.encode(root);
    } catch (const InvalidInput& exception) {
        return error(id, -32602, exception.what());
    } catch (const std::exception& exception) {
        return error(id, -32603, exception.what());
    }
}

yyjson_mut_val* Server::call(Json& out, const std::string_view tool, yyjson_val* arguments) {
    if (tool == "query")
        return query(out, arguments);
    if (tool == "transact")
        return transact(out, arguments);
    if (tool == "render")
        return render(out, arguments, false);
    if (tool == "export")
        return render(out, arguments, true);
    if (tool == "events")
        return events(out, arguments);
    throw InvalidInput("Unknown tool");
}

yyjson_mut_val* Server::events(Json& out, yyjson_val* arguments) {
    members(arguments, {"sinceRevision", "afterSequence"}, {"sinceRevision"});
    const auto since = integer(member(arguments, "sinceRevision"));
    const auto after =
        member(arguments, "afterSequence") ? integer(member(arguments, "afterSequence")) : 0;
    const auto revision = facade_.query.snapshot().revision().value();
    if (since > revision || after > sequence_)
        throw InvalidInput("Event cursor is in the future");
    auto* result = out.object();
    out.set(result, "revision", out.number(revision));
    out.set(result, "sequence", out.number(sequence_));
    out.set(result, "resetRequired",
            out.boolean(!events_.empty() && events_.front().first > 1 &&
                        after < events_.front().first - 1 &&
                        since <= events_.front().second.result.afterRevision.value()));
    auto* list = out.array();
    constexpr std::array<std::string_view, 3> names{"revision", "history", "rejected"};
    for (const auto& [sequence, event] : events_) {
        if (sequence <= after || event.result.afterRevision.value() < since)
            continue;
        auto* value = commandResult(out, event.result);
        out.set(value, "sequence", out.number(sequence));
        out.set(value, "kind", out.text(names.at(static_cast<std::size_t>(event.kind))));
        out.append(list, value);
    }
    out.set(result, "events", list);
    return result;
}
} // namespace bloom::mcp
