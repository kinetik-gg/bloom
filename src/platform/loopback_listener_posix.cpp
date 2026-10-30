// Loopback TCP sockets on Linux and macOS (ADR 0034). See bloom/platform/loopback_listener.hpp.
#include <bloom/platform/loopback_listener.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bloom::platform {
namespace {

#if defined(__linux__)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

[[maybe_unused]] bool makeCloexecNonBlocking(const int fd) noexcept {
    const int descriptorFlags = ::fcntl(fd, F_GETFD);
    const int statusFlags = ::fcntl(fd, F_GETFL);
    return descriptorFlags >= 0 && statusFlags >= 0 &&
           ::fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC) == 0 &&
           ::fcntl(fd, F_SETFL, statusFlags | O_NONBLOCK) == 0;
}

int newStreamSocket() noexcept {
#if defined(__linux__)
    return ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#else
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0 && !makeCloexecNonBlocking(fd)) {
        ::close(fd);
        return -1;
    }
    return fd;
#endif
}

void suppressSigpipe([[maybe_unused]] const int fd) noexcept {
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one));
#endif
}

// A pipe whose write end wakes a poll() on its read end from any thread.
struct WakePipe final {
    int read = -1;
    int write = -1;
    bool open() noexcept {
        int fds[2] = {-1, -1};
#if defined(__linux__)
        if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
            return false;
#else
        if (::pipe(fds) != 0)
            return false;
        if (!makeCloexecNonBlocking(fds[0]) || !makeCloexecNonBlocking(fds[1])) {
            ::close(fds[0]);
            ::close(fds[1]);
            return false;
        }
#endif
        read = fds[0];
        write = fds[1];
        return true;
    }
    void signal() const noexcept {
        if (write >= 0) {
            const char byte = 1;
            static_cast<void>(::write(write, &byte, 1));
        }
    }
    void drain() const noexcept {
        char bytes[64];
        while (read >= 0 && ::read(read, bytes, sizeof bytes) > 0) {
        }
    }
    void close() noexcept {
        if (read >= 0)
            ::close(read);
        if (write >= 0)
            ::close(write);
        read = write = -1;
    }
};

std::string portMessage(const LoopbackBindStatus status, const std::uint16_t port) {
    const auto name = "Port " + std::to_string(port);
    switch (status) {
    case LoopbackBindStatus::Bound:
        return {};
    case LoopbackBindStatus::PortInUse:
        return name + " is in use.";
    case LoopbackBindStatus::PermissionDenied:
        return name + " is not allowed for this user.";
    case LoopbackBindStatus::AddressUnavailable:
        return "This computer has no loopback network address.";
    case LoopbackBindStatus::ResourceExhausted:
        return "The system is out of network resources.";
    case LoopbackBindStatus::InvalidPort:
        return name + " can't be used.";
    case LoopbackBindStatus::SystemError:
        return name + " could not be opened.";
    }
    return name + " could not be opened.";
}

LoopbackBindStatus bindStatus(const int error) noexcept {
    switch (error) {
    case EADDRINUSE:
        return LoopbackBindStatus::PortInUse;
    case EACCES:
    case EPERM:
        return LoopbackBindStatus::PermissionDenied;
    case EADDRNOTAVAIL:
        return LoopbackBindStatus::AddressUnavailable;
    case EMFILE:
    case ENFILE:
    case ENOBUFS:
    case ENOMEM:
        return LoopbackBindStatus::ResourceExhausted;
    default:
        return LoopbackBindStatus::SystemError;
    }
}

int pollTimeout(const std::chrono::steady_clock::time_point deadline) noexcept {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(left.count(), 0, 60000));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Connection

struct LoopbackConnection::Native final {
    int fd = -1;
    std::uint16_t peer = 0;
    std::atomic_bool closed = false;
    ~Native() {
        if (fd >= 0)
            ::close(fd);
    }
    // shutdown() wakes a poll() blocked on this socket in another thread on Linux and macOS
    // without racing a descriptor close; the descriptor is released by the destructor.
    void interrupt() noexcept {
        if (!closed.exchange(true) && fd >= 0)
            static_cast<void>(::shutdown(fd, SHUT_RDWR));
    }
};

LoopbackConnection::LoopbackConnection(std::unique_ptr<Native> native) noexcept
    : native_(std::move(native)) {}
LoopbackConnection::LoopbackConnection(LoopbackConnection&&) noexcept = default;
LoopbackConnection& LoopbackConnection::operator=(LoopbackConnection&&) noexcept = default;
LoopbackConnection::~LoopbackConnection() = default;

