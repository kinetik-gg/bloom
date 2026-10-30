// McpServerService's network side (ADR 0034, sections 3 and 4): the accept thread, one thread per
// connection (read with deadlines, parse, admit), and the request workers that run
// McpServerCore::handle. None of this runs on the owner (UI) thread; it records facts that pump()
// publishes.
#include "server_service_state.hpp"

#include <bloom/platform/owner_only_secret.hpp>

#include <algorithm>
#include <array>

namespace bloom::mcp {
namespace {

using namespace std::chrono_literals;
using service::Client;
using service::Exchange;
using service::Job;
using service::Network;
using service::SteadyClock;
using service::SystemClock;
using Fields = std::vector<std::pair<std::string_view, std::string>>;

std::string newSessionId() {
    std::array<std::byte, kMcpSessionIdBytes> random{};
    if (!platform::fillSecureRandom(random))
        return {};
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    for (const auto byte : random) {
        const auto value = std::to_integer<unsigned>(byte);
        text.push_back(digits[value >> 4U]);
        text.push_back(digits[value & 0xFU]);
    }
    return text;
}

std::string_view rejectionName(const McpHttpRejection rejection) {
    constexpr std::array<std::string_view, 15> names{"none",
                                                     "host not allowed",
                                                     "origin not allowed",
                                                     "not found",
                                                     "unauthorized",
                                                     "method not allowed",
                                                     "unsupported media type",
                                                     "not acceptable",
                                                     "missing session",
                                                     "unknown session",
                                                     "unsupported protocol version",
                                                     "batch not supported",
                                                     "payload too large",
                                                     "busy",
                                                     "stopping"};
    return names.at(static_cast<std::size_t>(rejection));
}

struct Reply {
    std::string bytes;
    bool close = false;
};

Reply rejectionReply(const McpHttpRejection rejection, std::string_view message = {},
                     const McpProtocolError code = McpProtocolError::InvalidRequest) {
    Fields fields;
    switch (rejection) {
    case McpHttpRejection::Unauthorized:
        fields.emplace_back("WWW-Authenticate", R"(Bearer realm="bloom")");
        break;
    case McpHttpRejection::MethodNotAllowed:
        fields.emplace_back("Allow", "POST, DELETE");
        break;
    case McpHttpRejection::Busy:
        fields.emplace_back("Retry-After", "1");
        break;
    default:
        break;
    }
    fields.emplace_back("Connection", "close");
    if (message.empty()) {
        fields.emplace_back("Content-Length", "0");
        return {httpResponse(httpStatus(rejection), fields, std::nullopt), true};
    }
    fields.emplace_back("Content-Type", "application/json");
    return {httpResponse(httpStatus(rejection), fields,
                         McpServerCore::errorResponse(std::nullopt, code, message)),
            true};
}

Reply statusReply(const int status) {
    const Fields fields{{"Connection", "close"}, {"Content-Length", "0"}};
    return {httpResponse(status, fields, std::nullopt), true};
}

std::string progressNotification(const std::string& token, const McpProgress& progress) {
    // The token is the request's own raw JSON value (a string or an integer).
    std::string text = R"({"jsonrpc":"2.0","method":"notifications/progress","params":{)";
    text += R"("progressToken":)" + token;
    text += R"(,"progress":)" + std::to_string(progress.progress);
    if (progress.total)
        text += R"(,"total":)" + std::to_string(*progress.total);
    if (!progress.message.empty()) {
        std::string escaped;
        for (const char c : progress.message) {
            if (c == '"' || c == '\\')
                escaped.push_back('\\');
            if (static_cast<unsigned char>(c) >= 0x20U)
                escaped.push_back(c);
        }
        text += R"(,"message":")" + escaped + "\"";
    }
    text += "}}";
    return text;
}

bool writeAll(platform::LoopbackConnection& connection, const std::string_view bytes,
              const McpHttpLimits& limits, const std::stop_token& stop) {
    return connection.writeAll(std::as_bytes(std::span(bytes.data(), bytes.size())),
                               limits.writeTimeout, stop) == platform::LoopbackIoStatus::Ok;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Connections

namespace {

class ConnectionWorker final {
  public:
    ConnectionWorker(McpServerService::State& state, Network& network,
                     std::shared_ptr<platform::LoopbackConnection> connection)
        : state_(state), network_(network), connection_(std::move(connection)),
          limits_(state.dependencies.limits.http) {}

    void run() {
        McpHttpRequestParser parser(limits_, kMcpMessageLimits.requestBytes);
        std::array<std::byte, 64 * 1024> buffer{};
        const auto stop = network_.stop.get_token();
        while (!stop.stop_requested()) {
            auto status = parser.feed({});
            auto started = SteadyClock::now();
            auto bodyStarted = started;
            bool wasReadingBody = false;
            while (status == McpHttpParseStatus::NeedMore) {
                const bool idle = parser.idle();
                if (parser.readingBody() && !wasReadingBody) {
                    wasReadingBody = true;
                    bodyStarted = SteadyClock::now();
                }
                const auto deadline = idle             ? SteadyClock::now() + limits_.keepAliveIdle
                                      : wasReadingBody ? bodyStarted + limits_.bodyTimeout
                                                       : started + limits_.headerTimeout;
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - SteadyClock::now());
                if (remaining <= 0ms)
                    return;
                const auto read = connection_->read(buffer, remaining, stop);
                if (read.status != platform::LoopbackIoStatus::Ok || read.bytes == 0)
                    return;
                if (idle)
                    started = SteadyClock::now();
                status = parser.feed(
                    std::span(reinterpret_cast<const char*>(buffer.data()), read.bytes));
            }
            if (status != McpHttpParseStatus::Complete) {
                respondParseFailure(status, stop);
                return;
            }
            const auto request = parser.take();
            if (!process(request, stop))
                return;
            // The client asked to close after this exchange.
            if (const auto connection = request.header("connection");
                connection && (*connection == "close" || *connection == "Close"))
                return;
        }
    }

  private:
    void respondParseFailure(const McpHttpParseStatus status, const std::stop_token& stop) {
        Reply reply;
        switch (status) {
        case McpHttpParseStatus::HeaderTooLarge:
        case McpHttpParseStatus::TooManyHeaders:
            reply = statusReply(431);
            break;
        case McpHttpParseStatus::BodyTooLarge:
            reject(McpHttpRejection::PayloadTooLarge, {});
            reply = rejectionReply(McpHttpRejection::PayloadTooLarge, "Request exceeds 1 MiB");
            break;
        case McpHttpParseStatus::UnsupportedVersion:
            reply = statusReply(505);
            break;
        case McpHttpParseStatus::UnsupportedTransferEncoding:
            reply = statusReply(501);
            break;
        case McpHttpParseStatus::LengthRequired:
            reply = statusReply(411);
            break;
        default:
            reply = statusReply(400);
            break;
        }
        static_cast<void>(writeAll(*connection_, reply.bytes, limits_, stop));
    }

    void reject(const McpHttpRejection rejection, const std::shared_ptr<Client>& client) {
        McpActivityEntry entry;
        entry.kind = McpActivityKind::RequestRejected;
        entry.rejection = rejection;
        if (client) {
            entry.client = client->id;
            entry.clientLabel = McpServerService::State::clientLabel(client->info);
        }
        entry.summary = "Request rejected (" + std::string(rejectionName(rejection)) + ")";
        state_.recordActivity(std::move(entry));
    }

    // Answers one request. False closes the connection.
    bool process(const McpHttpRequest& request, const std::stop_token& stop) {
        std::string token;
        {
            const std::lock_guard lock(state_.tokenMutex);
            token = state_.token;
        }
        const auto rejection = admitRequest(request, {.port = network_.port, .allowedOrigins = {}},
                                            token.empty() ? std::string_view("\x01") : token);
        if (rejection != McpHttpRejection::None) {
            reject(rejection, {});
            send(rejectionReply(rejection, rejection == McpHttpRejection::BatchNotSupported
                                               ? "Batches are not supported"
                                               : std::string_view{}),
                 stop);
            return false;
        }
        const auto sessionId = request.header("mcp-session-id");
        std::shared_ptr<Client> client;
        if (sessionId) {
            const std::lock_guard lock(state_.factsMutex);
            if (const auto found = state_.sessions.find(std::string(*sessionId));
                found != state_.sessions.end())
                client = found->second;
        }
        if (request.method == McpHttpMethod::Delete) {
            if (!sessionId)
                return refuse(McpHttpRejection::MissingSession, client, stop);
            if (!client)
                return refuse(McpHttpRejection::UnknownSession, client, stop);
            {
                const std::lock_guard lock(state_.factsMutex);
                state_.dropClientLocked(client->sessionId, McpDisconnectCause::Ended);
            }
            state_.wake();
            return send({httpResponse(204, Fields{}, std::nullopt), false}, stop);
        }
        const auto envelope = inspectMessage(request.body);
        if (envelope.isInitialize() && envelope.kind == McpMessageKind::Request)
            return initialize(request, stop);
        if (!sessionId)
            return refuse(McpHttpRejection::MissingSession, client, stop);
        if (!client)
            return refuse(McpHttpRejection::UnknownSession, client, stop);
        if (const auto version = request.header("mcp-protocol-version");
            version && *version != kMcpProtocolVersion)
            return refuse(McpHttpRejection::UnsupportedProtocolVersion, client, stop);
        touch(*client);
        if (envelope.kind == McpMessageKind::Notification &&
            envelope.method == "notifications/cancelled") {
            // Scoped to the caller's own session.
            const std::lock_guard lock(state_.factsMutex);
            if (envelope.cancels) {
                if (const auto found = client->active.find(*envelope.cancels);
                    found != client->active.end())
                    found->second->request_stop();
            }
        } else if (envelope.kind != McpMessageKind::Request) {
            static_cast<void>(state_.core->handle(*client->protocol, request.body, {}));
        }
        if (envelope.kind != McpMessageKind::Request && envelope.kind != McpMessageKind::Invalid)
            return send({httpResponse(202, Fields{{"Content-Length", "0"}}, std::nullopt), false},
                        stop);
        const bool stream = envelope.tool &&
                            toolExecution(*envelope.tool) == McpToolExecution::Task &&
                            envelope.progressToken.has_value();
        return execute(client, request.body, envelope, stream, {}, stop);
    }

    bool refuse(const McpHttpRejection rejection, const std::shared_ptr<Client>& client,
                const std::stop_token& stop) {
        reject(rejection, client);
        send(rejectionReply(rejection, rejection == McpHttpRejection::UnknownSession
                                           ? "Session not found; initialize again"
                                           : std::string_view{}),
             stop);
        return false;
    }

    void touch(Client& client) {
        const std::lock_guard lock(state_.factsMutex);
        client.lastActivity = SteadyClock::now();
        client.lastActivityAt = SystemClock::now();
        state_.dirty = true;
    }

    bool initialize(const McpHttpRequest& request, const std::stop_token& stop) {
        const auto& limits = state_.dependencies.limits;
        {
            const std::lock_guard lock(state_.factsMutex);
            if (state_.sessions.size() >= limits.clients) {
                // Evict the least recently used client that is idle and has nothing in flight.
                std::shared_ptr<Client> oldest;
                const auto now = SteadyClock::now();
                for (const auto& [id, candidate] : state_.sessions) {
                    if (candidate->inFlight == 0 &&
                        now - candidate->lastActivity >= limits.clientEvictionIdle &&
                        (!oldest || candidate->lastActivity < oldest->lastActivity))
                        oldest = candidate;
                }
                if (!oldest) {
                    McpActivityEntry entry;
                    entry.kind = McpActivityKind::RequestRejected;
                    entry.rejection = McpHttpRejection::Busy;
                    entry.summary = "Client refused (client limit)";
                    entry.id = state_.nextActivity++;
                    entry.at = SystemClock::now();
                    state_.activityRing.push_back(std::move(entry));
                    state_.dirty = true;
                    clientLimitReached_ = true;
                } else {
                    state_.dropClientLocked(oldest->sessionId, McpDisconnectCause::Evicted);
                }
            }
        }
        if (clientLimitReached_) {
            clientLimitReached_ = false;
            state_.wake();
            send(rejectionReply(McpHttpRejection::Busy, "Bloom accepts at most 8 MCP clients.",
                                McpProtocolError::ClientLimitReached),
                 stop);
            return false;
        }
        auto client = std::make_shared<Client>();
        client->protocol = std::shared_ptr<McpProtocolSession>(state_.core->openSession());
        client->lastActivity = SteadyClock::now();
        client->connectedAt = client->lastActivityAt = SystemClock::now();
        const auto envelope = inspectMessage(request.body);
        return execute(client, request.body, envelope, false, newSessionId(), stop);
    }

    // Runs one request on the worker pool and answers it. `newSession` registers the client when
    // the request completes initialize.
    bool execute(const std::shared_ptr<Client>& client, const std::string& body,
                 const McpEnvelope& envelope, const bool stream, const std::string& newSession,
                 const std::stop_token& stop) {
        const auto& limits = state_.dependencies.limits;
        auto exchange = std::make_shared<Exchange>();
        auto cancel = std::make_shared<std::stop_source>();
        {
            const std::lock_guard facts(state_.factsMutex);
            if (client->inFlight >= limits.inFlightPerClient)
                busy_ = true;
        }
        if (busy_) {
            busy_ = false;
            reject(McpHttpRejection::Busy, client);
            send(rejectionReply(McpHttpRejection::Busy, "Too many requests from this client",
                                McpProtocolError::ServerBusy),
                 stop);
            return false;
        }
        {
            const std::lock_guard lock(network_.mutex);
            if (network_.stop.stop_requested()) {
                send(rejectionReply(McpHttpRejection::Stopping, "The server is stopping",
                                    McpProtocolError::ShuttingDown),
                     stop);
                return false;
            }
            if (network_.pending.size() >= kMcpMessageLimits.pendingMessages)
                busy_ = true;
            else
                network_.pending.push_back(Job{.client = client,
                                               .body = body,
                                               .stop = cancel,
                                               .streamProgress = stream,
                                               .exchange = exchange});
        }
        if (busy_) {
            busy_ = false;
            reject(McpHttpRejection::Busy, client);
            send(rejectionReply(McpHttpRejection::Busy, "Bloom is busy; retry shortly",
                                McpProtocolError::ServerBusy),
                 stop);
            return false;
        }
        {
            const std::lock_guard facts(state_.factsMutex);
            ++client->inFlight;
            ++state_.totalRequests;
            if (envelope.requestKey)
                client->active[*envelope.requestKey] = cancel;
            state_.dirty = true;
        }
        network_.changed.notify_all();
        state_.wake();

        // A client disconnect is not cancellation: the request finishes and its answer is
        // dropped when the write fails.
        bool streaming = false;
        std::unique_lock lock(exchange->mutex);
        while (true) {
            exchange->changed.wait(lock,
                                   [&] { return exchange->done || !exchange->progress.empty(); });
            while (!exchange->progress.empty()) {
                const auto progress = std::move(exchange->progress.front());
                exchange->progress.pop_front();
                lock.unlock();
                if (!streaming) {
                    streaming = true;
                    const Fields fields{{"Content-Type", "text/event-stream"},
                                        {"Cache-Control", "no-cache"},
                                        {"Connection", "close"}};
                    writeOk_ = writeAll(*connection_, httpResponse(200, fields, std::nullopt),
                                        limits_, {});
                }
                if (writeOk_)
                    writeOk_ = writeAll(
                        *connection_,
                        sseMessage(progressNotification(*envelope.progressToken, progress)),
                        limits_, {});
                lock.lock();
            }
            if (exchange->done)
                break;
        }
        const auto handled = exchange->handled;
        lock.unlock();
        if (!newSession.empty() && handled.initializedClient)
            registerClient(client, newSession);
        if (streaming) {
            if (writeOk_ && handled.response)
                static_cast<void>(
                    writeAll(*connection_, sseMessage(*handled.response), limits_, {}));
            connection_->shutdownWrite();
            return false;
        }
        if (!handled.response)
            return send({httpResponse(202, Fields{{"Content-Length", "0"}}, std::nullopt), false},
                        stop);
        Fields fields{{"Content-Type", "application/json"}, {"Cache-Control", "no-store"}};
        if (!newSession.empty() && handled.initializedClient)
            fields.emplace_back("Mcp-Session-Id", newSession);
        return send({httpResponse(200, fields, *handled.response), false}, stop);
    }

    void registerClient(const std::shared_ptr<Client>& client, const std::string& sessionId) {
        {
            const std::lock_guard lock(state_.factsMutex);
            client->id = state_.nextClient++;
            client->sessionId = sessionId;
            if (const auto info = client->protocol->client())
                client->info = *info;
            state_.sessions[sessionId] = client;
            state_.pendingEvents.emplace_back(McpClientConnected{state_.view(*client)});
            state_.dirty = true;
        }
        McpActivityEntry entry;
        entry.kind = McpActivityKind::ClientConnected;
        entry.client = client->id;
        entry.clientLabel = McpServerService::State::clientLabel(client->info);
        entry.summary = "Client connected";
        state_.recordActivity(std::move(entry));
    }

    bool send(const Reply& reply, const std::stop_token& stop) {
        return writeAll(*connection_, reply.bytes, limits_, stop) && !reply.close;
    }

    McpServerService::State& state_;
    Network& network_;
    std::shared_ptr<platform::LoopbackConnection> connection_;
    McpHttpLimits limits_;
    bool busy_ = false;
    bool clientLimitReached_ = false;
    bool writeOk_ = true;
};

} // namespace

// ---------------------------------------------------------------------------------------------
// Lifecycle

void McpServerService::State::startNetwork(std::unique_ptr<platform::LoopbackListener> listener) {
    network = std::make_unique<Network>();
    auto& net = *network;
    net.port = listener->port();
    net.listener = std::move(listener);
    const auto& limits = dependencies.limits;
    const auto stop = net.stop.get_token();
    // Request workers.
    for (std::size_t index = 0; index < limits.requestWorkers; ++index) {
        ++net.threads;
        net.workers.emplace_back([this, &net, stop] {
            while (true) {
                Job job;
                {
                    std::unique_lock lock(net.mutex);
                    net.changed.wait(lock,
                                     [&] { return stop.stop_requested() || !net.pending.empty(); });
                    if (net.pending.empty())
                        break;
                    job = std::move(net.pending.front());
                    net.pending.pop_front();
                    ++net.executing;
                }
                {
                    const std::lock_guard facts(factsMutex);
                    ++activeRequests;
                    dirty = true;
                }
                wake();
                McpRequestControl control;
                control.stop = job.stop->get_token();
                if (job.streamProgress) {
                    control.progress = [exchange = job.exchange](const McpProgress& progress) {
                        {
                            const std::lock_guard lock(exchange->mutex);
                            exchange->progress.push_back(progress);
                        }
                        exchange->changed.notify_all();
                    };
                }
                const auto started = SteadyClock::now();
                McpHandled handled;
                try {
                    handled = core->handle(*job.client->protocol, job.body, control);
                } catch (const std::exception& error) {
                    handled.response =
                        McpServerCore::errorResponse(inspectMessage(job.body).requestKey,
                                                     McpProtocolError::InternalError, error.what());
                    handled.outcome = McpCallOutcome::ProtocolError;
                }
                const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    SteadyClock::now() - started);
                const auto key = inspectMessage(job.body).requestKey;
                {
                    const std::lock_guard facts(factsMutex);
                    --activeRequests;
                    auto& client = *job.client;
                    if (client.inFlight > 0)
                        --client.inFlight;
                    if (key)
                        client.active.erase(*key);
                    ++client.requests;
                    client.lastActivity = SteadyClock::now();
                    client.lastActivityAt = SystemClock::now();
                    if (handled.tool)
                        client.lastTool = handled.tool;
                    dirty = true;
                }
                if (handled.tool) {
                    McpActivityEntry entry;
                    entry.kind = McpActivityKind::ToolCall;
                    entry.client = job.client->id;
                    entry.clientLabel = clientLabel(job.client->info);
                    entry.tool = handled.tool;
                    entry.outcome = handled.outcome;
                    entry.toolError = handled.toolError;
                    entry.duration = duration;
                    entry.summary = handled.summary;
                    entry.compileFailures = handled.compileFailures;
                    recordActivity(std::move(entry));
                }
                {
                    const std::lock_guard lock(job.exchange->mutex);
                    job.exchange->handled = std::move(handled);
                    job.exchange->done = true;
                }
                job.exchange->changed.notify_all();
                {
                    const std::lock_guard lock(net.mutex);
                    --net.executing;
                }
                wake();
            }
            --net.threads;
            wake();
        });
    }
    // The accept thread.
    ++net.threads;
    net.acceptor = std::jthread([this, &net, stop] {
        while (!stop.stop_requested()) {
            auto accepted = net.listener->accept(500ms, stop);
            if (accepted.status == platform::LoopbackAcceptStatus::Failed) {
                if (!stop.stop_requested())
                    net.failed = true;
                break;
            }
            if (accepted.status == platform::LoopbackAcceptStatus::Stopped)
                break;
            {
                // Reap finished connection threads (joining a finished thread does not block).
                const std::lock_guard lock(net.mutex);
                std::erase_if(net.connectionThreads,
                              [](const auto& entry) { return entry.first->load(); });
            }
            if (accepted.status != platform::LoopbackAcceptStatus::Accepted || !accepted.connection)
                continue;
            auto connection =
                std::make_shared<platform::LoopbackConnection>(std::move(*accepted.connection));
            bool busy = false;
            {
                const std::lock_guard facts(factsMutex);
                if (openConnections >= dependencies.limits.connections)
                    busy = true;
                else
                    ++openConnections;
            }
            if (busy) {
                McpActivityEntry entry;
                entry.kind = McpActivityKind::RequestRejected;
                entry.rejection = McpHttpRejection::Busy;
                entry.summary = "Connection refused (connection limit)";
                recordActivity(std::move(entry));
                const auto reply =
                    rejectionReply(McpHttpRejection::Busy, "Bloom accepts at most 16 connections",
                                   McpProtocolError::ServerBusy);
                static_cast<void>(connection->writeAll(
                    std::as_bytes(std::span(reply.bytes.data(), reply.bytes.size())), 1s, stop));
                connection->shutdownWrite();
                continue;
            }
            const std::lock_guard lock(net.mutex);
            net.connections.push_back(connection);
            ++net.threads;
            auto done = std::make_shared<std::atomic_bool>(false);
            net.connectionThreads.emplace_back(done, std::jthread([this, &net, connection, done] {
                                                   ConnectionWorker(*this, net, connection).run();
                                                   connection->close();
                                                   {
                                                       const std::lock_guard guard(net.mutex);
                                                       std::erase(net.connections, connection);
                                                   }
                                                   {
                                                       const std::lock_guard facts(factsMutex);
                                                       if (openConnections > 0)
                                                           --openConnections;
                                                   }
                                                   done->store(true);
                                                   --net.threads;
                                                   wake();
                                               }));
        }
        --net.threads;
        wake();
    });
}

