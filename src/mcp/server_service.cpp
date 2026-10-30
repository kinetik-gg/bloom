// McpServerService: lifecycle, commands, snapshot, activity, events, token and client
// configuration (ADR 0034, sections 3, 6 and 7). The network threads live in server_http.cpp.
#include "server_service_state.hpp"

#include <bloom/platform/owner_only_secret.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/live_session_host.hpp>

#include <algorithm>
#include <array>

namespace bloom::mcp {
namespace {

using service::SystemClock;

std::string portText(const std::uint32_t port) { return "Port " + std::to_string(port); }

McpCommandAvailability available() {
    McpCommandAvailability result;
    result.available = true;
    result.code = McpUnavailableCode::Available;
    return result;
}

McpCommandAvailability refusal(const McpUnavailableCode code, const McpField field,
                               std::string reason, std::optional<std::uint32_t> port = {}) {
    McpCommandAvailability result;
    result.code = code;
    result.field = field;
    result.reason = std::move(reason);
    result.port = port;
    return result;
}

McpCommandAvailability shuttingDown() {
    return refusal(McpUnavailableCode::ShuttingDown, McpField::Status, "Bloom is closing.");
}

McpEndpoint endpointFor(const std::uint16_t port) {
    McpEndpoint endpoint;
    endpoint.port = port;
    endpoint.url = "http://" + endpoint.host + ":" + std::to_string(port) + endpoint.path;
    return endpoint;
}

// The bind refusal as a command refusal and the failure the status line keeps.
McpCommandAvailability bindRefusal(const platform::LoopbackBindResult& bind) {
    const auto port = bind.port;
    switch (bind.status) {
    case platform::LoopbackBindStatus::PortInUse:
        return refusal(
            McpUnavailableCode::PortInUse, McpField::Port,
            portText(port) + " is in use. Choose another port or stop the other program.", port);
    case platform::LoopbackBindStatus::PermissionDenied:
    case platform::LoopbackBindStatus::InvalidPort:
        return refusal(McpUnavailableCode::PermissionDenied, McpField::Port,
                       portText(port) + " isn't allowed on this computer. Choose another port.",
                       port);
    case platform::LoopbackBindStatus::AddressUnavailable:
        return refusal(McpUnavailableCode::AddressUnavailable, McpField::Status,
                       "This computer has no local network address. Check its network settings.",
                       port);
    case platform::LoopbackBindStatus::ResourceExhausted:
        return refusal(McpUnavailableCode::ResourceExhausted, McpField::Status,
                       "The computer is out of network resources. Close some programs and retry.",
                       port);
    case platform::LoopbackBindStatus::Bound:
    case platform::LoopbackBindStatus::SystemError:
        break;
    }
    return refusal(McpUnavailableCode::SystemError, McpField::Status,
                   "The server couldn't start on " + portText(port) + ". Try again.", port);
}

McpServerFailureCode failureCode(const McpUnavailableCode code) {
    switch (code) {
    case McpUnavailableCode::PortInUse:
        return McpServerFailureCode::PortInUse;
    case McpUnavailableCode::PermissionDenied:
        return McpServerFailureCode::PermissionDenied;
    case McpUnavailableCode::AddressUnavailable:
        return McpServerFailureCode::AddressUnavailable;
    case McpUnavailableCode::ResourceExhausted:
        return McpServerFailureCode::ResourceExhausted;
    default:
        return McpServerFailureCode::SystemError;
    }
}

std::string newToken() {
    std::array<std::byte, kMcpTokenRandomBytes> random{};
    if (!platform::fillSecureRandom(random))
        return {};
    return std::string(kMcpTokenPrefix) + base64Url(random);
}

// The task owner of the token's blocking-I/O work (ADR 0034).
runtime::TaskOwner tokenOwner() noexcept {
    return {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(34)};
}

constexpr std::string_view kNoTokenFolder =
    "Bloom can't find a folder to keep the access token in.";
constexpr std::string_view kTokenNotSaved = "Bloom can't save the access token in its folder.";
constexpr std::string_view kNoRandom = "Bloom can't create an access token on this computer.";

// Blocking-I/O work: read the token, or create (or replace an exposed or malformed) one.
void loadToken(const std::filesystem::path& file, const bool regenerate, service::TokenTask& task) {
    std::string token;
    std::string reason;
    if (!regenerate) {
        const auto read = platform::readOwnerOnlySecret(file, 256);
        if (read.status == platform::OwnerOnlySecretStatus::Ok && tokenWellFormed(read.value))
            token = read.value;
    }
    if (token.empty()) {
        token = newToken();
        if (token.empty())
            reason = kNoRandom;
        else if (platform::writeOwnerOnlySecret(file, token).status !=
                 platform::OwnerOnlySecretStatus::Ok) {
            token.clear();
            reason = kTokenNotSaved;
        }
    }
    const std::lock_guard lock(task.mutex);
    task.token = std::move(token);
    task.reason = std::move(reason);
    task.state = task.token.empty() ? McpTokenState::Unavailable : McpTokenState::Ready;
    task.done = true;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Pure

std::string clientConfigText(const McpClientKind kind, const McpEndpoint& endpoint,
                             const std::string_view credential) {
    const auto url = endpoint.url.empty() ? endpointFor(endpoint.port).url : endpoint.url;
    const auto header = "Bearer " + std::string(credential);
    switch (kind) {
    case McpClientKind::ClaudeCode:
        return "claude mcp add --transport http bloom " + url +
               " --header \"Authorization: " + header + "\"";
    case McpClientKind::McpJson:
        return R"({"mcpServers":{"bloom":{"type":"http","url":")" + url +
               R"(","headers":{"Authorization":")" + header + R"("}}}})";
    case McpClientKind::VsCode:
        return R"({"servers":{"bloom":{"type":"http","url":")" + url +
               R"(","headers":{"Authorization":")" + header + R"("}}}})";
    case McpClientKind::Generic:
        break;
    }
    return url + "\nAuthorization: " + header;
}

