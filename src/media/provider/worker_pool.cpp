#include <algorithm>
#include <array>
#include <bloom/media/provider/worker_pool.hpp>
#include <new>
#include <random>

namespace bloom::media::provider {
namespace {
using Clock = std::chrono::steady_clock;
Unavailable failure(const platform::ProcessFailure& p) {
    switch (p.code) {
    case platform::ProcessError::Unavailable:
        return {Error::Unavailable, "Worker process unavailable on this platform"};
    case platform::ProcessError::Timeout:
        return {Error::Timeout, "Worker watchdog expired"};
    case platform::ProcessError::Cancelled:
        return {Error::Cancelled, "Worker call cancelled"};
    case platform::ProcessError::Crashed:
        return {Error::Crashed, "Worker exited during call"};
    case platform::ProcessError::ResourceLimit:
        return {Error::Oversized, "Worker resource limits rejected"};
    case platform::ProcessError::Spawn:
    case platform::ProcessError::Io:
        return {Error::Io, "Worker launch or pipe I/O failed"};
    }
    return {Error::Io, "Unknown process failure"};
}
std::string diagnosticCode(Error e) {
    switch (e) {
    case Error::Cancelled:
        return "media.worker.cancelled";
    case Error::Timeout:
        return "media.worker.timeout";
    case Error::Crashed:
        return "media.worker.crashed";
    case Error::Unavailable:
        return "media.worker.unavailable";
    default:
        return "media.worker.rejected";
    }
}
class Connection final {
  public:
    Connection(platform::ProcessSupervisor& process, platform::ProcessDeadline deadline,
               platform::ProcessCancellation cancel)
        : process_(process), deadline_(deadline), cancel_(std::move(cancel)) {}
    Result<Message> exchange(const Message& message, HostProtocol& host, MessageKind expected) {
        auto encoded = encodeMessage(message);
        const auto* bytes = std::get_if<Bytes>(&encoded);
        if (!bytes)
            return std::get<Unavailable>(encoded);
        auto written = process_.write(*bytes, deadline_, cancel_);
        if (const auto* e = std::get_if<platform::ProcessFailure>(&written))
            return failure(*e);
        std::array<std::byte, 4> prefix{};
        auto read = process_.read(prefix, deadline_, cancel_);
        if (const auto* e = std::get_if<platform::ProcessFailure>(&read))
            return failure(*e);
        const auto size = messageLength(prefix);
        if (const auto* e = std::get_if<Unavailable>(&size))
            return *e;
        Bytes response(prefix.begin(), prefix.end());
        response.resize(4U + std::get<std::uint32_t>(size));
        read = process_.read(std::span(response).subspan(4), deadline_, cancel_);
        if (const auto* e = std::get_if<platform::ProcessFailure>(&read)) {
            if (e->code == platform::ProcessError::Crashed)
                return Unavailable{Error::Truncated, "Worker closed an incomplete frame"};
            return failure(*e);
        }
        return host.receive(response, expected);
    }

