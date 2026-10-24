#include <bloom/runtime/cryptomatte_matte_list.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace bloom::runtime {
namespace {

[[nodiscard]] std::uint32_t rotateLeft(const std::uint32_t value, const int shift) noexcept {
    return (value << shift) | (value >> (32 - shift));
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

} // namespace

std::uint32_t cryptomatteNameId(const std::string_view name) noexcept {
    // MurmurHash3_x86_32, seed 0.
    constexpr std::uint32_t c1 = 0xcc9e2d51U;
    constexpr std::uint32_t c2 = 0x1b873593U;
    std::uint32_t h = 0;
    const auto* data = reinterpret_cast<const unsigned char*>(name.data());
    const auto length = name.size();
    const auto blocks = length / 4;
    for (std::size_t block = 0; block < blocks; ++block) {
        std::uint32_t k = static_cast<std::uint32_t>(data[block * 4]) |
                          (static_cast<std::uint32_t>(data[block * 4 + 1]) << 8U) |
                          (static_cast<std::uint32_t>(data[block * 4 + 2]) << 16U) |
                          (static_cast<std::uint32_t>(data[block * 4 + 3]) << 24U);
        k *= c1;
        k = rotateLeft(k, 15);
        k *= c2;
        h ^= k;
        h = rotateLeft(h, 13);
        h = h * 5U + 0xe6546b64U;
    }
    const auto* tail = data + blocks * 4;
    std::uint32_t k = 0;
    switch (length & 3U) {
    case 3:
        k ^= static_cast<std::uint32_t>(tail[2]) << 16U;
        [[fallthrough]];
    case 2:
        k ^= static_cast<std::uint32_t>(tail[1]) << 8U;
        [[fallthrough]];
    case 1:
        k ^= tail[0];
        k *= c1;
        k = rotateLeft(k, 15);
        k *= c2;
        h ^= k;
        break;
    default:
        break;
    }
    h ^= static_cast<std::uint32_t>(length);
    h ^= h >> 16U;
    h *= 0x85ebca6bU;
    h ^= h >> 13U;
    h *= 0xc2b2ae35U;
    h ^= h >> 16U;
    // Keep the float finite and normal (the specification's exponent rule).
    const auto exponent = (h >> 23U) & 0xFFU;
    if (exponent == 0U || exponent == 0xFFU)
        h ^= 1U << 23U;
    return h;
}

std::vector<std::string> splitCryptomatteMatteList(const std::string_view list) {
    std::vector<std::string> entries;
    std::string current;
    const auto flush = [&] {
        const auto trimmed = trim(current);
        if (!trimmed.empty())
            entries.emplace_back(trimmed);
        current.clear();
    };
    for (std::size_t index = 0; index < list.size(); ++index) {
        const char c = list[index];
        if (c == '\\' && index + 1 < list.size() &&
            (list[index + 1] == ',' || list[index + 1] == '\\')) {
            current.push_back(list[++index]);
        } else if (c == ',') {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();
    return entries;
}

std::string joinCryptomatteMatteList(const std::span<const std::string> mattes) {
    std::string list;
    for (const auto& matte : mattes) {
        if (!list.empty())
            list += ", ";
        for (const char c : matte) {
            if (c == ',' || c == '\\')
                list.push_back('\\');
            list.push_back(c);
        }
    }
    return list;
}

std::optional<std::uint32_t> parseCryptomatteRawId(const std::string_view entry) noexcept {
    if (entry.size() < 3 || entry.front() != '<' || entry.back() != '>')
        return std::nullopt;
    const auto body = entry.substr(1, entry.size() - 2);
    if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) {
        std::uint32_t value = 0;
        const auto parsed = std::from_chars(body.data() + 2, body.data() + body.size(), value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != body.data() + body.size())
            return std::nullopt;
        return value;
    }
    const std::string text(body);
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(value))
        return std::nullopt;
    return std::bit_cast<std::uint32_t>(value);
}

std::string formatCryptomatteRawId(const std::uint32_t id) {
    // Nuke writes raw IDs as their float value; nine significant digits round-trip binary32.
    std::array<char, 48> buffer{};
    const auto written = std::snprintf(buffer.data(), buffer.size(), "<%.9g>",
                                       static_cast<double>(std::bit_cast<float>(id)));
    return std::string(buffer.data(), static_cast<std::size_t>(std::max(written, 0)));
}

} // namespace bloom::runtime