LoopbackReadResult LoopbackConnection::read(const std::span<std::byte> buffer,
                                            const std::chrono::milliseconds timeout,
                                            const std::stop_token& stop) {
    if (!native_ || native_->closed || stop.stop_requested())
        return {LoopbackIoStatus::Cancelled, 0};
    if (buffer.empty())
        return {LoopbackIoStatus::Ok, 0};
    const std::stop_callback wake(stop, [native = native_.get()] { native->interrupt(); });
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto received = ::recv(native_->fd, buffer.data(), buffer.size(), 0);
        if (received > 0)
            return {LoopbackIoStatus::Ok, static_cast<std::size_t>(received)};
        if (native_->closed || stop.stop_requested())
            return {LoopbackIoStatus::Cancelled, 0};
        if (received == 0)
            return {LoopbackIoStatus::Closed, 0};
        if (errno == EINTR)
            continue;
        if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN)
            return {LoopbackIoStatus::Closed, 0};
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return {LoopbackIoStatus::Failed, 0};
        pollfd entry{native_->fd, POLLIN, 0};
        const int wait = pollTimeout(deadline);
        if (wait == 0)
            return {LoopbackIoStatus::TimedOut, 0};
        const int ready = ::poll(&entry, 1, wait);
        if (ready < 0 && errno != EINTR)
            return {LoopbackIoStatus::Failed, 0};
        if (ready == 0 && std::chrono::steady_clock::now() >= deadline)
            return {LoopbackIoStatus::TimedOut, 0};
    }
}

LoopbackIoStatus LoopbackConnection::writeAll(std::span<const std::byte> bytes,
                                              const std::chrono::milliseconds timeout,
                                              const std::stop_token& stop) {
    if (!native_ || native_->closed || stop.stop_requested())
        return LoopbackIoStatus::Cancelled;
    const std::stop_callback wake(stop, [native = native_.get()] { native->interrupt(); });
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!bytes.empty()) {
        const auto sent = ::send(native_->fd, bytes.data(), bytes.size(), kSendFlags);
        if (sent > 0) {
            bytes = bytes.subspan(static_cast<std::size_t>(sent));
            continue;
        }
        if (native_->closed || stop.stop_requested())
            return LoopbackIoStatus::Cancelled;
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN))
            return LoopbackIoStatus::Closed;
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            return LoopbackIoStatus::Failed;
        pollfd entry{native_->fd, POLLOUT, 0};
        const int wait = pollTimeout(deadline);
        if (wait == 0)
            return LoopbackIoStatus::TimedOut;
        if (::poll(&entry, 1, wait) < 0 && errno != EINTR)
            return LoopbackIoStatus::Failed;
    }
    return LoopbackIoStatus::Ok;
}

void LoopbackConnection::shutdownWrite() noexcept {
    if (native_ && !native_->closed)
        static_cast<void>(::shutdown(native_->fd, SHUT_WR));
}

void LoopbackConnection::close() noexcept {
    if (native_)
        native_->interrupt();
}

std::uint16_t LoopbackConnection::peerPort() const noexcept {
    return native_ ? native_->peer : std::uint16_t{0};
}

// ---------------------------------------------------------------------------------------------
// Listener

struct LoopbackListener::Native final {
    int fd = -1;
    std::uint16_t port = 0;
    WakePipe wake;
    std::mutex mutex;
    bool closed = false;
    int accepting = 0;
    ~Native() { release(); }
    void release() noexcept {
        if (fd >= 0)
            ::close(fd);
        fd = -1;
        wake.close();
    }
};

LoopbackListener::LoopbackListener(std::unique_ptr<Native> native) noexcept
    : native_(std::move(native)) {}
LoopbackListener::~LoopbackListener() { close(); }