// ---------------------------------------------------------------------------------------------
// Subscription

McpServerSubscription::McpServerSubscription(std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}
McpServerSubscription::McpServerSubscription(McpServerSubscription&&) noexcept = default;
McpServerSubscription& McpServerSubscription::operator=(McpServerSubscription&&) noexcept = default;
McpServerSubscription::~McpServerSubscription() = default;

std::uint64_t McpServerSubscription::serial() const noexcept {
    if (!state_)
        return 0;
    const std::lock_guard lock(state_->mutex);
    return state_->serial;
}

std::optional<std::uint64_t>
McpServerSubscription::waitForChange(const std::uint64_t observed,
                                     const std::stop_token& stop) const {
    if (!state_)
        return std::nullopt;
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock, stop, [&] { return state_->closed || state_->serial != observed; });
    if (state_->closed || stop.stop_requested())
        return std::nullopt;
    return state_->serial;
}

void McpServerSubscription::close() noexcept {
    if (!state_)
        return;
    {
        const std::lock_guard lock(state_->mutex);
        state_->closed = true;
    }
    state_->changed.notify_all();
}

// ---------------------------------------------------------------------------------------------
// State helpers

void McpServerService::State::wake() {
    const std::lock_guard lock(wakeMutex);
    std::erase_if(subscribers, [](const auto& subscriber) {
        const std::lock_guard guard(subscriber->mutex);
        return subscriber->closed;
    });
    for (const auto& subscriber : subscribers) {
        {
            const std::lock_guard guard(subscriber->mutex);
            ++subscriber->serial;
        }
        subscriber->changed.notify_all();
    }
}

std::string McpServerService::State::clientLabel(const McpClientInfo& info) {
    std::string label = info.title.empty() ? info.name : info.title;
    if (label.size() > 64) {
        auto end = std::size_t{64};
        while (end > 0 && (static_cast<unsigned char>(label[end]) & 0xc0U) == 0x80U)
            --end;
        label.resize(end);
    }
    return label;
}

McpClientView McpServerService::State::view(const service::Client& client) const {
    McpClientView view;
    view.id = client.id;
    view.info = client.info;
    view.connectedAt = client.connectedAt;
    view.lastActivityAt = client.lastActivityAt;
    view.inFlight = client.inFlight;
    view.requests = client.requests;
    view.lastTool = client.lastTool;
    return view;
}

void McpServerService::State::recordActivity(McpActivityEntry entry) {
    {
        const std::lock_guard lock(factsMutex);
        entry.at = SystemClock::now();
        if (entry.summary.size() > 256)
            entry.summary.resize(256);
        // Identical rejections within one second fold into one entry.
        if (entry.kind == McpActivityKind::RequestRejected && !activityRing.empty()) {
            auto& last = activityRing.back();
            if (last.kind == McpActivityKind::RequestRejected &&
                last.rejection == entry.rejection && last.clientLabel == entry.clientLabel &&
                entry.at - last.at < std::chrono::seconds(1)) {
                ++last.repeat;
                last.at = entry.at;
                dirty = true;
                return;
            }
        }
        entry.id = nextActivity++;
        activityRing.push_back(std::move(entry));
        while (activityRing.size() > dependencies.limits.activityEntries) {
            activityRing.pop_front();
            activityGap = true;
        }
        dirty = true;
    }
    wake();
}