void McpServerService::State::stopNetwork() {
    if (!network)
        return;
    auto& net = *network;
    net.stop.request_stop();
    net.listener->close();
    std::deque<Job> pending;
    {
        const std::lock_guard lock(net.mutex);
        for (const auto& connection : net.connections)
            connection->close();
        pending.swap(net.pending);
    }
    net.changed.notify_all();
    // Requests that never started are answered as cancelled; the ones running see their stop
    // token (queued commits are withdrawn; a commit already applying finishes).
    for (auto& job : pending) {
        {
            const std::lock_guard lock(job.exchange->mutex);
            job.exchange->handled.response =
                McpServerCore::errorResponse(inspectMessage(job.body).requestKey,
                                             McpProtocolError::ShuttingDown, "The server stopped");
            job.exchange->done = true;
        }
        job.exchange->changed.notify_all();
        const std::lock_guard facts(factsMutex);
        if (job.client->inFlight > 0)
            --job.client->inFlight;
    }
    {
        const std::lock_guard facts(factsMutex);
        std::vector<std::string> ids;
        for (const auto& [id, client] : sessions)
            ids.push_back(id);
        for (const auto& id : ids)
            dropClientLocked(id, McpDisconnectCause::ServerStopped);
        dirty = true;
    }
    wake();
}

bool McpServerService::State::networkQuiescent() const {
    return !network || network->threads.load() == 0;
}

} // namespace bloom::mcp
