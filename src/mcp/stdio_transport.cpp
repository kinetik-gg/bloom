// The stdio transport (newline-delimited UTF-8 JSON-RPC), moved from apps/bloom-mcp/transport.cpp:
// a bounded reader thread, the 16-message inbox, cancellation intercepted while a call runs, and
// one protocol session. Cancellation now stops only the cancelled request's own work.
#include <bloom/mcp/server_core.hpp>

#include <condition_variable>
#include <deque>
#include <istream>
#include <mutex>
#include <ostream>
#include <thread>

namespace bloom::mcp {
namespace {

struct Message {
    std::string text;
    bool oversized = false;
    bool unterminated = false;
};

struct Inbox {
    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable space;
    std::deque<Message> queue;
    bool closed = false;
    // The request handle() is running, and its stop source.
    std::optional<std::string> active;
    std::stop_source cancel;
};

} // namespace

void serveStdio(McpServerCore& core, std::istream& input, std::ostream& output,
                const std::stop_token& stop) {
    const auto limits = kMcpMessageLimits;
    Inbox inbox;
    auto session = core.openSession();
    std::jthread reader([&] {
        const auto send = [&](Message message) {
            std::unique_lock lock(inbox.mutex);
            inbox.space.wait(lock, [&] { return inbox.queue.size() < limits.pendingMessages; });
            inbox.queue.push_back(std::move(message));
            inbox.ready.notify_one();
        };
        // notifications/cancelled is handled here, while the call it cancels may be running.
        const auto intercept = [&](const std::string& text) {
            const auto envelope = inspectMessage(text, limits);
            if (envelope.kind != McpMessageKind::Notification ||
                envelope.method != "notifications/cancelled")
                return false;
            const std::lock_guard lock(inbox.mutex);
            if (envelope.cancels && inbox.active == envelope.cancels)
                inbox.cancel.request_stop();
            return true;
        };
        Message message;
        char byte = 0;
        while (input.get(byte)) {
            if (byte == '\n') {
                if (message.oversized || !intercept(message.text))
                    send(std::move(message));
                message = {};
            } else if (message.text.size() < limits.requestBytes) {
                message.text.push_back(byte);
            } else {
                message.oversized = true;
            }
        }
        if (!message.text.empty() || message.oversized) {
            message.unterminated = true;
            send(std::move(message));
        }
        const std::lock_guard lock(inbox.mutex);
        inbox.closed = true;
        inbox.ready.notify_one();
    });
    while (!stop.stop_requested()) {
        Message message;
        McpRequestControl control;
        {
            std::unique_lock lock(inbox.mutex);
            inbox.ready.wait(lock, [&] { return inbox.closed || !inbox.queue.empty(); });
            if (inbox.queue.empty())
                break;
            message = std::move(inbox.queue.front());
            inbox.queue.pop_front();
            inbox.space.notify_one();
            if (!message.oversized && !message.unterminated) {
                inbox.cancel = std::stop_source();
                inbox.active = inspectMessage(message.text, limits).requestKey;
                control.stop = inbox.cancel.get_token();
            }
        }
        std::optional<std::string> response;
        if (message.oversized)
            response = McpServerCore::errorResponse(std::nullopt, McpProtocolError::InvalidRequest,
                                                    "Request exceeds 1 MiB");
        else if (message.unterminated)
            response = McpServerCore::errorResponse(std::nullopt, McpProtocolError::ParseError,
                                                    "Unterminated stdio message");
        else
            response = core.handle(*session, message.text, control).response;
        {
            const std::lock_guard lock(inbox.mutex);
            inbox.active.reset();
        }
        if (response)
            output << *response << '\n' << std::flush;
    }
}

} // namespace bloom::mcp