void McpServerService::State::recordEvent(McpServerEventPayload payload) {
    {
        const std::lock_guard lock(factsMutex);
        pendingEvents.push_back(std::move(payload));
        dirty = true;
    }
    wake();
}

void McpServerService::State::dropClientLocked(const std::string& sessionId,
                                               const McpDisconnectCause cause) {
    const auto found = sessions.find(sessionId);
    if (found == sessions.end())
        return;
    const auto client = found->second;
    sessions.erase(found);
    for (const auto& [key, stop] : client->active) {
        static_cast<void>(key);
        stop->request_stop();
    }
    pendingEvents.emplace_back(McpClientDisconnected{client->id, cause});
    McpActivityEntry entry;
    entry.id = nextActivity++;
    entry.at = SystemClock::now();
    entry.kind = McpActivityKind::ClientDisconnected;
    entry.client = client->id;
    entry.clientLabel = clientLabel(client->info);
    constexpr std::array<std::string_view, 5> causes{"ended", "expired", "evicted",
                                                     "token regenerated", "server stopped"};
    entry.summary =
        "Client disconnected (" + std::string(causes.at(static_cast<std::size_t>(cause))) + ")";
    activityRing.push_back(std::move(entry));
    while (activityRing.size() > dependencies.limits.activityEntries) {
        activityRing.pop_front();
        activityGap = true;
    }
    dirty = true;
}

bool McpServerService::State::runOnOwner(const McpExportAccess::Work& work,
                                         const std::stop_token& stop) {
    auto task = std::make_shared<service::OwnerTask>();
    task->work = work;
    {
        const std::lock_guard lock(ownerMutex);
        if (!ownerOpen)
            return false;
        ownerTasks.push_back(task);
    }
    wake();
    std::unique_lock lock(ownerMutex);
    while (!task->done && !task->withdrawn) {
        ownerChanged.wait(lock, stop, [&] { return task->done || task->withdrawn; });
        if (!task->done && !task->started && stop.stop_requested()) {
            std::erase(ownerTasks, task);
            return false;
        }
        if (!task->done && task->started)
            ownerChanged.wait(lock, [&] { return task->done; });
    }
    if (task->withdrawn && !task->done)
        return false;
    if (task->error)
        std::rethrow_exception(task->error);
    return true;
}

void McpServerService::State::runOwnerTasks() {
    host::ExportQueueService* queue = nullptr;
    bool queried = false;
    while (true) {
        std::shared_ptr<service::OwnerTask> task;
        {
            const std::lock_guard lock(ownerMutex);
            if (ownerTasks.empty())
                return;
            task = ownerTasks.front();
            ownerTasks.pop_front();
            task->started = true;
        }
        if (!queried) {
            queue = dependencies.exportQueue ? dependencies.exportQueue() : nullptr;
            queried = true;
        }
        try {
            task->work(queue, exportGeneration);
        } catch (...) {
            task->error = std::current_exception();
        }
        {
            const std::lock_guard lock(ownerMutex);
            task->done = true;
        }
        ownerChanged.notify_all();
    }
}

void McpServerService::State::withdrawOwnerTasks() {
    {
        const std::lock_guard lock(ownerMutex);
        for (const auto& task : ownerTasks)
            task->withdrawn = true;
        ownerTasks.clear();
    }
    ownerChanged.notify_all();
}

// ---------------------------------------------------------------------------------------------
// Service

