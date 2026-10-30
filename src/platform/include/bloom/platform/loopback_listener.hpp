#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>

// Loopback TCP sockets for local servers (ADR 0034). Qt-free; the only socket code in Bloom.
//
// Scope is deliberately narrow: IPv4 127.0.0.1 only, stream sockets only, no name resolution, no
// TLS, no other interface. A caller can never ask for a wildcard or a routable address, so a server
// built on this boundary is unreachable from other machines by construction.
//
// Backends: POSIX sockets on Linux and macOS, Winsock 2 on Windows (initialised on first use,
// reference counted, released when the last listener or connection closes). Every socket is created
// non-inheritable (SOCK_CLOEXEC / FD_CLOEXEC on POSIX, WSA_FLAG_NO_HANDLE_INHERIT on Windows) so
// supervised worker processes never inherit it, and writes never raise SIGPIPE (MSG_NOSIGNAL on
// Linux, SO_NOSIGPIPE on macOS).
//
// Port ownership: POSIX listeners set SO_REUSEADDR so a stopped server can rebind while old
// connections sit in TIME_WAIT; it never lets two listeners share 127.0.0.1:<port> on Linux.
// Windows listeners set SO_EXCLUSIVEADDRUSE so no other process can bind the same port over them.
// A port that is taken is a typed PortInUse result; this boundary never tries another port.
//
// Threading: a listener and each connection are used by one thread at a time, except wake() and
// close(), which are safe from any thread and make a blocked accept()/read()/write() return
// promptly (Stopped/Cancelled). Every blocking call takes a timeout and a stop token.
namespace bloom::platform {

// The only address a loopback listener binds. Literal, never resolved.
inline constexpr std::string_view kLoopbackAddress = "127.0.0.1";

enum class LoopbackBindStatus : std::uint8_t {
    Bound = 0,
    // EADDRINUSE / WSAEADDRINUSE (or WSAEACCES under another process's exclusive bind).
    PortInUse = 1,
    // EACCES / EPERM: the operating system refused this port to this user.
    PermissionDenied = 2,
    // EADDRNOTAVAIL: no loopback interface is configured.
    AddressUnavailable = 3,
    // EMFILE / ENFILE / ENOBUFS / WSAENOBUFS.
    ResourceExhausted = 4,
    // Port 0 or a value the boundary does not accept (ephemeral binding is never requested).
    InvalidPort = 5,
    SystemError = 6,
};

struct LoopbackBindResult final {
    LoopbackBindStatus status = LoopbackBindStatus::SystemError;
    // The port that was requested; on Bound, the port actually bound (always the same).
    std::uint16_t port = 0;
    // errno or WSAGetLastError() value; 0 when not applicable. Never shown alone to a user.
    int systemCode = 0;
    // Plain-language, names the port ("Port 6767 is in use by another program").
    std::string message;
    [[nodiscard]] bool bound() const noexcept { return status == LoopbackBindStatus::Bound; }
};

enum class LoopbackIoStatus : std::uint8_t {
    Ok = 0,
    // The peer closed its side (read of 0 bytes) or reset the connection.
    Closed = 1,
    TimedOut = 2,
    // The stop token fired or close() was called from another thread.
    Cancelled = 3,
    Failed = 4,
};

struct LoopbackReadResult final {
    LoopbackIoStatus status = LoopbackIoStatus::Failed;
    std::size_t bytes = 0;
};

// One accepted connection. Move-only; the destructor closes the socket.
class LoopbackConnection final {
  public:
    LoopbackConnection(LoopbackConnection&&) noexcept;
    LoopbackConnection& operator=(LoopbackConnection&&) noexcept;
    LoopbackConnection(const LoopbackConnection&) = delete;
    LoopbackConnection& operator=(const LoopbackConnection&) = delete;
    ~LoopbackConnection();

    // Reads at most buffer.size() bytes, waiting up to `timeout` for the first byte.
    [[nodiscard]] LoopbackReadResult read(std::span<std::byte> buffer,
                                          std::chrono::milliseconds timeout,
                                          const std::stop_token& stop);
    // Writes every byte or reports why not; `timeout` bounds the whole write, so a client that
    // stops reading cannot hold a server thread indefinitely.
    [[nodiscard]] LoopbackIoStatus writeAll(std::span<const std::byte> bytes,
                                            std::chrono::milliseconds timeout,
                                            const std::stop_token& stop);
    // Half-close after a close-delimited response.
    void shutdownWrite() noexcept;
    // Any thread; idempotent; wakes a blocked read or write.
    void close() noexcept;
    // The peer's port on 127.0.0.1, for diagnostics only. The peer address is always loopback:
    // accept() rejects anything else.
    [[nodiscard]] std::uint16_t peerPort() const noexcept;

  private:
    struct Native;
    friend class LoopbackListener;
    explicit LoopbackConnection(std::unique_ptr<Native> native) noexcept;
    std::unique_ptr<Native> native_;
};

enum class LoopbackAcceptStatus : std::uint8_t {
    Accepted = 0,
    TimedOut = 1,
    // wake(), close() or the stop token.
    Stopped = 2,
    // A transient accept error (ECONNABORTED, EMFILE): the listener stays usable.
    Transient = 3,
    // The listening socket itself failed; the listener is unusable.
    Failed = 4,
};

struct LoopbackAcceptResult final {
    LoopbackAcceptStatus status = LoopbackAcceptStatus::Failed;
    std::optional<LoopbackConnection> connection;
    int systemCode = 0;
};

class LoopbackListener;

struct LoopbackListenResult final {
    LoopbackBindResult bind;
    // Present exactly when bind.bound().
    std::unique_ptr<LoopbackListener> listener;
};

// A listening socket on 127.0.0.1:<port>.
class LoopbackListener final {
  public:
    // Creates, binds and listens synchronously. Binding a loopback port is a bounded system call
    // with no filesystem or network I/O, so the owner thread may call it and receive a typed
    // PortInUse immediately. `backlog` is clamped to the platform maximum.
    [[nodiscard]] static LoopbackListenResult listen(std::uint16_t port, int backlog = 16);

    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;
    LoopbackListener(LoopbackListener&&) = delete;
    LoopbackListener& operator=(LoopbackListener&&) = delete;
    ~LoopbackListener();

    [[nodiscard]] std::uint16_t port() const noexcept;
    // Waits up to `timeout` for one connection. A connection whose peer is not 127.0.0.0/8 is
    // closed and reported as Transient (defence in depth; loopback binding already prevents it).
    [[nodiscard]] LoopbackAcceptResult accept(std::chrono::milliseconds timeout,
                                              const std::stop_token& stop);
    // Any thread; makes a blocked accept() return Stopped. Idempotent.
    void wake() noexcept;
    // Any thread; stops listening and releases the port. Idempotent.
    void close() noexcept;

  private:
    struct Native;
    explicit LoopbackListener(std::unique_ptr<Native> native) noexcept;
    std::unique_ptr<Native> native_;
};

} // namespace bloom::platform
