#include "transport.hpp"
#include "server.hpp"

#include <condition_variable>
#include <iostream>
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
    std::condition_variable ready, space;
    std::deque<Message> queue;
    bool closed = false;
};
} // namespace
void serveStdio(Server& server) {
    Inbox inbox;
    std::jthread reader([&] {
        const auto send = [&](Message message) {
            std::unique_lock lock(inbox.mutex);
            inbox.space.wait(lock, [&] { return inbox.queue.size() < 16; });
            inbox.queue.push_back(std::move(message));
            inbox.ready.notify_one();
        };
        Message message;
        char byte = 0;
        while (std::cin.get(byte)) {
            if (byte == '\n') {
                if (message.oversized || !server.interceptCancellation(message.text))
                    send(std::move(message));
                message = {};
            } else if (message.text.size() < kRequestLimit)
                message.text.push_back(byte);
            else
                message.oversized = true;
        }
        if (!message.text.empty() || message.oversized) {
            message.unterminated = true;
            send(std::move(message));
        }
        const std::lock_guard lock(inbox.mutex);
        inbox.closed = true;
        inbox.ready.notify_one();
    });
    while (true) {
        Message message;
        {
            std::unique_lock lock(inbox.mutex);
            inbox.ready.wait(lock, [&] { return inbox.closed || !inbox.queue.empty(); });
            if (inbox.queue.empty())
                break;
            message = std::move(inbox.queue.front());
            inbox.queue.pop_front();
            inbox.space.notify_one();
        }
        const auto result =
            message.oversized
                ? std::optional(Server::error(nullptr, -32600, "Request exceeds 1 MiB"))
            : message.unterminated
                ? std::optional(Server::error(nullptr, -32700, "Unterminated stdio message"))
                : server.handle(message.text);
        if (result)
            std::cout << *result << '\n' << std::flush;
    }
}
} // namespace bloom::mcp