McpServerService::McpServerService(McpServerServiceDependencies dependencies)
    : state_(std::make_shared<State>(std::move(dependencies))) {
    auto& s = *state_;
    s.preferences = s.dependencies.preferences;
    if (!portInRange(s.preferences.port))
        s.preferences.port = kMcpDefaultPort;
    std::weak_ptr<State> weak = state_;
    s.core = std::make_unique<McpServerCore>(McpCoreDependencies{
        .session = std::ref(s.dependencies.live),
        .exports = {.run =
                        [weak](const McpExportAccess::Work& work, const std::stop_token& stop) {
                            const auto state = weak.lock();
                            return state && state->runOnOwner(work, stop);
                        },
                    .callerIsOwner = false},
        .scheduler = s.dependencies.scheduler,
        .gpuProvider = nullptr,
        .captureDirectory = s.dependencies.captureDirectory,
        .ui = s.dependencies.liveUi,
        .serverVersion = s.dependencies.serverVersion,
        .limits = kMcpMessageLimits});
    // The token loads on the blocking-I/O executor; pump() publishes it.
    if (s.dependencies.tokenFile.empty()) {
        s.tokenState = McpTokenState::Unavailable;
        s.tokenReason = std::string(kNoTokenFolder);
    } else {
        s.tokenTask = std::make_shared<service::TokenTask>();
        auto submitted = s.dependencies.scheduler.submit<void>(
            runtime::TaskRequest("Load the MCP access token", tokenOwner(),
                                 runtime::TaskPriority::Foreground,
                                 runtime::TaskExecutor::BlockingIo),
            [task = s.tokenTask, file = s.dependencies.tokenFile,
             weak](runtime::TaskContext&) -> runtime::TaskResult<void> {
                loadToken(file, false, *task);
                if (const auto state = weak.lock())
                    state->wake();
                return runtime::TaskResult<void>::succeeded();
            });
        static_cast<void>(submitted);
    }
    pump();
}

McpServerService::~McpServerService() {
    auto& s = *state_;
    s.shuttingDown = true;
    s.withdrawOwnerTasks();
    {
        const std::lock_guard lock(s.ownerMutex);
        s.ownerOpen = false;
    }
    if (s.network) {
        s.stopNetwork();
        // Destruction is the one place that joins: every thread was told to stop.
        s.network.reset();
    }
    s.core.reset();
    const std::lock_guard lock(s.wakeMutex);
    for (const auto& subscriber : s.subscribers) {
        {
            const std::lock_guard guard(subscriber->mutex);
            subscriber->closed = true;
        }
        subscriber->changed.notify_all();
    }
}

std::shared_ptr<const McpServerSnapshot> McpServerService::snapshot() const {
    const std::lock_guard lock(state_->publishMutex);
    return state_->published;
}

McpServerEventBatch McpServerService::events(const std::uint64_t afterSequence) const {
    const std::lock_guard lock(state_->publishMutex);
    McpServerEventBatch batch;
    batch.lastSequence = state_->nextEvent - 1;
    if (!state_->eventRing.empty() && afterSequence + 1 < state_->eventRing.front().sequence)
        batch.gap = true;
    for (const auto& event : state_->eventRing) {
        if (event.sequence > afterSequence)
            batch.events.push_back(event);
    }
    return batch;
}

McpActivityBatch McpServerService::activity(const std::uint64_t afterId) const {
    const std::lock_guard lock(state_->factsMutex);
    McpActivityBatch batch;
    batch.lastId = state_->nextActivity - 1;
    if (!state_->activityRing.empty() && afterId + 1 < state_->activityRing.front().id)
        batch.gap = true;
    for (const auto& entry : state_->activityRing) {
        if (entry.id > afterId)
            batch.entries.push_back(entry);
    }
    return batch;
}

McpServerSubscription McpServerService::subscribe() {
    auto subscriber = std::make_shared<McpServerSubscription::State>();
    const std::lock_guard lock(state_->wakeMutex);
    state_->subscribers.push_back(subscriber);
    return McpServerSubscription(subscriber);
}

McpCommandAvailability McpServerService::availability(const McpServerCommand command) const {
    const auto published = snapshot();
    return published ? published->commands[command] : McpCommandAvailability{};
}

