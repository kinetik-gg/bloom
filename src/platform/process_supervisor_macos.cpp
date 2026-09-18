#include <bloom/platform/process_supervisor.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#if !defined(__APPLE__)
#error "process_supervisor_macos.cpp requires macOS"
#endif

// macOS port of the Linux supervisor. Differences from the Linux provider, all forced by Darwin:
//   * no pipe2(O_CLOEXEC): pipe() plus F_DUPFD_CLOEXEC over fd 4 (the same high-fd trick).
//   * no posix_spawnattr_setrlimit_np and no addclosefrom_np: the child inherits no kernel rlimits
//     and relies on close-on-exec for every descriptor the host already marked, plus the explicit
//     file actions below. ProcessOptions limits remain validated but are not enforced by the
//     kernel.
//   * no sigtimedwait: pipeWrite consumes a SIGPIPE that only this thread raised with sigwait.
namespace bloom::platform {
namespace {
using Clock = std::chrono::steady_clock;
class Fd final {
  public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    int get() const { return fd_; }

  private:
    int fd_;
};
bool makePipe(std::array<Fd, 2>& ends) {
    std::array<int, 2> raw{};
    if (::pipe(raw.data()) != 0)
        return false;
    // Keep launch file actions independent of whether stdin/stdout/stderr were open in the host.
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const int high = ::fcntl(raw[i], F_DUPFD_CLOEXEC, 4);
        ::close(raw[i]);
        if (high < 0) {
            for (std::size_t j = i + 1; j < raw.size(); ++j)
                ::close(raw[j]);
            return false;
        }
        ends[i] = Fd(high);
    }
    return true;
}
class SpawnActions final {
  public:
    posix_spawn_file_actions_t value{};
    int status = posix_spawn_file_actions_init(&value);
    ~SpawnActions() {
        if (status == 0)
            posix_spawn_file_actions_destroy(&value);
    }
};
class SpawnAttributes final {
  public:
    posix_spawnattr_t value{};
    int status = posix_spawnattr_init(&value);
    ~SpawnAttributes() {
        if (status == 0)
            posix_spawnattr_destroy(&value);
    }
};
// Block SIGPIPE in this calling thread only; consume only a newly generated signal before restore.
ssize_t pipeWrite(int fd, const void* data, std::size_t size) {
    sigset_t set{}, old{}, pending{};
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &set, &old) != 0) {
        errno = EIO;
        return -1;
    }
    sigpending(&pending);
    const bool alreadyPending = sigismember(&pending, SIGPIPE) == 1;
    const auto written = ::write(fd, data, size);
    const int saved = errno;
    if (written < 0 && saved == EPIPE && !alreadyPending) {
        int signal = 0;
        ::sigwait(&set, &signal);
    }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    errno = saved;
    return written;
}
ProcessResult<std::size_t> transfer(int fd, std::span<std::byte> bytes,
                                    std::span<const std::byte> output, ProcessDeadline deadline,
                                    const ProcessCancellation& cancel) {
    const bool writing = !output.empty();
    const auto size = writing ? output.size() : bytes.size();
    std::size_t offset = 0;
    while (offset < size) {
        if (cancel && cancel())
            return ProcessFailure{ProcessError::Cancelled, 0};
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0)
            return ProcessFailure{ProcessError::Timeout, 0};
        pollfd p{fd, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        const int ready =
            ::poll(&p, 1, static_cast<int>(std::min<std::int64_t>(10, remaining.count())));
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return ProcessFailure{ProcessError::Io, errno};
        }
        if (ready == 0)
            continue;
        if ((p.revents & POLLNVAL) != 0)
            return ProcessFailure{ProcessError::Io, EBADF};
        const auto n = writing ? pipeWrite(fd, output.data() + offset, size - offset)
                               : ::read(fd, bytes.data() + offset, size - offset);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return ProcessFailure{errno == EPIPE ? ProcessError::Crashed : ProcessError::Io, errno};
        }
        if (n == 0)
            return ProcessFailure{ProcessError::Crashed, 0};
        offset += static_cast<std::size_t>(n);
    }
    return offset;
}
} // namespace
struct ProcessSupervisor::State {
    pid_t pid = -1;
    Fd input, output;
    ProcessOptions options;
    int status = 0;
    bool reaped = false;
    bool waitUntil(ProcessDeadline deadline) {
        while (!reaped) {
            const auto result = ::waitpid(pid, &status, WNOHANG);
            if (result == pid || (result < 0 && errno == ECHILD)) {
                reaped = true;
                return true;
            }
            if (result < 0 && errno != EINTR)
                return false;
            if (Clock::now() >= deadline)
                return false;
            ::poll(nullptr, 0, 2);
        }
        return true;
    }
};
ProcessSupervisor::ProcessSupervisor(std::unique_ptr<State> state) : state_(std::move(state)) {}
ProcessSupervisor::~ProcessSupervisor() { stop(); }
ProcessResult<std::unique_ptr<ProcessSupervisor>>
ProcessSupervisor::launch(const ProcessOptions& options) {
    if (options.executable.empty() || options.executable.front() != '/' ||
        options.executable.find('\0') != std::string::npos ||
        options.addressSpaceBytes < 64ULL * 1024U * 1024U || options.openFiles < 8 ||
        options.openFiles > 4096 || options.messageGrace.count() < 0 ||
        options.messageGrace > std::chrono::seconds(5) || options.termGrace.count() < 0 ||
        options.termGrace > std::chrono::seconds(5))
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    std::array<Fd, 2> input, output, gate;
    if (!makePipe(input) || !makePipe(output) || !makePipe(gate))
        return ProcessFailure{ProcessError::Spawn, errno};
    SpawnActions actions;
    SpawnAttributes attributes;
    if (actions.status != 0 || attributes.status != 0)
        return ProcessFailure{ProcessError::Spawn,
                              actions.status != 0 ? actions.status : attributes.status};
    sigset_t empty{}, defaults{};
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGPIPE);
    int code = posix_spawnattr_setflags(
        &attributes.value, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    if (code == 0)
        code = posix_spawnattr_setpgroup(&attributes.value, 0);
    if (code == 0)
        code = posix_spawnattr_setsigmask(&attributes.value, &empty);
    if (code == 0)
        code = posix_spawnattr_setsigdefault(&attributes.value, &defaults);
    if (code == 0)
        code = posix_spawn_file_actions_adddup2(&actions.value, input[0].get(), STDIN_FILENO);
    if (code == 0)
        code = posix_spawn_file_actions_adddup2(&actions.value, output[1].get(), STDOUT_FILENO);
    if (code == 0)
        code = posix_spawn_file_actions_addopen(&actions.value, STDERR_FILENO, "/dev/null",
                                                O_WRONLY, 0);
    if (code == 0)
        code = posix_spawn_file_actions_adddup2(&actions.value, gate[0].get(), 3);
    if (code != 0)
        return ProcessFailure{ProcessError::Spawn, code};
    std::vector<std::string> arguments{options.executable};
    for (const auto& arg : options.arguments) {
        if (arg.find('\0') != std::string::npos)
            return ProcessFailure{ProcessError::Spawn, EINVAL};
        arguments.push_back(arg);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& arg : arguments)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    auto state = std::make_unique<State>();
    state->options = options;
    std::vector<std::string> environment{"LC_ALL=C", "TZ=UTC"};
    for (const auto& entry : options.environment) {
        if (entry.find('\0') != std::string::npos || entry.find('=') == std::string::npos)
            return ProcessFailure{ProcessError::Spawn, EINVAL};
        const auto separator = entry.find('=');
        const auto name = entry.substr(0, separator);
        auto found = std::find_if(environment.begin(), environment.end(),
                                  [&](const auto& value) { return value.starts_with(name + '='); });
        if (found == environment.end())
            environment.push_back(entry);
        else
            *found = entry;
    }
    std::vector<char*> environmentPointers;
    environmentPointers.reserve(environment.size() + 1);
    for (auto& entry : environment)
        environmentPointers.push_back(entry.data());
    environmentPointers.push_back(nullptr);
    code = ::posix_spawn(&state->pid, options.executable.c_str(), &actions.value, &attributes.value,
                         argv.data(), environmentPointers.data());
    if (code != 0)
        return ProcessFailure{ProcessError::Spawn, code};
    state->input = std::move(input[1]);
    state->output = std::move(output[0]);
    input[0] = Fd{};
    output[1] = Fd{};
    gate[0] = Fd{};
    auto child = std::unique_ptr<ProcessSupervisor>(new ProcessSupervisor(std::move(state)));
    for (const int fd : {child->state_->input.get(), child->state_->output.get()}) {
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
            return ProcessFailure{ProcessError::Io, errno};
    }
    // The gate keeps the worker protocol identical to Linux: the child's bootstrap waits for 'R'
    // before loading provider code. macOS has no kernel rlimit to install between spawn and the
    // ready byte, so the byte simply releases the child.
    constexpr char ready = 'R';
    if (pipeWrite(gate[1].get(), &ready, 1) != 1)
        return ProcessFailure{ProcessError::Spawn, errno};
    return child;
}
ProcessResult<std::size_t> ProcessSupervisor::write(std::span<const std::byte> bytes,
                                                    ProcessDeadline deadline,
                                                    const ProcessCancellation& cancel) {
    return transfer(state_->input.get(), {}, bytes, deadline, cancel);
}
ProcessResult<std::size_t> ProcessSupervisor::read(std::span<std::byte> bytes,
                                                   ProcessDeadline deadline,
                                                   const ProcessCancellation& cancel) {
    return transfer(state_->output.get(), bytes, {}, deadline, cancel);
}
ProcessResult<int> ProcessSupervisor::finish(ProcessDeadline deadline) {
    state_->input = Fd{};
    if (!state_->waitUntil(deadline)) {
        stop();
        return ProcessFailure{ProcessError::Timeout, 0};
    }
    if (!WIFEXITED(state_->status) || WEXITSTATUS(state_->status) != 0)
        return ProcessFailure{ProcessError::Crashed, state_->status};
    return 0;
}
void ProcessSupervisor::stop(std::span<const std::byte> message) {
    if (state_->reaped)
        return;
    if (!message.empty()) {
        const auto ignored = write(message, Clock::now() + state_->options.messageGrace, {});
        (void)ignored;
    }
    state_->input = Fd{};
    if (state_->waitUntil(Clock::now() + state_->options.messageGrace))
        return;
    ::kill(-state_->pid, SIGTERM);
    if (state_->waitUntil(Clock::now() + state_->options.termGrace))
        return;
    ::kill(-state_->pid, SIGKILL);
    while (::waitpid(state_->pid, &state_->status, 0) < 0 && errno == EINTR) {
    }
    state_->reaped = true;
}
std::int64_t ProcessSupervisor::processId() const { return state_->pid; }
bool processWorkerBootstrap() {
    char ready = 0;
    ssize_t n = 0;
    do {
        n = ::read(3, &ready, 1);
    } while (n < 0 && errno == EINTR);
    ::close(3);
    return n == 1 && ready == 'R';
}
} // namespace bloom::platform
