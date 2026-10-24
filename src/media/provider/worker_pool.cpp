#include <algorithm>
#include <array>
#include <bloom/media/provider/worker_pool.hpp>
#include <bloom/platform/file_snapshot.hpp>
#include <mutex>
#include <new>
#include <random>

namespace bloom::media::provider {
namespace {
using Clock = std::chrono::steady_clock;
// A retained worker is relaunched after five minutes or this many calls. Relaunching discards
// the decoder position and packet index, so the call bound is sized for continuous playback
// (about 20 minutes at 50 frames per second) and the five-minute lifetime is the binding limit.
constexpr std::size_t kRetainedCallLimit = 65536;
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

[[nodiscard]] std::uint64_t newSessionId() {
    std::random_device random;
    std::uint64_t session = (static_cast<std::uint64_t>(random()) << 32U) | random();
    return session == 0 ? 1 : session;
}

[[nodiscard]] MessageKind expectedProductKind(const CallRequest& request) {
    return request.capability.role == Role::Probe         ? MessageKind::Probe
           : request.capability.role == Role::DemuxIndex  ? MessageKind::Index
           : request.capability.role == Role::AudioDecode ? MessageKind::Audio
                                                          : MessageKind::Frame;
}

[[nodiscard]] WorkerReply validatedProduct(const CallRequest& request, Message message) {
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
    if (auto* frame = std::get_if<FrameProduct>(&message.payload)) {
        if (frame->planes.empty() || frame->planes.front().width != request.width ||
            frame->planes.front().height != request.height)
            return Unavailable{Error::IdentityMismatch, "Frame extent does not match the request"};
        return std::move(*frame);
    }
    return Unavailable{Error::UnexpectedMessage, "Worker returned no media product"};
}

WorkerReply execute(const WorkerPoolOptions& options, const CallRequest& request,
                    const std::shared_ptr<const platform::ReadOnlyFileSnapshot>& snapshot,
                    const platform::ProcessCancellation& cancel) {
    if (cancel())
        return Unavailable{Error::Cancelled, "Worker call cancelled before launch"};
    const auto deadline = Clock::now() + options.timeout;
    auto processOptions = options.process;
    processOptions.inputSnapshot = snapshot;
    processOptions.addressSpaceBytes = std::max(
        options.process.addressSpaceBytes, workerAddressSpaceBytes(request.width, request.height));
    auto launched = platform::ProcessSupervisor::launch(processOptions);
    auto* process = std::get_if<std::unique_ptr<platform::ProcessSupervisor>>(&launched);
    if (!process)
        return failure(std::get<platform::ProcessFailure>(launched));
    if (options.launched)
        options.launched((*process)->processId());
    const std::uint64_t session = newSessionId();
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
    auto response = connection.exchange({MessageKind::Call, session, 2, request}, host,
                                        expectedProductKind(request));
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
    return validatedProduct(request, std::move(message));
}
} // namespace
struct WorkerTicket::State {
    std::mutex mutex;
    std::shared_ptr<const WorkerReply> reply;
    std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot;
};
std::shared_ptr<const WorkerReply> WorkerTicket::result() const {
    if (!state_)
        return {};
    std::lock_guard lock(state_->mutex);
    return state_->reply;
}
struct MediaWorkerPool::State {
    explicit State(WorkerPoolOptions value) : options(std::move(value)) {}
    struct Retained final {
        Retained(std::unique_ptr<platform::ProcessSupervisor> value,
                 std::shared_ptr<const platform::ReadOnlyFileSnapshot> source,
                 const std::uint64_t id, const Handshake& expected)
            : process(std::move(value)), snapshot(std::move(source)), session(id),
              host(id, expected), started(Clock::now()), lastUsed(started) {}
        std::unique_ptr<platform::ProcessSupervisor> process;
        std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot;
        std::uint64_t session = 0;
        std::uint64_t sequence = 1;
        HostProtocol host;
        Clock::time_point started;
        Clock::time_point lastUsed;
        std::size_t calls = 0;
        std::uint64_t addressSpace = 0;
    };
    WorkerPoolOptions options;
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> active{0};
    std::mutex retainedMutex;
    std::unique_ptr<Retained> retained;
};
MediaWorkerPool::MediaWorkerPool(WorkerPoolOptions options)
    : state_(std::make_shared<State>(std::move(options))) {}
MediaWorkerPool::~MediaWorkerPool() { shutdown(); }
void MediaWorkerPool::shutdown() { state_->stopping.store(true); }
void MediaWorkerPool::releaseRetained() {
    const std::lock_guard lock(state_->retainedMutex);
    state_->retained.reset();
}

WorkerReply
MediaWorkerPool::call(const MediaAttempt& attempt, const std::size_t step, CallRequest request,
                      const std::shared_ptr<const platform::ReadOnlyFileSnapshot>& snapshot,
                      const platform::ProcessCancellation& cancel, const bool retainWorker) {
    if (request.snapshotBytes != 0 || state_->options.process.inputSnapshot)
        return Unavailable{Error::IdentityMismatch,
                           "Snapshot authority must be supplied by its owning host capability"};
    if (snapshot) {
        if (request.capability.role != Role::Probe && request.sourceDigest != snapshot->digest())
            return Unavailable{Error::SourceChanged,
                               "Snapshot does not match the requested source revision"};
        request.sourceDigest = snapshot->digest();
        request.snapshotBytes = snapshot->size();
    }
    if (state_->stopping.load())
        return Unavailable{Error::Shutdown, "Worker pool is shutting down"};
    if (state_->options.capacity != 1 || state_->options.timeout <= std::chrono::milliseconds(0) ||
        state_->options.timeout > std::chrono::minutes(5))
        return Unavailable{Error::InvalidValue, "Invalid direct worker pool limits"};
    if (state_->options.expected.execution.resourceProfile != "media-v1" ||
        state_->options.process.addressSpaceBytes != kWorkerAddressSpaceFloor ||
        state_->options.process.openFiles != 64)
        return Unavailable{Error::IdentityMismatch,
                           "Process limits do not match the execution profile"};
    if (!attempt.accepts(step, state_->options.expected.execution) ||
        attempt.pipeline().providers[step].capability != request.capability ||
        (request.capability.role != Role::Probe && request.capability.role != Role::VideoDecode &&
         request.capability.role != Role::DemuxIndex &&
         request.capability.role != Role::AudioDecode))
        return Unavailable{Error::Unavailable,
                           "Call does not match pinned execution and capability"};
    if (std::ranges::find(state_->options.expected.declarations,
                          attempt.pipeline().providers[step]) ==
        state_->options.expected.declarations.end())
        return Unavailable{Error::IdentityMismatch,
                           "Pinned qualification is absent from the handshake"};
    std::unique_lock lock(state_->retainedMutex, std::try_to_lock);
    if (!lock.owns_lock())
        return Unavailable{Error::Busy, "Video worker capacity reached"};
    if (state_->active.fetch_add(1) != 0) {
        state_->active.fetch_sub(1);
        return Unavailable{Error::Busy, "Video worker capacity reached"};
    }
    struct Slot final {
        std::atomic<std::size_t>& count;
        ~Slot() { count.fetch_sub(1); }
    } slot{state_->active};
    const auto cancelled = [&] { return state_->stopping.load() || (cancel && cancel()); };
    if (cancelled()) {
        state_->retained.reset();
        return Unavailable{Error::Cancelled, "Video worker call cancelled"};
    }
    const bool retain =
        retainWorker && state_->options.retainedVideo && snapshot &&
        (request.capability.role == Role::Probe || request.capability.role == Role::VideoDecode ||
         request.capability.role == Role::DemuxIndex ||
         request.capability.role == Role::AudioDecode);
    try {
        if (!retain) {
            state_->retained.reset();
            return execute(state_->options, request, snapshot, cancelled);
        }
        // The inherited immutable source handle is fixed at process launch. A newly captured
        // generation, even with the same digest, must get a fresh worker and decoder state.
        const auto now = Clock::now();
        // A retained worker launched for a smaller stream (or a probe) is relaunched with the
        // address space this stream needs.
        const auto addressSpace =
            std::max(state_->options.process.addressSpaceBytes,
                     workerAddressSpaceBytes(request.width, request.height));
        if (state_->retained && (state_->retained->snapshot != snapshot ||
                                 state_->retained->addressSpace < addressSpace ||
                                 now - state_->retained->started >= std::chrono::minutes(5) ||
                                 now - state_->retained->lastUsed >= std::chrono::seconds(30) ||
                                 state_->retained->calls >= kRetainedCallLimit))
            state_->retained.reset();
        if (!state_->retained) {
            auto options = state_->options.process;
            options.inputSnapshot = snapshot;
            options.addressSpaceBytes = addressSpace;
            options.arguments.emplace_back("--retained-video");
            auto launched = platform::ProcessSupervisor::launch(options);
            auto* process = std::get_if<std::unique_ptr<platform::ProcessSupervisor>>(&launched);
            if (!process)
                return failure(std::get<platform::ProcessFailure>(launched));
            if (state_->options.launched)
                state_->options.launched((*process)->processId());
            const auto id = newSessionId();
            state_->retained = std::make_unique<State::Retained>(std::move(*process), snapshot, id,
                                                                 state_->options.expected);
            state_->retained->addressSpace = addressSpace;
            Connection connection(*state_->retained->process, now + state_->options.timeout,
                                  cancelled);
            auto hello =
                connection.exchange({MessageKind::Handshake, id, 1, state_->options.expected},
                                    state_->retained->host, MessageKind::Handshake);
            if (const auto* error = std::get_if<Unavailable>(&hello)) {
                state_->retained.reset();
                return *error;
            }
        }
        auto& retained = *state_->retained;
        const auto sequence = ++retained.sequence;
        Connection connection(*retained.process, Clock::now() + state_->options.timeout, cancelled);
        auto response =
            connection.exchange({MessageKind::Call, retained.session, sequence, request},
                                retained.host, expectedProductKind(request));
        if (const auto* error = std::get_if<Unavailable>(&response)) {
            state_->retained.reset();
            return *error;
        }
        auto message = std::get<Message>(std::move(response));
        if (const auto* error = std::get_if<Unavailable>(&message.payload)) {
            state_->retained.reset();
            return *error;
        }
        if (cancelled()) {
            state_->retained.reset();
            return Unavailable{Error::Cancelled, "Cancelled before video product publication"};
        }
        auto product = validatedProduct(request, std::move(message));
        if (std::holds_alternative<Unavailable>(product)) {
            state_->retained.reset();
            return product;
        }
        retained.lastUsed = Clock::now();
        ++retained.calls;
        return product;
    } catch (const std::bad_alloc&) {
        state_->retained.reset();
        return Unavailable{Error::Oversized, "Worker allocation budget exhausted"};
    } catch (const std::exception&) {
        state_->retained.reset();
        return Unavailable{Error::Io, "Worker call failed unexpectedly"};
    }
}
WorkerTicket
MediaWorkerPool::submit(runtime::TaskScheduler& scheduler, runtime::TaskOwner owner,
                        const MediaAttempt& attempt, std::size_t step, CallRequest request,
                        std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot) {
    WorkerTicket ticket;
    ticket.state_ = std::make_shared<WorkerTicket::State>();
    auto reject = [&](Unavailable error) {
        ticket.state_->reply = std::make_shared<const WorkerReply>(std::move(error));
        ticket.submission.status = runtime::TaskSubmissionStatus::InvalidRequest;
    };
    if (request.snapshotBytes != 0 || state_->options.process.inputSnapshot) {
        reject({Error::IdentityMismatch,
                "Snapshot authority must be supplied by its owning host capability"});
        return ticket;
    }
    if (snapshot) {
        if (request.capability.role != Role::Probe && request.sourceDigest != snapshot->digest()) {
            reject({Error::SourceChanged, "Snapshot does not match the requested source revision"});
            return ticket;
        }
        request.sourceDigest = snapshot->digest();
        request.snapshotBytes = snapshot->size();
    }
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
        state_->options.process.addressSpaceBytes != kWorkerAddressSpaceFloor ||
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
    ticket.state_->snapshot = std::move(snapshot);
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
            std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot;
            if (auto mailbox = result.lock()) {
                std::lock_guard lock(mailbox->mutex);
                snapshot = mailbox->snapshot;
            } else
                return runtime::TaskResult<void>::cancelled();
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
                        reply = execute(state->options, request, snapshot, cancel);
                } catch (const std::bad_alloc&) {
                    reply = Unavailable{Error::Oversized, "Worker allocation budget exhausted"};
                } catch (const std::exception&) {
                    reply = Unavailable{Error::Io, "Worker call failed unexpectedly"};
                }
            }
            snapshot.reset();
            const auto* error = std::get_if<Unavailable>(&reply);
            const auto outcome = error ? *error : Unavailable{};
            if (auto mailbox = result.lock()) {
                std::lock_guard lock(mailbox->mutex);
                mailbox->reply = std::make_shared<const WorkerReply>(std::move(reply));
                mailbox->snapshot.reset();
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