namespace {

McpCommandAvailabilitySet commandsFor(const McpServerService::State& s, const McpTokenState token,
                                      const std::string& tokenReason) {
    McpCommandAvailabilitySet set;
    const auto entry = [&](const McpServerCommand command) -> McpCommandAvailability& {
        return set.entries[static_cast<std::size_t>(command)];
    };
    const auto tokenRefusal = [&]() -> std::optional<McpCommandAvailability> {
        if (token == McpTokenState::Loading || token == McpTokenState::Regenerating)
            return refusal(McpUnavailableCode::TokenLoading, McpField::Token,
                           "The access token is loading.");
        if (token == McpTokenState::Unavailable)
            return refusal(McpUnavailableCode::TokenUnavailable, McpField::Token,
                           tokenReason.empty() ? std::string(kNoTokenFolder) : tokenReason);
        return std::nullopt;
    };
    const bool running = s.state == McpServerState::Running;
    const bool stopping = s.state == McpServerState::Stopping;
    const auto stoppingRefusal =
        refusal(McpUnavailableCode::Stopping, McpField::Status, "The server is stopping.");
    if (s.shuttingDown) {
        for (auto& value : set.entries)
            value = shuttingDown();
        return set;
    }
    // Start
    if (running)
        entry(McpServerCommand::Start) =
            refusal(McpUnavailableCode::AlreadyRunning, McpField::Status, "The server is running.");
    else if (stopping)
        entry(McpServerCommand::Start) = stoppingRefusal;
    else if (const auto refused = tokenRefusal())
        entry(McpServerCommand::Start) = *refused;
    else
        entry(McpServerCommand::Start) = available();
    // Stop
    if (stopping)
        entry(McpServerCommand::Stop) = stoppingRefusal;
    else if (!running)
        entry(McpServerCommand::Stop) =
            refusal(McpUnavailableCode::NotRunning, McpField::Status, "The server is not running.");
    else
        entry(McpServerCommand::Stop) = available();
    // SetPort
    if (stopping)
        entry(McpServerCommand::SetPort) = stoppingRefusal;
    else if (running)
        entry(McpServerCommand::SetPort) =
            refusal(McpUnavailableCode::ServerRunning, McpField::Port,
                    "Stop the server to change the port.", s.preferences.port);
    else
        entry(McpServerCommand::SetPort) = available();
    entry(McpServerCommand::SetAutoStart) = available();
    // RegenerateToken: available while Unavailable, to retry.
    if (token == McpTokenState::Loading || token == McpTokenState::Regenerating)
        entry(McpServerCommand::RegenerateToken) = refusal(
            McpUnavailableCode::TokenLoading, McpField::Token, "The access token is loading.");
    else
        entry(McpServerCommand::RegenerateToken) = available();
    const auto reveal = tokenRefusal();
    entry(McpServerCommand::RevealToken) = reveal ? *reveal : available();
    entry(McpServerCommand::CopyClientConfig) = reveal ? *reveal : available();
    return set;
}

} // namespace

