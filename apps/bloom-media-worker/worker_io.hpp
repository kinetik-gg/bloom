#pragma once
#include <array>
#include <bloom/media/provider/protocol.hpp>
#include <chrono>
#include <iostream>
#if defined(__linux__) || defined(__APPLE__)
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace bloom::media::worker {
inline provider::Result<provider::Message>
read(std::chrono::milliseconds idleTimeout = std::chrono::milliseconds{0}) {
#if defined(__linux__) || defined(__APPLE__)
    if (idleTimeout.count() > 0) {
        const auto deadline = std::chrono::steady_clock::now() + idleTimeout;
        pollfd input{STDIN_FILENO, POLLIN, 0};
        while (true) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
                return provider::Unavailable{provider::Error::Timeout,
                                             "Worker idle lifetime expired"};
            const int status = ::poll(&input, 1, static_cast<int>(remaining.count()));
            if (status > 0)
                break;
            if (status == 0)
                return provider::Unavailable{provider::Error::Timeout,
                                             "Worker idle lifetime expired"};
            if (errno != EINTR)
                return provider::Unavailable{provider::Error::Io, "Worker input poll failed"};
        }
    }
#elif defined(_WIN32)
    // Anonymous/named pipe stdin: wait for readable bytes or a closed pipe in short steps.
    if (idleTimeout.count() > 0) {
        const auto deadline = std::chrono::steady_clock::now() + idleTimeout;
        auto* input = GetStdHandle(STD_INPUT_HANDLE);
        while (true) {
            DWORD available = 0;
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr) || available > 0)
                break; // data, or a closed pipe the read below reports
            if (std::chrono::steady_clock::now() >= deadline)
                return provider::Unavailable{provider::Error::Timeout,
                                             "Worker idle lifetime expired"};
            Sleep(5);
        }
    }
#else
    (void)idleTimeout;
#endif
    std::array<std::byte, 4> prefix{};
    if (!std::cin.read(reinterpret_cast<char*>(prefix.data()), 4))
        return provider::Unavailable{provider::Error::Truncated, "Worker input closed"};
    const auto length = provider::messageLength(prefix);
    if (const auto* e = std::get_if<provider::Unavailable>(&length))
        return *e;
    provider::Bytes bytes(prefix.begin(), prefix.end());
    bytes.resize(4U + std::get<std::uint32_t>(length));
    if (!std::cin.read(reinterpret_cast<char*>(bytes.data() + 4),
                       static_cast<std::streamsize>(bytes.size() - 4)))
        return provider::Unavailable{provider::Error::Truncated, "Worker input truncated"};
    return provider::decodeMessage(bytes);
}
inline bool write(const provider::Message& message) {
    const auto encoded = provider::encodeMessage(message);
    const auto* bytes = std::get_if<provider::Bytes>(&encoded);
    if (!bytes)
        return false;
    std::cout.write(reinterpret_cast<const char*>(bytes->data()),
                    static_cast<std::streamsize>(bytes->size()));
    std::cout.flush();
    return static_cast<bool>(std::cout);
}
} // namespace bloom::media::worker
