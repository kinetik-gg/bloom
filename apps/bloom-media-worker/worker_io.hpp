#pragma once
#include <array>
#include <bloom/media/provider/protocol.hpp>
#include <iostream>
namespace bloom::media::worker {
inline provider::Result<provider::Message> read() {
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