void McpServerService::pump() {
    auto& s = *state_;
    // Owner-thread calls queued by request workers (export queue commands and reads).
    s.runOwnerTasks();
    s.core->followLiveSession();

    // Token task results.
    if (s.tokenTask) {
        std::optional<service::TokenTask> result;
        {
            const std::lock_guard lock(s.tokenTask->mutex);
            if (s.tokenTask->done)
                result.emplace();
            if (result) {
                result->state = s.tokenTask->state;
                result->token = s.tokenTask->token;
                result->reason = s.tokenTask->reason;
            }
        }
        if (result) {
            s.tokenTask.reset();
            const bool regenerated = s.tokenRegenerating;
            s.tokenRegenerating = false;
            bool changed = false;
            {
                const std::lock_guard lock(s.tokenMutex);
                if (result->state == McpTokenState::Ready) {
                    changed = s.token != result->token;
                    s.token = result->token;
                    s.tokenState = McpTokenState::Ready;
                    s.tokenReason.clear();
                    if (regenerated)
                        ++s.tokenGeneration;
                } else if (!regenerated || s.token.empty()) {
                    s.tokenState = McpTokenState::Unavailable;
                    s.tokenReason = result->reason;
                } else {
                    // A failed regeneration keeps the working token.
                    s.tokenState = McpTokenState::Ready;
                }
            }
            if (regenerated && changed) {
                std::uint64_t generation = 0;
                {
                    const std::lock_guard lock(s.tokenMutex);
                    generation = s.tokenGeneration;
                }
                {
                    const std::lock_guard lock(s.factsMutex);
                    std::vector<std::string> ids;
                    for (const auto& [id, client] : s.sessions)
                        ids.push_back(id);
                    for (const auto& id : ids)
                        s.dropClientLocked(id, McpDisconnectCause::TokenRegenerated);
                    s.pendingEvents.emplace_back(McpTokenRegenerated{generation});
                    s.dirty = true;
                }
                McpActivityEntry entry;
                entry.kind = McpActivityKind::TokenRegenerated;
                entry.summary = "Access token regenerated";
                s.recordActivity(std::move(entry));
                // Connections authorised with the old token are closed; clients reconnect.
                if (s.network) {
                    const std::lock_guard lock(s.network->mutex);
                    for (const auto& connection : s.network->connections)
                        connection->close();
                }
            }
            const std::lock_guard lock(s.factsMutex);
            s.dirty = true;
        }
    }

    // Auto-start once the token is Ready.
    if (s.autoStartPending && !s.shuttingDown) {
        McpTokenState token = McpTokenState::Loading;
        {
            const std::lock_guard lock(s.tokenMutex);
            token = s.tokenState;
        }
        if (token != McpTokenState::Loading && token != McpTokenState::Regenerating) {
            s.autoStartPending = false;
            if (s.preferences.autoStart && token == McpTokenState::Ready &&
                (s.state == McpServerState::Stopped || s.state == McpServerState::Failed))
                static_cast<void>(start());
        }
    }

    // Listener failure while running.
    if (s.state == McpServerState::Running && s.network && s.network->failed) {
        const auto port = s.network->port;
        s.stopNetwork();
        McpServerFailure failure{McpServerFailureCode::ListenerFailed, port,
                                 "The server stopped unexpectedly on " + portText(port) +
                                     ". Start it again.",
                                 SystemClock::now()};
        s.failure = failure;
        s.recordEvent(McpServerListenerFailed{failure});
        McpActivityEntry entry;
        entry.kind = McpActivityKind::ListenerFailed;
        entry.summary = "Listener failed";
        s.recordActivity(std::move(entry));
        s.stopCause = McpStopCause::ListenerFailed;
        s.state = McpServerState::Stopping;
    }

    // Stopping reaches Stopped (or Failed) once every thread has finished.
    if (s.state == McpServerState::Stopping && s.networkQuiescent()) {
        s.network.reset();
        s.endpoint.reset();
        const auto cause = s.stopCause.value_or(McpStopCause::User);
        s.state = cause == McpStopCause::ListenerFailed ? McpServerState::Failed
                                                        : McpServerState::Stopped;
        s.recordEvent(McpServerStopped{cause});
        McpActivityEntry entry;
        entry.kind = McpActivityKind::ServerStopped;
        entry.summary = "Server stopped";
        s.recordActivity(std::move(entry));
        s.stopCause.reset();
    }

    // Idle client expiry.
    {
        const std::lock_guard lock(s.factsMutex);
        const auto now = service::SteadyClock::now();
        std::vector<std::string> expired;
        for (const auto& [id, client] : s.sessions) {
            if (!client->scripted && client->inFlight == 0 &&
                now - client->lastActivity >= s.dependencies.limits.sessionIdle)
                expired.push_back(id);
        }
        for (const auto& id : expired)
            s.dropClientLocked(id, McpDisconnectCause::Expired);
    }

    // The live project as the server sees it.
    const auto liveGeneration = s.dependencies.live.generation();
    if (liveGeneration != s.publishedLiveGeneration) {
        if (s.publishedLiveGeneration != 0 &&
            s.dependencies.live.availability() != scripting::LiveSessionAvailability::Bound) {
            McpActivityEntry entry;
            entry.kind = McpActivityKind::ProjectReplaced;
            entry.summary = "Project replaced";
            s.recordActivity(std::move(entry));
        }
        s.publishedLiveGeneration = liveGeneration;
        const std::lock_guard lock(s.factsMutex);
        s.dirty = true;
    }

    // Publish.
    std::vector<McpServerEventPayload> events;
    McpServerSnapshot next;
    {
        const std::lock_guard lock(s.factsMutex);
        if (!s.dirty && s.published)
            return;
        s.dirty = false;
        events.swap(s.pendingEvents);
        std::vector<std::shared_ptr<service::Client>> clients;
        for (const auto& [id, client] : s.sessions)
            clients.push_back(client);
        std::ranges::sort(clients, {}, [](const auto& client) { return client->id; });
        for (const auto& client : clients)
            next.clients.push_back(s.view(*client));
        next.activeRequests = s.activeRequests;
        next.totalRequests = s.totalRequests;
        next.lastActivityId = s.nextActivity - 1;
    }
    next.state = s.state;
    next.preferences = s.preferences;
    next.preferencesRevision = s.preferencesRevision;
    next.endpoint = s.endpoint;
    next.failure = s.failure;
    McpTokenState tokenState = McpTokenState::Loading;
    std::string tokenReason;
    {
        const std::lock_guard lock(s.tokenMutex);
        tokenState = s.tokenState;
        tokenReason = s.tokenReason;
        next.token.state = s.tokenState;
        next.token.generation = s.tokenGeneration;
        next.token.masked = s.tokenState == McpTokenState::Ready ? maskToken(s.token) : "";
        next.token.reason = s.tokenState == McpTokenState::Unavailable ? s.tokenReason : "";
    }
    const auto availability = s.dependencies.live.availability();
    next.project.bound = availability == scripting::LiveSessionAvailability::Bound;
    next.project.generation = liveGeneration;
    next.project.reason = next.project.bound ? ""
                          : availability == scripting::LiveSessionAvailability::ShuttingDown
                              ? "Bloom is closing."
                              : "A project is opening.";
    next.commands = commandsFor(s, tokenState, tokenReason);
    {
        const std::lock_guard lock(s.publishMutex);
        next.revision = (s.published ? s.published->revision : 0) + 1;
        for (auto& payload : events) {
            s.eventRing.push_back({s.nextEvent++, next.revision, std::move(payload)});
            while (s.eventRing.size() > s.dependencies.limits.retainedEvents)
                s.eventRing.pop_front();
        }
        s.published = std::make_shared<const McpServerSnapshot>(std::move(next));
    }
}