  private:
    platform::ProcessSupervisor& process_;
    platform::ProcessDeadline deadline_;
    platform::ProcessCancellation cancel_;
};
WorkerReply execute(const WorkerPoolOptions& options, const CallRequest& request,
                    const platform::ProcessCancellation& cancel) {
    if (cancel())
        return Unavailable{Error::Cancelled, "Worker call cancelled before launch"};
    const auto deadline = Clock::now() + options.timeout;
    auto launched = platform::ProcessSupervisor::launch(options.process);
    auto* process = std::get_if<std::unique_ptr<platform::ProcessSupervisor>>(&launched);
    if (!process)
        return failure(std::get<platform::ProcessFailure>(launched));
    if (options.launched)
        options.launched((*process)->processId());
    std::random_device random;
    std::uint64_t session = (static_cast<std::uint64_t>(random()) << 32U) | random();
    if (session == 0)
        session = 1;
    HostProtocol host(session, options.expected);
    Connection connection(**process, deadline, cancel);
    auto stop = [&] {
        const auto message = encodeMessage({MessageKind::Cancel, session, 3, std::monostate{}});
        if (const auto* bytes = std::get_if<Bytes>(&message))
            (*process)->stop(*bytes);
        else
            (*process)->stop();
    };
    auto hello = connection.exchange({MessageKind::Handshake, session, 1, options.expected}, host,
                                     MessageKind::Handshake);
    if (const auto* e = std::get_if<Unavailable>(&hello)) {
        stop();
        return *e;
    }
    const auto expectedKind = request.capability.role == Role::Probe         ? MessageKind::Probe
                              : request.capability.role == Role::DemuxIndex  ? MessageKind::Index
                              : request.capability.role == Role::AudioDecode ? MessageKind::Audio
                                                                             : MessageKind::Frame;
    auto response =
        connection.exchange({MessageKind::Call, session, 2, request}, host, expectedKind);
    if (const auto* e = std::get_if<Unavailable>(&response)) {
        stop();
        return *e;
    }
    auto message = std::get<Message>(std::move(response));
    if (const auto* e = std::get_if<Unavailable>(&message.payload)) {
        stop();
        return *e;
    }
    auto closed = connection.exchange({MessageKind::Shutdown, session, 3, std::monostate{}}, host,
                                      MessageKind::Ack);
    if (const auto* e = std::get_if<Unavailable>(&closed)) {
        stop();
        return *e;
    }
    if (const auto* e = std::get_if<Unavailable>(&std::get<Message>(closed).payload)) {
        stop();
        return *e;
    }
    auto finished = (*process)->finish(deadline);
    if (const auto* e = std::get_if<platform::ProcessFailure>(&finished))
        return failure(*e);
    // Cancellation arriving during orderly close also prevents product publication.
    if (cancel())
        return Unavailable{Error::Cancelled, "Cancelled before product publication"};
    if (auto* probe = std::get_if<ProbeResult>(&message.payload)) {
        if (request.capability.mapping != "bounded-discovery-v1" &&
            (probe->container != request.capability.container ||
             std::ranges::any_of(probe->streams, [&](const auto& stream) {
                 return stream.kind == MediaKind::Video &&
                        (stream.width != request.width || stream.height != request.height ||
                         stream.codec != request.capability.codec);
             })))
            return Unavailable{Error::IdentityMismatch, "Probe does not match the requested tuple"};
        return std::move(*probe);
    }
    if (auto* index = std::get_if<DemuxIndex>(&message.payload))
        return std::move(*index);
    if (auto* audio = std::get_if<AudioBlock>(&message.payload))
        return std::move(*audio);
    const auto& frame = std::get<FrameProduct>(message.payload);
    if (frame.planes.front().width != request.width ||
        frame.planes.front().height != request.height)
        return Unavailable{Error::IdentityMismatch, "Frame extent does not match the request"};
    return std::get<FrameProduct>(std::move(message.payload));
}
} // namespace
struct WorkerTicket::State {
    std::mutex mutex;
    std::shared_ptr<const WorkerReply> reply;
};
std::shared_ptr<const WorkerReply> WorkerTicket::result() const {
    if (!state_)
        return {};
    std::lock_guard lock(state_->mutex);
    return state_->reply;
}
struct MediaWorkerPool::State {
    explicit State(WorkerPoolOptions value) : options(std::move(value)) {}
    WorkerPoolOptions options;
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> active{0};
};
MediaWorkerPool::MediaWorkerPool(WorkerPoolOptions options)
    : state_(std::make_shared<State>(std::move(options))) {}
MediaWorkerPool::~MediaWorkerPool() { shutdown(); }
void MediaWorkerPool::shutdown() { state_->stopping.store(true); }
WorkerTicket MediaWorkerPool::submit(runtime::TaskScheduler& scheduler, runtime::TaskOwner owner,
                                     const MediaAttempt& attempt, std::size_t step,
                                     CallRequest request) {
    WorkerTicket ticket;
    ticket.state_ = std::make_shared<WorkerTicket::State>();
    auto reject = [&](Unavailable error) {
        ticket.state_->reply = std::make_shared<const WorkerReply>(std::move(error));
        ticket.submission.status = runtime::TaskSubmissionStatus::InvalidRequest;
    };
    if (state_->stopping.load()) {
        reject({Error::Shutdown, "Worker pool is shutting down"});
        return ticket;
    }
    if (state_->options.capacity == 0 || state_->options.capacity > 64 ||
        state_->options.timeout <= std::chrono::milliseconds(0) ||
        state_->options.timeout > std::chrono::minutes(5)) {
        reject({Error::InvalidValue, "Invalid worker pool limits"});
        return ticket;
    }
    if (state_->options.expected.execution.resourceProfile != "media-v1" ||
        state_->options.process.addressSpaceBytes != 2ULL * 1024 * 1024 * 1024 ||
        state_->options.process.openFiles != 64) {
        reject({Error::IdentityMismatch, "Process limits do not match the execution profile"});
        return ticket;
    }
    if (!attempt.accepts(step, state_->options.expected.execution) ||
        attempt.pipeline().providers[step].capability != request.capability ||
        (request.capability.role != Role::Probe && request.capability.role != Role::VideoDecode &&
         request.capability.role != Role::DemuxIndex &&
         request.capability.role != Role::AudioDecode)) {
        reject({Error::Unavailable, "Call does not match pinned execution and capability"});
        return ticket;
    }
    if (std::ranges::find(state_->options.expected.declarations,
                          attempt.pipeline().providers[step]) ==
        state_->options.expected.declarations.end()) {
        reject({Error::IdentityMismatch, "Pinned qualification is absent from the handshake"});
        return ticket;
    }
    if (state_->active.load() >= state_->options.capacity) {
        reject({Error::Busy, "Worker pool capacity reached"});
        return ticket;
    }
    // The scheduler bounds queued requests. Reserve process slots only while work runs:
    // terminal task history retains closures, and queued cancellation never invokes the body.
    runtime::TaskRequest task("Media worker", owner, runtime::TaskPriority::Foreground,
                              runtime::TaskExecutor::BlockingIo);
    ticket.submission = scheduler.submit<void>(
        std::move(task),
        [poolState = std::weak_ptr(state_), result = std::weak_ptr(ticket.state_), attempt, step,
         request = std::move(request)](runtime::TaskContext& context) mutable {
            auto state = poolState.lock();
            if (!state)
                return runtime::TaskResult<void>::cancelled();
            context.reportProgress({"Media worker", "Probe or decode", 0, std::nullopt});
            const auto cancel = [&] {
                return context.isCancellationRequested() || state->stopping.load();
            };
            WorkerReply reply;
            {
                const auto active = state->active.fetch_add(1);
                struct Slot {
                    std::atomic<std::size_t>& count;
                    ~Slot() { count.fetch_sub(1); }
                } slot{state->active};
                try {
                    if (active >= state->options.capacity)
                        reply = Unavailable{Error::Busy, "Worker pool capacity reached"};
                    else if (!attempt.accepts(step, state->options.expected.execution))
                        reply =
                            Unavailable{Error::Unavailable, "Attempt terminated before execution"};
                    else
                        reply = execute(state->options, request, cancel);
                } catch (const std::bad_alloc&) {
                    reply = Unavailable{Error::Oversized, "Worker allocation budget exhausted"};
                } catch (const std::exception&) {
                    reply = Unavailable{Error::Io, "Worker call failed unexpectedly"};
                }
            }
            const auto* error = std::get_if<Unavailable>(&reply);
            const auto outcome = error ? *error : Unavailable{};
            if (auto mailbox = result.lock()) {
                std::lock_guard lock(mailbox->mutex);
                mailbox->reply = std::make_shared<const WorkerReply>(std::move(reply));
            }
            if (error) {
                attempt.terminate();
                runtime::TaskDiagnostic diagnostic{
                    diagnosticCode(outcome.reason), runtime::DiagnosticSeverity::Error,
                    outcome.detail, "", "Retry as a new qualified attempt"};
                if (outcome.reason == Error::Cancelled)
                    return runtime::TaskResult<void>::cancelled({diagnostic});
                return runtime::TaskResult<void>::failed(std::move(diagnostic));
            }
            context.reportProgress({"Media worker", "Complete", 1, 1});
            return runtime::TaskResult<void>::succeeded();
        });
    return ticket;
}
} // namespace bloom::media::provider
