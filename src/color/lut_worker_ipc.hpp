#pragma once
#include "lut_worker_protocol.hpp"
#ifdef __linux__
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>

namespace bloom::color::detail {
// Monotonic per-process generation for the abstract descriptor socket name. Shared so two callers
// in one process (CPU processor preparation and GPU program extraction) can never collide on a
// name.
inline std::uint64_t nextLutGeneration() noexcept {
    static std::atomic<std::uint64_t> generations{0};
    return ++generations;
}
inline sockaddr_un lutSocketAddress(const std::uint64_t pid, const std::uint64_t generation) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto name = "bloom-color-" + std::to_string(pid) + "-" + std::to_string(generation);
    std::copy(name.begin(), name.end(), address.sun_path + 1);
    return address;
}
inline bool sendDescriptors(const int socket, const std::span<const int> descriptors) {
    if (descriptors.empty() || descriptors.size() > 2)
        return false;
    char marker = 'F';
    iovec io{&marker, 1};
    alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(2 * sizeof(int))> control{};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = CMSG_SPACE(descriptors.size_bytes());
    auto* header = CMSG_FIRSTHDR(&message);
    if (!header)
        return false;
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(descriptors.size_bytes());
    std::memcpy(CMSG_DATA(header), descriptors.data(), descriptors.size_bytes());
    return ::sendmsg(socket, &message, MSG_NOSIGNAL | MSG_DONTWAIT) == 1;
}
inline std::array<int, 2> receiveDescriptors(const int socket, const std::size_t count) {
    char marker = 0;
    iovec io{&marker, 1};
    alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(2 * sizeof(int))> control{};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    if (::recvmsg(socket, &message, MSG_CMSG_CLOEXEC) != 1 || marker != 'F')
        return {-1, -1};
    const auto* header = CMSG_FIRSTHDR(&message);
    std::array<int, 2> descriptors{-1, -1};
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len < CMSG_LEN(0) ||
        header->cmsg_len > CMSG_LEN(descriptors.size() * sizeof(int)))
        return descriptors;
    const auto bytes = header->cmsg_len - CMSG_LEN(0);
    std::memcpy(descriptors.data(), CMSG_DATA(header), bytes);
    if (bytes != count * sizeof(int) || message.msg_flags != MSG_CMSG_CLOEXEC) {
        for (const auto fd : descriptors)
            if (fd >= 0)
                ::close(fd);
        return {-1, -1};
    }
    return descriptors;
}
} // namespace bloom::color::detail
#endif
