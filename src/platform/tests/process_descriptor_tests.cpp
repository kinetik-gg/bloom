#include <bloom/platform/process_supervisor.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>

namespace {
// This executable also acts as a codec-free trusted worker. Bootstrap proves the real fd3 gate;
// the exchange proves stdin/stdout are intact and the absent source capability leaves fd4 closed.
int worker() {
    if (!bloom::platform::processWorkerBootstrap())
        return 1;
    errno = 0;
    if (::fcntl(4, F_GETFD) != -1 || errno != EBADF)
        return 2;
    char received = 0;
    if (::read(STDIN_FILENO, &received, 1) != 1 || received != 'Q')
        return 3;
    constexpr char reply = 'R';
    return ::write(STDOUT_FILENO, &reply, 1) == 1 ? 0 : 4;
}

int exchange(const std::string& executable, const bool occupied) {
    // This runs in a dedicated forked harness, before any worker threads. Its descriptor setup
    // cannot close or replace a descriptor in CTest or in the parent test process.
    ::close(4);
    if (occupied) {
        const int source = ::open("/dev/null", O_RDONLY);
        if (source < 0)
            return 10;
        const int copied = ::dup2(source, 4);
        if (source != 4)
            ::close(source);
        if (copied != 4)
            return 11;
    }
    bloom::platform::ProcessOptions options;
    options.executable = executable;
    options.arguments = {"--descriptor-worker"};
    auto launched = bloom::platform::ProcessSupervisor::launch(options);
    auto* process = std::get_if<std::unique_ptr<bloom::platform::ProcessSupervisor>>(&launched);
    if (!process) {
        const auto failure = std::get<bloom::platform::ProcessFailure>(launched);
        std::fprintf(stderr, "descriptor launch failed: code=%u native=%d\n",
                     static_cast<unsigned>(failure.code), failure.nativeCode);
        return 12;
    }
    errno = 0;
    const int flags = ::fcntl(4, F_GETFD);
    if ((occupied && flags < 0) || (!occupied && (flags != -1 || errno != EBADF)))
        return 13;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    const std::array query{std::byte{'Q'}};
    std::array<std::byte, 1> reply{};
    const auto written = (*process)->write(query, deadline, {});
    const auto read = (*process)->read(reply, deadline, {});
    const auto finished = (*process)->finish(deadline);
    return std::holds_alternative<std::size_t>(written) && std::get<std::size_t>(written) == 1 &&
                   std::holds_alternative<std::size_t>(read) && std::get<std::size_t>(read) == 1 &&
                   reply.front() == std::byte{'R'} && std::holds_alternative<int>(finished) &&
                   std::get<int>(finished) == 0
               ? 0
               : 14;
}

bool isolatedExchange(const std::string& executable, const bool occupied) {
    const pid_t harness = ::fork();
    if (harness < 0)
        return false;
    if (harness == 0)
        ::_exit(exchange(executable, occupied));
    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(harness, &status, 0);
    } while (waited < 0 && errno == EINTR);
    const bool passed = waited == harness && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!passed)
        std::fprintf(stderr, "fd4 %s harness failed: status=%d\n", occupied ? "occupied" : "closed",
                     status);
    return passed;
}
} // namespace

int main(const int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--descriptor-worker")
            return worker();
        const auto executable = std::filesystem::absolute(argv[0]).string();
        return isolatedExchange(executable, false) && isolatedExchange(executable, true) ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "descriptor fixture failed: %s\n", error.what());
        return 1;
    }
}
