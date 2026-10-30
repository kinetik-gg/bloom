// Loopback TCP sockets on Windows through Winsock 2 (ADR 0034). See
// bloom/platform/loopback_listener.hpp.
#include <bloom/platform/loopback_listener.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

namespace bloom::platform {
namespace {

// Winsock is reference counted by the system: every listener and connection holds one startup.
struct WinsockUse final {
    bool started = false;
    WinsockUse() noexcept {
        WSADATA data{};
        started = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockUse() {
        if (started)
            ::WSACleanup();
    }
    WinsockUse(const WinsockUse&) = delete;
    WinsockUse& operator=(const WinsockUse&) = delete;
};

SOCKET newStreamSocket() noexcept {
    // WSA_FLAG_NO_HANDLE_INHERIT: supervised worker processes never inherit the socket.
    return ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

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
    // Under SO_EXCLUSIVEADDRUSE another process's bind surfaces as WSAEACCES.
    case WSAEADDRINUSE:
    case WSAEACCES:
        return LoopbackBindStatus::PortInUse;
    case WSAEADDRNOTAVAIL:
        return LoopbackBindStatus::AddressUnavailable;
    case WSAENOBUFS:
    case WSAEMFILE:
        return LoopbackBindStatus::ResourceExhausted;
    default:
        return LoopbackBindStatus::SystemError;
    }
}

DWORD waitTimeout(const std::chrono::steady_clock::time_point deadline) noexcept {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return static_cast<DWORD>(std::clamp<std::chrono::milliseconds::rep>(left.count(), 0, 60000));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Connection

struct LoopbackConnection::Native final {
    WinsockUse winsock;
    SOCKET socket = INVALID_SOCKET;
    WSAEVENT network = WSA_INVALID_EVENT;
    // Signalled by close() or the stop token from any thread.
    WSAEVENT cancel = WSA_INVALID_EVENT;
    std::uint16_t peer = 0;
    std::atomic_bool closed = false;
    ~Native() {
        if (socket != INVALID_SOCKET)
            ::closesocket(socket);
        if (network != WSA_INVALID_EVENT)
            ::WSACloseEvent(network);
        if (cancel != WSA_INVALID_EVENT)
            ::WSACloseEvent(cancel);
    }
    void interrupt() noexcept {
        if (!closed.exchange(true))
            ::WSASetEvent(cancel);
    }
    // Waits for the network event or cancellation; true when the socket may make progress.
    LoopbackIoStatus wait(const std::chrono::steady_clock::time_point deadline) const noexcept {
        const WSAEVENT events[2] = {network, cancel};
        const DWORD timeout = waitTimeout(deadline);
        if (timeout == 0)
            return LoopbackIoStatus::TimedOut;
        const DWORD which = ::WSAWaitForMultipleEvents(2, events, FALSE, timeout, FALSE);
        if (which == WSA_WAIT_TIMEOUT)
            return std::chrono::steady_clock::now() >= deadline ? LoopbackIoStatus::TimedOut
                                                                : LoopbackIoStatus::Ok;
        if (which == WSA_WAIT_EVENT_0 + 1)
            return LoopbackIoStatus::Cancelled;
        if (which != WSA_WAIT_EVENT_0)
            return LoopbackIoStatus::Failed;
        WSANETWORKEVENTS happened{};
        static_cast<void>(::WSAEnumNetworkEvents(socket, network, &happened));
        return LoopbackIoStatus::Ok;
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
    const int size = static_cast<int>(std::min<std::size_t>(buffer.size(), 1U << 30U));
    while (true) {
        const int received =
            ::recv(native_->socket, reinterpret_cast<char*>(buffer.data()), size, 0);
        if (received > 0)
            return {LoopbackIoStatus::Ok, static_cast<std::size_t>(received)};
        if (native_->closed || stop.stop_requested())
            return {LoopbackIoStatus::Cancelled, 0};
        if (received == 0)
            return {LoopbackIoStatus::Closed, 0};
        const int error = ::WSAGetLastError();
        if (error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN)
            return {LoopbackIoStatus::Closed, 0};
        if (error != WSAEWOULDBLOCK)
            return {LoopbackIoStatus::Failed, 0};
        const auto waited = native_->wait(deadline);
        if (waited != LoopbackIoStatus::Ok)
            return {waited, 0};
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
        const int size = static_cast<int>(std::min<std::size_t>(bytes.size(), 1U << 30U));
        const int sent =
            ::send(native_->socket, reinterpret_cast<const char*>(bytes.data()), size, 0);
        if (sent > 0) {
            bytes = bytes.subspan(static_cast<std::size_t>(sent));
            continue;
        }
        if (native_->closed || stop.stop_requested())
            return LoopbackIoStatus::Cancelled;
        const int error = ::WSAGetLastError();
        if (error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN ||
            error == WSAESHUTDOWN)
            return LoopbackIoStatus::Closed;
        if (error != WSAEWOULDBLOCK)
            return LoopbackIoStatus::Failed;
        const auto waited = native_->wait(deadline);
        if (waited != LoopbackIoStatus::Ok)
            return waited;
    }
    return LoopbackIoStatus::Ok;
}

void LoopbackConnection::shutdownWrite() noexcept {
    if (native_ && !native_->closed)
        static_cast<void>(::shutdown(native_->socket, SD_SEND));
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
    WinsockUse winsock;
    SOCKET socket = INVALID_SOCKET;
    WSAEVENT network = WSA_INVALID_EVENT;
    WSAEVENT wake = WSA_INVALID_EVENT;
    std::uint16_t port = 0;
    std::mutex mutex;
    bool closed = false;
    int accepting = 0;
    ~Native() {
        release();
        if (wake != WSA_INVALID_EVENT)
            ::WSACloseEvent(wake);
    }
    void release() noexcept {
        if (socket != INVALID_SOCKET)
            ::closesocket(socket);
        socket = INVALID_SOCKET;
        if (network != WSA_INVALID_EVENT)
            ::WSACloseEvent(network);
        network = WSA_INVALID_EVENT;
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
    if (!native->winsock.started)
        return refuse(LoopbackBindStatus::SystemError, 0);
    native->socket = newStreamSocket();
    if (native->socket == INVALID_SOCKET)
        return refuse(bindStatus(::WSAGetLastError()), ::WSAGetLastError());
    // No other process can bind the same port over this listener.
    const BOOL exclusive = TRUE;
    static_cast<void>(::setsockopt(native->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                                   reinterpret_cast<const char*>(&exclusive), sizeof exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::bind(native->socket, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        const int error = ::WSAGetLastError();
        return refuse(bindStatus(error), error);
    }
    if (::listen(native->socket, std::clamp(backlog, 1, SOMAXCONN)) != 0) {
        const int error = ::WSAGetLastError();
        return refuse(bindStatus(error), error);
    }
    native->network = ::WSACreateEvent();
    native->wake = ::WSACreateEvent();
    if (native->network == WSA_INVALID_EVENT || native->wake == WSA_INVALID_EVENT ||
        ::WSAEventSelect(native->socket, native->network, FD_ACCEPT | FD_CLOSE) != 0)
        return refuse(LoopbackBindStatus::ResourceExhausted, ::WSAGetLastError());
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
        sockaddr_in peer{};
        int length = sizeof peer;
        const SOCKET accepted =
            ::accept(native_->socket, reinterpret_cast<sockaddr*>(&peer), &length);
        if (accepted == INVALID_SOCKET) {
            const int error = ::WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                result.systemCode = error;
                result.status = error == WSAENOTSOCK || error == WSAEINVAL
                                    ? LoopbackAcceptStatus::Failed
                                    : LoopbackAcceptStatus::Transient;
                return result;
            }
            const WSAEVENT events[2] = {native_->network, native_->wake};
            const DWORD wait = waitTimeout(deadline);
            if (wait == 0) {
                result.status = LoopbackAcceptStatus::TimedOut;
                return result;
            }
            const DWORD which = ::WSAWaitForMultipleEvents(2, events, FALSE, wait, FALSE);
            if (which == WSA_WAIT_EVENT_0 + 1) {
                ::WSAResetEvent(native_->wake);
                result.status = LoopbackAcceptStatus::Stopped;
                return result;
            }
            if (which == WSA_WAIT_EVENT_0) {
                WSANETWORKEVENTS happened{};
                static_cast<void>(
                    ::WSAEnumNetworkEvents(native_->socket, native_->network, &happened));
            } else if (which != WSA_WAIT_TIMEOUT) {
                result.status = LoopbackAcceptStatus::Failed;
                return result;
            }
            continue;
        }
        if (peer.sin_family != AF_INET || (::ntohl(peer.sin_addr.s_addr) >> 24U) != 127U) {
            ::closesocket(accepted);
            result.status = LoopbackAcceptStatus::Transient;
            return result;
        }
        static_cast<void>(
            ::SetHandleInformation(reinterpret_cast<HANDLE>(accepted), HANDLE_FLAG_INHERIT, 0));
        auto connection = std::make_unique<LoopbackConnection::Native>();
        connection->socket = accepted;
        connection->peer = ::ntohs(peer.sin_port);
        connection->network = ::WSACreateEvent();
        connection->cancel = ::WSACreateEvent();
        // A socket from accept() inherits the listener's event selection; give it its own.
        if (connection->network == WSA_INVALID_EVENT || connection->cancel == WSA_INVALID_EVENT ||
            ::WSAEventSelect(accepted, connection->network, FD_READ | FD_WRITE | FD_CLOSE) != 0) {
            result.status = LoopbackAcceptStatus::Transient;
            return result;
        }
        const BOOL noDelay = TRUE;
        static_cast<void>(::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                                       reinterpret_cast<const char*>(&noDelay), sizeof noDelay));
        result.status = LoopbackAcceptStatus::Accepted;
        result.connection.emplace(LoopbackConnection(std::move(connection)));
        return result;
    }
}

void LoopbackListener::wake() noexcept {
    if (native_->wake != WSA_INVALID_EVENT)
        ::WSASetEvent(native_->wake);
}

void LoopbackListener::close() noexcept {
    const std::lock_guard lock(native_->mutex);
    if (native_->closed)
        return;
    native_->closed = true;
    if (native_->wake != WSA_INVALID_EVENT)
        ::WSASetEvent(native_->wake);
    if (native_->accepting == 0)
        native_->release();
}

} // namespace bloom::platform