McpCommandResult McpServerService::start() {
    auto& s = *state_;
    McpCommandResult result;
    const auto refuse = [&](McpCommandAvailability availability) {
        result.availability = std::move(availability);
        pump();
        result.revision = snapshot()->revision;
        return result;
    };
    pump();
    if (const auto allowed = availability(McpServerCommand::Start); !allowed)
        return refuse(allowed);
    const auto port = s.preferences.port;
    auto listened = platform::LoopbackListener::listen(port);
    if (!listened.bind.bound()) {
        // Never another port: a client configured for this one must not reach a moved server.
        auto refused = bindRefusal(listened.bind);
        McpServerFailure failure{failureCode(refused.code), port, refused.reason,
                                 SystemClock::now()};
        s.failure = failure;
        s.recordEvent(McpServerStartRefused{failure});
        McpActivityEntry entry;
        entry.kind = McpActivityKind::StartRefused;
        entry.summary = "Start refused (" + portText(port) + ")";
        s.recordActivity(std::move(entry));
        return refuse(std::move(refused));
    }
    s.failure.reset();
    s.endpoint = endpointFor(port);
    s.state = McpServerState::Running;
    s.startNetwork(std::move(listened.listener));
    s.recordEvent(McpServerStarted{*s.endpoint});
    McpActivityEntry entry;
    entry.kind = McpActivityKind::ServerStarted;
    entry.summary = "Server started on " + portText(port);
    s.recordActivity(std::move(entry));
    pump();
    result.accepted = true;
    result.availability = available();
    result.revision = snapshot()->revision;
    return result;
}

McpCommandResult McpServerService::stop() {
    auto& s = *state_;
    McpCommandResult result;
    pump();
    if (const auto allowed = availability(McpServerCommand::Stop); !allowed) {
        result.availability = allowed;
        result.revision = snapshot()->revision;
        return result;
    }
    s.stopCause = McpStopCause::User;
    s.stopNetwork();
    s.state = McpServerState::Stopping;
    {
        const std::lock_guard lock(s.factsMutex);
        s.dirty = true;
    }
    pump();
    result.accepted = true;
    result.availability = available();
    result.revision = snapshot()->revision;
    return result;
}

McpCommandResult McpServerService::setPort(const std::uint32_t port) {
    auto& s = *state_;
    McpCommandResult result;
    pump();
    // ShuttingDown, Stopping and ServerRunning come from the state first, then the range.
    auto allowed = availability(McpServerCommand::SetPort);
    if (allowed) {
        if (auto range = validateMcpPort(port))
            allowed = std::move(*range);
    }
    if (!allowed) {
        result.availability = allowed;
        result.revision = snapshot()->revision;
        return result;
    }
    if (s.preferences.port != port) {
        s.preferences.port = static_cast<std::uint16_t>(port);
        ++s.preferencesRevision;
    }
    // The failure was about the old port.
    s.failure.reset();
    if (s.state == McpServerState::Failed)
        s.state = McpServerState::Stopped;
    {
        const std::lock_guard lock(s.factsMutex);
        s.dirty = true;
    }
    pump();
    result.accepted = true;
    result.availability = available();
    result.revision = snapshot()->revision;
    return result;
}

McpCommandResult McpServerService::setAutoStart(const bool enabled) {
    auto& s = *state_;
    McpCommandResult result;
    pump();
    if (const auto allowed = availability(McpServerCommand::SetAutoStart); !allowed) {
        result.availability = allowed;
        result.revision = snapshot()->revision;
        return result;
    }
    if (s.preferences.autoStart != enabled) {
        s.preferences.autoStart = enabled;
        ++s.preferencesRevision;
        const std::lock_guard lock(s.factsMutex);
        s.dirty = true;
    }
    pump();
    result.accepted = true;
    result.availability = available();
    result.revision = snapshot()->revision;
    return result;
}

