// bloom.platform.loopback-listener (ADR 0034, section 10): a taken port is a typed PortInUse naming
// the port with no other port tried, a stopped listener's port rebinds at once, wake() unblocks
// accept(), close() from another thread unblocks read(), and the API cannot express a non-loopback
// bind.
#include <bloom/platform/loopback_listener.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace bloom::platform;
using namespace std::chrono_literals;

// Compile-time shape: the only bind parameter is a port; there is no address, interface or host
// name parameter through which a caller could ask for a wildcard or a routable address.
static_assert(std::is_invocable_r_v<LoopbackListenResult, decltype(&LoopbackListener::listen),
                                    std::uint16_t, int>);
static_assert(!std::is_invocable_v<decltype(&LoopbackListener::listen), const char*, int>);
static_assert(!std::is_invocable_v<decltype(&LoopbackListener::listen), std::string, int>);
static_assert(kLoopbackAddress == "127.0.0.1");

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
void closeSocket(const Socket socket) { ::closesocket(socket); }
struct Startup final {
    Startup() {
        WSADATA data{};
        ::WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~Startup() { ::WSACleanup(); }
};
#else
using Socket = int;
constexpr Socket kInvalid = -1;
void closeSocket(const Socket socket) { ::close(socket); }
struct Startup final {};
#endif

#if defined(_WIN32)
using Length = int;
#else
using Length = std::size_t;
#endif

Socket connectTo(const std::uint16_t port) {
    const Socket socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket == kInvalid)
        return kInvalid;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        closeSocket(socket);
        return kInvalid;
    }
    return socket;
}

// A port this test may bind, found by binding (never by guessing that one is free).
std::unique_ptr<LoopbackListener> firstFreeListener() {
    for (std::uint16_t port = 41000; port < 42000; ++port) {
        auto result = LoopbackListener::listen(port);
        if (result.bind.bound())
            return std::move(result.listener);
    }
    throw std::runtime_error("no free loopback port in 41000..41999");
}

void portInUseNamesThePort() {
    auto listener = firstFreeListener();
    const auto port = listener->port();
    const auto second = LoopbackListener::listen(port);
    expect(!second.bind.bound() && second.listener == nullptr, "a second listener is refused");
    expect(second.bind.status == LoopbackBindStatus::PortInUse, "the refusal is PortInUse");
    expect(second.bind.port == port, "the refusal names the requested port, not another one");
    expect(second.bind.message == "Port " + std::to_string(port) + " is in use.",
           "the message names the port in plain words");
    expect(second.bind.message.find("bind") == std::string::npos &&
               second.bind.message.find("socket") == std::string::npos,
           "the message has no socket vocabulary");
    const auto zero = LoopbackListener::listen(0);
    expect(zero.bind.status == LoopbackBindStatus::InvalidPort,
           "an ephemeral port is never requested");
}

void stopThenStartRebindsImmediately() {
    auto listener = firstFreeListener();
    const auto port = listener->port();
    // Leave a connection behind so the port has state when it is released.
    const auto client = connectTo(port);
    auto accepted = listener->accept(2s, {});
    expect(accepted.status == LoopbackAcceptStatus::Accepted, "a loopback client is accepted");
    closeSocket(client);
    accepted.connection.reset();
    listener->close();
    listener.reset();
    const auto again = LoopbackListener::listen(port);
    expect(again.bind.bound() && again.listener && again.listener->port() == port,
           "the same port rebinds immediately after stop");
}

void wakeUnblocksAccept() {
    auto listener = firstFreeListener();
    std::atomic_bool returned = false;
    LoopbackAcceptStatus status = LoopbackAcceptStatus::Failed;
    std::thread waiter([&] {
        status = listener->accept(30s, {}).status;
        returned = true;
    });
    std::this_thread::sleep_for(50ms);
    expect(!returned, "accept() blocks while nobody connects");
    listener->wake();
    waiter.join();
    expect(status == LoopbackAcceptStatus::Stopped, "wake() makes accept() return Stopped");

    std::stop_source stop;
    std::thread stopped([&] { status = listener->accept(30s, stop.get_token()).status; });
    std::this_thread::sleep_for(20ms);
    stop.request_stop();
    stopped.join();
    expect(status == LoopbackAcceptStatus::Stopped, "a stop token makes accept() return");
    const auto timed = listener->accept(20ms, {});
    expect(timed.status == LoopbackAcceptStatus::TimedOut, "accept() honours its timeout");
}

void closeUnblocksRead() {
    auto listener = firstFreeListener();
    const auto client = connectTo(listener->port());
    auto accepted = listener->accept(2s, {});
    expect(accepted.connection.has_value(), "connection accepted");
    if (!accepted.connection) {
        closeSocket(client);
        return;
    }
    auto& connection = *accepted.connection;
    LoopbackIoStatus status = LoopbackIoStatus::Ok;
    std::thread reader([&] {
        std::array<std::byte, 16> buffer{};
        status = connection.read(buffer, 30s, {}).status;
    });
    std::this_thread::sleep_for(50ms);
    connection.close();
    reader.join();
    expect(status == LoopbackIoStatus::Cancelled, "close() from another thread unblocks read()");

    // Round trip and peer close on a fresh connection.
    const auto second = connectTo(listener->port());
    auto next = listener->accept(2s, {});
    expect(next.connection.has_value(), "second connection accepted");
    if (next.connection) {
        const std::string hello = "hello";
        static_cast<void>(::send(second, hello.data(), static_cast<Length>(hello.size()), 0));
        std::array<std::byte, 16> buffer{};
        const auto read = next.connection->read(buffer, 2s, {});
        expect(read.status == LoopbackIoStatus::Ok && read.bytes == hello.size(), "bytes arrive");
        const std::string reply = "world";
        expect(next.connection->writeAll(std::as_bytes(std::span(reply.data(), reply.size())), 2s,
                                         {}) == LoopbackIoStatus::Ok,
               "writeAll succeeds");
        std::array<char, 16> received{};
        const auto got = ::recv(second, received.data(), static_cast<Length>(received.size()), 0);
        static_cast<void>(got);
        expect(got == 5 && std::string(received.data(), 5) == reply, "the client reads it");
        expect(next.connection->peerPort() != 0, "the peer port is reported");
        closeSocket(second);
        expect(next.connection->read(buffer, 2s, {}).status == LoopbackIoStatus::Closed,
               "a peer close reads as Closed");
        expect(next.connection->read(buffer, 20ms, {}).status != LoopbackIoStatus::Ok,
               "nothing more arrives");
    }
    const auto quiet = connectTo(listener->port());
    auto idle = listener->accept(2s, {});
    if (idle.connection) {
        std::array<std::byte, 4> buffer{};
        expect(idle.connection->read(buffer, 30ms, {}).status == LoopbackIoStatus::TimedOut,
               "read() honours its timeout");
    }
    closeSocket(quiet);
    closeSocket(client);
}

} // namespace

int main() {
    try {
        [[maybe_unused]] const Startup startup;
        portInUseNamesThePort();
        stopThenStartRebindsImmediately();
        wakeUnblocksAccept();
        closeUnblocksRead();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "loopback listener: port in use, rebind, wake, close and I/O passed\n";
    return failures == 0 ? 0 : 1;
}
