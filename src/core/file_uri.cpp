#include <bloom/core/file_uri.hpp>

#include <bloom/core/utf8.hpp>

namespace bloom::core {
namespace {
bool alpha(const char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
bool digit(const char c) { return c >= '0' && c <= '9'; }
bool unreserved(const char c) {
    return alpha(c) || digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}
int hexDigit(const char c) {
    if (digit(c))
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}
std::string encode(std::string_view path) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string result;
    result.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        const auto byte = static_cast<unsigned char>(path[i]);
        if (unreserved(path[i]) || path[i] == '/' ||
            (path[i] == ':' && i == 2 && path[0] == '/' && alpha(path[1]))) {
            result.push_back(path[i]);
        } else {
            result.push_back('%');
            result.push_back(hex[byte >> 4U]);
            result.push_back(hex[byte & 15U]);
        }
    }
    return result;
}
bool validAuthority(std::string_view authority) {
    if (authority.empty())
        return true;
    if (authority.front() == '.' || authority.back() == '.')
        return false;
    for (const char c : authority)
        if (!alpha(c) && !digit(c) && c != '-' && c != '.' && c != '_')
            return false;
    return true;
}
bool localHost(std::string_view authority) {
    constexpr std::string_view localhost = "localhost";
    if (authority.size() != localhost.size())
        return false;
    for (std::size_t i = 0; i < authority.size(); ++i) {
        const char c = authority[i];
        if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != localhost[i])
            return false;
    }
    return true;
}
bool fileScheme(std::string_view uri) {
    constexpr std::string_view scheme = "file:";
    if (uri.size() < scheme.size())
        return false;
    for (std::size_t i = 0; i < scheme.size(); ++i) {
        const char c = uri[i];
        if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != scheme[i])
            return false;
    }
    return true;
}
} // namespace

std::optional<std::string> fileUriFromAbsolutePath(const std::filesystem::path& path) {
    if (!path.is_absolute())
        return std::nullopt;
    const auto u8 = path.generic_u8string();
    std::string raw(reinterpret_cast<const char*>(u8.data()), u8.size());
    if (raw.find('\0') != std::string::npos || !isValidUtf8(raw))
        return std::nullopt;
#if defined(_WIN32)
    if (raw.starts_with("//")) {
        const auto slash = raw.find('/', 2);
        if (slash == std::string::npos || slash == 2 || !validAuthority(
                std::string_view(raw).substr(2, slash - 2)))
            return std::nullopt;
        return "file://" + raw.substr(2, slash - 2) + encode(std::string_view(raw).substr(slash));
    }
    if (raw.size() < 3 || !alpha(raw[0]) || raw[1] != ':' || raw[2] != '/')
        return std::nullopt;
    raw.insert(raw.begin(), '/');
#else
    if (raw.empty() || raw.front() != '/')
        return std::nullopt;
#endif
    return "file://" + encode(raw);
}

std::optional<std::filesystem::path> absolutePathFromFileUri(std::string_view uri) {
    if (!fileScheme(uri))
        return std::nullopt;
    uri.remove_prefix(5);
    std::string_view authority;
    if (uri.starts_with("//")) {
        uri.remove_prefix(2);
        const auto slash = uri.find('/');
        if (slash == std::string_view::npos)
            return std::nullopt;
        authority = uri.substr(0, slash);
        uri.remove_prefix(slash);
    }
    if (!validAuthority(authority) || uri.empty() || uri.front() != '/' ||
        uri.find_first_of("?#\\") != std::string_view::npos ||
        uri.find('\0') != std::string_view::npos)
        return std::nullopt;
    std::string decoded;
    decoded.reserve(uri.size() + authority.size() + 2);
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%') {
            if (i + 2 >= uri.size())
                return std::nullopt;
            const int high = hexDigit(uri[i + 1]);
            const int low = hexDigit(uri[i + 2]);
            if (high < 0 || low < 0)
                return std::nullopt;
            const char value = static_cast<char>((high << 4) | low);
            if (value == '\0' || value == '/' || value == '\\')
                return std::nullopt;
            decoded.push_back(value);
            i += 2;
        } else {
            if (!unreserved(uri[i]) && uri[i] != '/' && uri[i] != ':')
                return std::nullopt;
            decoded.push_back(uri[i]);
        }
    }
    if (!isValidUtf8(decoded))
        return std::nullopt;
    if (!authority.empty() && !localHost(authority)) {
#if defined(_WIN32)
        decoded = "//" + std::string(authority) + decoded;
#else
        // A remote authority is a UNC path on Windows, not a local POSIX pathname.
        return std::nullopt;
#endif
    }
#if defined(_WIN32)
    if (decoded.size() >= 3 && decoded[0] == '/' && alpha(decoded[1]) && decoded[2] == ':')
        decoded.erase(0, 1);
#else
    if (decoded.size() >= 3 && decoded[0] == '/' && alpha(decoded[1]) && decoded[2] == ':')
        return std::nullopt;
#endif
    const auto path = std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(decoded.data()), decoded.size()));
    if (!path.is_absolute())
        return std::nullopt;
    return path;
}

} // namespace bloom::core