McpCommandResult McpServerService::regenerateToken() {
    auto& s = *state_;
    McpCommandResult result;
    pump();
    if (const auto allowed = availability(McpServerCommand::RegenerateToken); !allowed) {
        result.availability = allowed;
        result.revision = snapshot()->revision;
        return result;
    }
    if (s.dependencies.tokenFile.empty()) {
        result.availability = refusal(McpUnavailableCode::TokenUnavailable, McpField::Token,
                                      std::string(kNoTokenFolder));
        result.revision = snapshot()->revision;
        return result;
    }
    {
        const std::lock_guard lock(s.tokenMutex);
        // The current token keeps working until the new one is written.
        s.tokenState = s.token.empty() ? McpTokenState::Loading : McpTokenState::Regenerating;
    }
    s.tokenRegenerating = true;
    s.tokenTask = std::make_shared<service::TokenTask>();
    std::weak_ptr<State> weak = state_;
    auto submitted = s.dependencies.scheduler.submit<void>(
        runtime::TaskRequest("Regenerate the MCP access token", tokenOwner(),
                             runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo),
        [task = s.tokenTask, file = s.dependencies.tokenFile,
         weak](runtime::TaskContext&) -> runtime::TaskResult<void> {
            loadToken(file, true, *task);
            if (const auto state = weak.lock())
                state->wake();
            return runtime::TaskResult<void>::succeeded();
        });
    static_cast<void>(submitted);
    {
        const std::lock_guard lock(s.factsMutex);
        s.dirty = true;
    }
    pump();
    result.accepted = true;
    result.availability = available();
    result.revision = snapshot()->revision;
    return result;
}

McpTokenReveal McpServerService::revealToken() {
    auto& s = *state_;
    McpTokenReveal reveal;
    pump();
    reveal.availability = availability(McpServerCommand::RevealToken);
    if (!reveal.availability)
        return reveal;
    {
        const std::lock_guard lock(s.tokenMutex);
        reveal.token = s.token;
    }
    McpActivityEntry entry;
    entry.kind = McpActivityKind::TokenRevealed;
    entry.summary = "Token revealed";
    s.recordActivity(std::move(entry));
    pump();
    return reveal;
}

McpClientConfig McpServerService::clientConfig(const McpClientKind kind,
                                               const McpTokenDisclosure disclosure) const {
    const auto& s = *state_;
    McpClientConfig config;
    config.kind = kind;
    constexpr std::array<std::string_view, kMcpClientKindCount> titles{"Claude Code", "MCP JSON",
                                                                       "VS Code", "Other client"};
    constexpr std::array<std::string_view, kMcpClientKindCount> hints{
        "Run this command in a terminal.", "Paste this into the project's .mcp.json file.",
        "Paste this into .vscode/mcp.json.", "Enter the address and header in your client."};
    config.title = std::string(titles.at(static_cast<std::size_t>(kind)));
    config.hint = std::string(hints.at(static_cast<std::size_t>(kind)));
    const auto endpoint = endpointFor(s.preferences.port);
    if (disclosure == McpTokenDisclosure::EnvironmentPlaceholder) {
        config.availability = available();
        config.text = clientConfigText(kind, endpoint, "${BLOOM_MCP_TOKEN}");
        return config;
    }
    config.availability = availability(McpServerCommand::CopyClientConfig);
    if (!config.availability)
        return config;
    std::string token;
    {
        const std::lock_guard lock(s.tokenMutex);
        token = s.token;
    }
    config.text = clientConfigText(kind, endpoint, token);
    return config;
}

void McpServerService::requestAutoStart() {
    state_->autoStartPending = true;
    pump();
}

void McpServerService::beginShutdown() {
    auto& s = *state_;
    if (s.shuttingDown)
        return;
    if (s.state == McpServerState::Running) {
        s.stopCause = McpStopCause::Shutdown;
        s.stopNetwork();
        s.state = McpServerState::Stopping;
    }
    s.shuttingDown = true;
    s.autoStartPending = false;
    {
        const std::lock_guard lock(s.ownerMutex);
        s.ownerOpen = false;
    }
    s.withdrawOwnerTasks();
    {
        const std::lock_guard lock(s.factsMutex);
        s.dirty = true;
    }
    pump();
}

bool McpServerService::quiescent() const noexcept {
    const auto& s = *state_;
    if (s.state == McpServerState::Running || s.state == McpServerState::Stopping)
        return false;
    if (!s.tokenTask)
        return true;
    const std::lock_guard lock(s.tokenTask->mutex);
    return s.tokenTask->done;
}

void McpServerService::exportQueueReplaced() {
    ++state_->exportGeneration;
    pump();
}

} // namespace bloom::mcp
