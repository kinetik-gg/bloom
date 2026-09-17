#include "support.hpp"
#include <atomic>
#include <bloom/media/provider/worker_pool.hpp>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <map>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace bloom::media::provider;
namespace {
using Clock = std::chrono::steady_clock;
struct Fixture {
    Handshake hello;
    CapabilityRegistry registry;
    std::vector<PipelineQualificationV1> pipelines;
    Fixture() {
        hello.execution = test::execution();
        hello.execution.build = BLOOM_MEDIA_BUILD_ID;
        hello.execution.os = BLOOM_MEDIA_OS;
        hello.execution.architecture = BLOOM_MEDIA_ARCH;
        const auto lock = Digest::fromLowercaseHex(BLOOM_MEDIA_DEPENDENCY_LOCK);
        if (!lock)
            throw std::runtime_error("lock digest");
        hello.execution.dependencyLock = *lock;
        hello.transports = {Transport::PipeCopiesV0};
        for (auto role : {Role::Probe, Role::VideoDecode}) {
            ProviderDeclaration d{test::capability(role), hello.execution, test::evidence()};
            hello.declarations.push_back(d);
            test::check(std::holds_alternative<Digest>(registry.registerProvider(d)),
                        "register fake");
            PipelineQualificationV1 p;
            p.steps = {{std::get<Digest>(digest(d.capability)),
                        std::get<Digest>(digest(d.execution)),
                        std::get<Digest>(digest(d.evidence))}};
            p.profile = "synthetic-v1";
            p.reopenPolicy = "none";
            p.qcProfile = "none";
            p.result = QcResult::Pass;
            test::check(std::holds_alternative<Digest>(registry.qualifyPipeline(p)),
                        "qualify fake");
            pipelines.push_back(p);
        }
    }
    MediaAttempt attempt(bool decode = false) {
        return std::get<MediaAttempt>(registry.begin(pipelines[decode ? 1U : 0U]));
    }
    CallRequest request(bool decode = false) {
        return {test::capability(decode ? Role::VideoDecode : Role::Probe), "synthetic:rgba8", 16,
                8, 7};
    }
    WorkerPoolOptions options(std::string executable = BLOOM_MEDIA_WORKER) {
        WorkerPoolOptions o;
        o.process.executable = std::move(executable);
        o.expected = hello;
        o.timeout = std::chrono::seconds(3);
        return o;
    }
};
bloom::runtime::TaskOwner owner() {
    return {bloom::runtime::TaskOwnerKind::Application, bloom::runtime::TaskOwnerId::fromRaw(812)};
}
void waitDone(WorkerTicket& ticket) {
    test::check(ticket.submission.accepted(), "task admission");
    const auto deadline = Clock::now() + std::chrono::seconds(6);
    while (Clock::now() < deadline) {
        if (ticket.submission.handle.tryTakeResult())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("worker task did not terminate");
}
void expectError(WorkerTicket& ticket, Error expected) {
    waitDone(ticket);
    const auto result = ticket.result();
    test::check(result && std::holds_alternative<Unavailable>(*result), "worker error outcome");
    const auto& error = std::get<Unavailable>(*result);
    if (error.reason != expected)
        throw std::runtime_error("wrong error: " + error.detail + " got " +
                                 std::to_string(static_cast<int>(error.reason)) + " expected " +
                                 std::to_string(static_cast<int>(expected)));
}
void reaped(std::int64_t pid) {
    test::check(pid > 0, "launched PID");
    int status = 0;
    errno = 0;
    test::check(::waitpid(static_cast<pid_t>(pid), &status, WNOHANG) == -1 && errno == ECHILD,
                "child reaped");
}
void waitMarker(const std::filesystem::path& marker) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < deadline) {
        if (std::filesystem::exists(marker)) {
            std::ifstream stream(marker);
            std::string text;
            stream >> text;
            if (text == "call-received")
                return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("fixture never received call");
}
void queuedCancellation(Fixture& fixture) {
    bloom::runtime::TaskSchedulerConfig config;
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    config.rowBandWorkerCount = bloom::runtime::kSerialRowBandWorkers;
    std::atomic<bool> started{false}, release{false};
    bloom::runtime::TaskScheduler scheduler(config);
    auto barrier =
        scheduler.submit<void>({"Queue fixture", owner(), bloom::runtime::TaskPriority::Foreground,
                                bloom::runtime::TaskExecutor::BlockingIo},
                               [&](bloom::runtime::TaskContext& context) {
                                   started.store(true);
                                   while (!release.load() && !context.isCancellationRequested())
                                       std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                   return bloom::runtime::TaskResult<void>::succeeded();
                               });
    test::check(barrier.accepted(), "queue barrier admission");
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!started.load() && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    test::check(started.load(), "queue barrier started");
    auto options = fixture.options();
    options.capacity = 1;
    MediaWorkerPool pool(options);
    auto cancelled = pool.submit(scheduler, owner(), fixture.attempt(), 0, fixture.request());
    test::check(cancelled.submission.accepted(), "queued worker admitted");
    cancelled.submission.handle.cancel();
    waitDone(cancelled);
    test::check(!cancelled.result(), "cancelled queued task never publishes a product");
    auto next = pool.submit(scheduler, owner(), fixture.attempt(), 0, fixture.request());
    release.store(true);
    waitDone(next);
    test::check(next.result() && std::holds_alternative<ProbeResult>(*next.result()),
                "queued cancellation does not retain a pool slot");
}
void run() {
    Fixture fixture;
    bloom::runtime::TaskSchedulerConfig config;
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 2;
    config.rowBandWorkerCount = bloom::runtime::kSerialRowBandWorkers;
    bloom::runtime::TaskScheduler scheduler(config);
    std::atomic<std::int64_t> pid{0};
    auto options = fixture.options();
    options.capacity = 1;
    options.launched = [&](auto value) { pid.store(value); };
    MediaWorkerPool pool(options);
    auto probe = pool.submit(scheduler, owner(), fixture.attempt(), 0, fixture.request());
    waitDone(probe);
    const auto probed = probe.result();
    test::check(probed && std::holds_alternative<ProbeResult>(*probed), "real worker probe");
    const auto& stream = std::get<ProbeResult>(*probed).streams.front();
    test::check(stream.codec == "fake.rgba8" && stream.timebase == Rational{1, 24} &&
                    stream.width == 16 && stream.height == 8,
                "probe descriptor");
    reaped(pid.load());
    auto decode = pool.submit(scheduler, owner(), fixture.attempt(true), 0, fixture.request(true));
    waitDone(decode);
    const auto decoded = decode.result();
    test::check(decoded && std::holds_alternative<FrameProduct>(*decoded), "real worker decode");
    const auto& frame = std::get<FrameProduct>(*decoded);
    test::check(frame.pts == Rational{7, 24} && valid(frame), "frame timing and bounds");
    Bytes oracle;
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 16; ++x)
            for (unsigned component : {x, y, 7U, 255U})
                oracle.push_back(static_cast<std::byte>(component));
    test::check(frame.planes[0].bytes == oracle && frame.planes[0].digest == digestBytes(oracle),
                "independent deterministic plane oracle");
    reaped(pid.load());
    std::weak_ptr<const WorkerReply> released;
    {
        auto completed =
            pool.submit(scheduler, owner(), fixture.attempt(true), 0, fixture.request(true));
        waitDone(completed);
        released = completed.result();
        test::check(!released.expired(), "ticket owns completed frame");
    }
    test::check(released.expired(), "terminal task history does not retain media products");
    const std::map<std::string, Error> errors{{"Truncated", Error::Truncated},
                                              {"Crashed", Error::Crashed},
                                              {"Oversized", Error::Oversized},
                                              {"BadEnum", Error::BadEnum},
                                              {"BadLength", Error::BadLength},
                                              {"Replay", Error::Replay},
                                              {"VersionMismatch", Error::VersionMismatch},
                                              {"IdentityMismatch", Error::IdentityMismatch},
                                              {"DigestMismatch", Error::DigestMismatch},
                                              {"Timeout", Error::Timeout}};
    std::ifstream cases(BLOOM_MEDIA_FIXTURES);
    test::check(static_cast<bool>(cases), "synthetic fixture manifest");
    std::string line;
    while (std::getline(cases, line)) {
        if (line.empty() || line.front() == '#')
            continue;
        const auto split = line.find(' ');
        test::check(split != std::string::npos, "case syntax");
        const auto mode = line.substr(0, split);
        const auto expected = errors.at(line.substr(split + 1));
        auto hostile = fixture.options(BLOOM_MEDIA_HOSTILE_WORKER);
        hostile.process.arguments = {mode};
        hostile.timeout = std::chrono::milliseconds(600);
        hostile.launched = options.launched;
        MediaWorkerPool bad(hostile);
        const bool video = mode == "digest";
        auto ticket =
            bad.submit(scheduler, owner(), fixture.attempt(video), 0, fixture.request(video));
        expectError(ticket, expected);
        reaped(pid.load());
    }
    auto limited = fixture.options(BLOOM_MEDIA_HOSTILE_WORKER);
    limited.process.arguments = {"limits"};
    MediaWorkerPool limits(limited);
    auto limitedCall = limits.submit(scheduler, owner(), fixture.attempt(), 0, fixture.request());
    waitDone(limitedCall);
    test::check(limitedCall.result() && std::holds_alternative<ProbeResult>(*limitedCall.result()),
                "rlimits installed before provider call");
    const auto marker = std::filesystem::temp_directory_path() /
                        ("bloom-media-worker-test-" + std::to_string(::getpid()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{marker};
    for (const std::string mode : {"kill", "cancel", "term", "hang", "shutdown"}) {
        std::filesystem::remove(marker);
        auto controlled = fixture.options(BLOOM_MEDIA_HOSTILE_WORKER);
        controlled.process.arguments = {mode == "shutdown" ? "hang" : mode, marker.string()};
        controlled.launched = options.launched;
        controlled.capacity = 1;
        MediaWorkerPool control(controlled);
        auto attempt = fixture.attempt();
        auto ticket = control.submit(scheduler, owner(), attempt, 0, fixture.request());
        waitMarker(marker);
        const auto snapshot = scheduler.snapshot(ticket.submission.handle.id());
        test::check(snapshot && snapshot->executor == bloom::runtime::TaskExecutor::BlockingIo &&
                        snapshot->progress.indeterminate(),
                    "BlockingIo activity reporting");
        auto excess = control.submit(scheduler, owner(), fixture.attempt(), 0, fixture.request());
        test::check(!excess.submission.accepted() &&
                        std::get<Unavailable>(*excess.result()).reason == Error::Busy,
                    "bounded pool admission");
        if (mode == "kill") {
            test::check(::kill(static_cast<pid_t>(pid.load()), SIGKILL) == 0, "kill -9 mid-call");
            expectError(ticket, Error::Crashed);
        } else {
            if (mode == "shutdown")
                control.shutdown();
            else
                ticket.submission.handle.cancel();
            expectError(ticket, Error::Cancelled);
        }
        reaped(pid.load());
        if (mode == "cancel") {
            std::ifstream markerStream(marker);
            std::string acknowledgement;
            markerStream >> acknowledgement;
            test::check(acknowledgement == "cancel-received",
                        "cooperative cancel message delivered");
        }
        test::check(!attempt.accepts(0, fixture.hello.execution),
                    "failed worker terminates attempt");
    }
    queuedCancellation(fixture);
    pool.shutdown();
    auto stopped = pool.submit(scheduler, owner(), fixture.attempt(), 0, fixture.request());
    test::check(!stopped.submission.accepted() &&
                    std::get<Unavailable>(*stopped.result()).reason == Error::Shutdown,
                "shutdown rejects new calls");
}
} // namespace
int main() {
    try {
        run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