LoopbackListenResult LoopbackListener::listen(const std::uint16_t port, const int backlog) {
    LoopbackListenResult result;
    result.bind.port = port;
    const auto refuse = [&](const LoopbackBindStatus status, const int code) {
        result.bind.status = status;
        result.bind.systemCode = code;
        result.bind.message = portMessage(status, port);
        return std::move(result);
    };
    if (port == 0)
        return refuse(LoopbackBindStatus::InvalidPort, 0);
    auto native = std::make_unique<Native>();
    native->port = port;
    if (!native->wake.open())
        return refuse(bindStatus(errno), errno);
    native->fd = newStreamSocket();
    if (native->fd < 0)
        return refuse(bindStatus(errno), errno);
    // SO_REUSEADDR lets a stopped server rebind while old connections sit in TIME_WAIT. It does
    // not let two listeners share an active 127.0.0.1:<port> on Linux or macOS.
    const int one = 1;
    static_cast<void>(::setsockopt(native->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(native->fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0)
        return refuse(bindStatus(errno), errno);
    if (::listen(native->fd, std::clamp(backlog, 1, SOMAXCONN)) != 0)
        return refuse(bindStatus(errno), errno);
    result.bind.status = LoopbackBindStatus::Bound;
    result.listener.reset(new LoopbackListener(std::move(native)));
    return result;
}

std::uint16_t LoopbackListener::port() const noexcept { return native_->port; }

LoopbackAcceptResult LoopbackListener::accept(const std::chrono::milliseconds timeout,
                                              const std::stop_token& stop) {
    LoopbackAcceptResult result;
    {
        const std::lock_guard lock(native_->mutex);
        if (native_->closed || stop.stop_requested()) {
            result.status = LoopbackAcceptStatus::Stopped;
            return result;
        }
        ++native_->accepting;
    }
    struct Leave final {
        Native& native;
        ~Leave() {
            const std::lock_guard lock(native.mutex);
            if (--native.accepting == 0 && native.closed)
                native.release();
        }
    } leave{*native_};
    const std::stop_callback wake(stop, [this] { this->wake(); });
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (stop.stop_requested()) {
            result.status = LoopbackAcceptStatus::Stopped;
            return result;
        }
        pollfd entries[2] = {{native_->fd, POLLIN, 0}, {native_->wake.read, POLLIN, 0}};
        const int ready = ::poll(entries, 2, pollTimeout(deadline));
        if (ready < 0 && errno != EINTR) {
            result.status = LoopbackAcceptStatus::Failed;
            result.systemCode = errno;
            return result;
        }
        if ((entries[1].revents & POLLIN) != 0) {
            native_->wake.drain();
            result.status = LoopbackAcceptStatus::Stopped;
            return result;
        }
        {
            const std::lock_guard lock(native_->mutex);
            if (native_->closed) {
                result.status = LoopbackAcceptStatus::Stopped;
                return result;
            }
        }
        if ((entries[0].revents & (POLLERR | POLLNVAL)) != 0) {
            result.status = LoopbackAcceptStatus::Failed;
            return result;
        }
        if ((entries[0].revents & POLLIN) == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                result.status = LoopbackAcceptStatus::TimedOut;
                return result;
            }
            continue;
        }
        sockaddr_in peer{};
        socklen_t length = sizeof peer;
#if defined(__linux__)
        const int fd = ::accept4(native_->fd, reinterpret_cast<sockaddr*>(&peer), &length,
                                 SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
        int fd = ::accept(native_->fd, reinterpret_cast<sockaddr*>(&peer), &length);
        if (fd >= 0 && !makeCloexecNonBlocking(fd)) {
            ::close(fd);
            fd = -1;
            errno = EMFILE;
        }
#endif
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            result.systemCode = errno;
            result.status = errno == EBADF || errno == EINVAL || errno == ENOTSOCK
                                ? LoopbackAcceptStatus::Failed
                                : LoopbackAcceptStatus::Transient;
            return result;
        }
        // Defence in depth: the socket is bound to 127.0.0.1, so a non-loopback peer cannot
        // connect; refuse one anyway.
        if (peer.sin_family != AF_INET || (ntohl(peer.sin_addr.s_addr) >> 24U) != 127U) {
            ::close(fd);
            result.status = LoopbackAcceptStatus::Transient;
            return result;
        }
        suppressSigpipe(fd);
        const int one = 1;
        static_cast<void>(::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one));
        auto connection = std::make_unique<LoopbackConnection::Native>();
        connection->fd = fd;
        connection->peer = ntohs(peer.sin_port);
        result.status = LoopbackAcceptStatus::Accepted;
        result.connection.emplace(LoopbackConnection(std::move(connection)));
        return result;
    }
}

void LoopbackListener::wake() noexcept { native_->wake.signal(); }

void LoopbackListener::close() noexcept {
    const std::lock_guard lock(native_->mutex);
    if (native_->closed)
        return;
    native_->closed = true;
    native_->wake.signal();
    // A thread blocked in accept() releases the descriptors when it leaves; otherwise release the
    // port now so a restart can bind it immediately.
    if (native_->accepting == 0)
        native_->release();
    else if (native_->fd >= 0)
        static_cast<void>(::shutdown(native_->fd, SHUT_RDWR));
}

} // namespace bloom::platform
