// MCP Streamable HTTP framing and admission (ADR 0034, section 3): pure functions over bytes.
#include <bloom/mcp/streamable_http.hpp>

#include <algorithm>
#include <array>
#include <charconv>

namespace bloom::mcp {
namespace {

constexpr std::string_view kCrlf = "\r\n";
// A chunk-size line (hex digits plus bounded extensions).
constexpr std::size_t kChunkLineLimit = 1024;

[[nodiscard]] constexpr char lower(const char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), lower);
    return out;
}

[[nodiscard]] bool equalsIgnoringCase(const std::string_view a, const std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](const char x, const char y) { return lower(x) == lower(y); });
}

// RFC 9110 tchar.
[[nodiscard]] constexpr bool tokenCharacter(const char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    constexpr std::string_view extra = "!#$%&'*+-.^_`|~";
    return extra.find(c) != std::string_view::npos;
}

[[nodiscard]] bool token(const std::string_view text) noexcept {
    return !text.empty() && std::ranges::all_of(text, tokenCharacter);
}

// Field values: visible ASCII, obs-text and horizontal tab or space; never NUL, CR, LF or another
// control character.
[[nodiscard]] bool fieldValue(const std::string_view text) noexcept {
    return std::ranges::all_of(text, [](const char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte == '\t' || (byte >= 0x20U && byte != 0x7fU);
    });
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] McpHttpMethod methodOf(const std::string_view method) noexcept {
    if (method == "GET")
        return McpHttpMethod::Get;
    if (method == "POST")
        return McpHttpMethod::Post;
    if (method == "DELETE")
        return McpHttpMethod::Delete;
    if (method == "OPTIONS")
        return McpHttpMethod::Options;
    return McpHttpMethod::Other;
}

// How many fields named `name` the request carries.
[[nodiscard]] std::size_t fieldCount(const McpHttpRequest& request,
                                     const std::string_view name) noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        request.headers, [&](const auto& field) { return field.first == name; }));
}

// The single field named `name`: nullopt when absent; `duplicate` when it appears more than once.
struct SingleField final {
    std::optional<std::string_view> value;
    bool duplicate = false;
};
[[nodiscard]] SingleField singleField(const McpHttpRequest& request, const std::string_view name) {
    return {request.header(name), fieldCount(request, name) > 1};
}

// The media type of a Content-Type or Accept element, lower-cased, without parameters.
[[nodiscard]] std::string mediaType(const std::string_view element) {
    return lowered(trim(element.substr(0, element.find(';'))));
}

// True when an Accept element's q parameter is zero (explicitly not acceptable).
[[nodiscard]] bool qualityZero(const std::string_view element) {
    auto parameters = element;
    while (true) {
        const auto separator = parameters.find(';');
        if (separator == std::string_view::npos)
            return false;
        parameters.remove_prefix(separator + 1);
        const auto parameter = trim(parameters.substr(0, parameters.find(';')));
        if (parameter.size() >= 2 && lower(parameter[0]) == 'q' && parameter[1] == '=') {
            const auto value = parameter.substr(2);
            return !value.empty() &&
                   std::ranges::all_of(value, [](const char c) { return c == '0' || c == '.'; });
        }
    }
}

[[nodiscard]] bool accepts(const std::string_view accept, const std::string_view wanted) {
    const auto slash = wanted.find('/');
    const auto family = std::string(wanted.substr(0, slash)) + "/*";
    std::string_view rest = accept;
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const auto element = rest.substr(0, comma);
        const auto type = mediaType(element);
        if ((type == wanted || type == family || type == "*/*") && !qualityZero(element))
            return true;
        if (comma == std::string_view::npos)
            break;
        rest.remove_prefix(comma + 1);
    }
    return false;
}

} // namespace

std::optional<std::string_view> McpHttpRequest::header(const std::string_view name) const {
    for (auto it = headers.rbegin(); it != headers.rend(); ++it) {
        if (it->first == name)
            return std::string_view(it->second);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// Parser

McpHttpRequestParser::McpHttpRequestParser(const McpHttpLimits limits, const std::size_t bodyLimit)
    : limits_(limits), bodyLimit_(bodyLimit) {}

McpHttpParseStatus McpHttpRequestParser::fail(const McpHttpParseStatus status) {
    phase_ = Phase::Failed;
    failure_ = status;
    return status;
}

bool McpHttpRequestParser::idle() const noexcept {
    return phase_ == Phase::Head && buffer_.empty();
}

McpHttpParseStatus McpHttpRequestParser::feed(const std::span<const char> bytes) {
    if (phase_ == Phase::Failed)
        return failure_;
    buffer_.append(bytes.data(), bytes.size());
    if (phase_ == Phase::Done)
        return McpHttpParseStatus::Complete;
    return advance();
}

McpHttpRequest McpHttpRequestParser::take() {
    McpHttpRequest taken;
    if (phase_ != Phase::Done)
        return taken;
    taken = std::move(request_);
    request_ = {};
    buffer_.erase(0, consumed_);
    consumed_ = 0;
    scanned_ = 0;
    remaining_ = 0;
    trailerBytes_ = 0;
    chunkStep_ = ChunkStep::Size;
    phase_ = Phase::Head;
    return taken;
}

McpHttpParseStatus McpHttpRequestParser::advance() {
    if (phase_ == Phase::Head) {
        // RFC 9112 section 2.2: ignore empty lines before the request line.
        while (buffer_.size() >= consumed_ + 2 && buffer_.compare(consumed_, 2, kCrlf) == 0) {
            consumed_ += 2;
            scanned_ = consumed_;
            if (consumed_ > limits_.headerBytes)
                return fail(McpHttpParseStatus::HeaderTooLarge);
        }
        const auto from = std::max(consumed_, scanned_ >= 3 ? scanned_ - 3 : std::size_t{0});
        const auto end = buffer_.find("\r\n\r\n", from);
        if (end == std::string::npos) {
            scanned_ = buffer_.size();
            if (buffer_.size() - consumed_ > limits_.headerBytes)
                return fail(McpHttpParseStatus::HeaderTooLarge);
            return McpHttpParseStatus::NeedMore;
        }
        if (end + 4 - consumed_ > limits_.headerBytes)
            return fail(McpHttpParseStatus::HeaderTooLarge);
        const auto head = std::string_view(buffer_).substr(consumed_, end - consumed_);
        consumed_ = end + 4;
        if (const auto status = parseHead(head); status != McpHttpParseStatus::Complete)
            return status;
    }
    if (phase_ == Phase::Body) {
        if (buffer_.size() - consumed_ < remaining_)
            return McpHttpParseStatus::NeedMore;
        request_.body.assign(buffer_, consumed_, remaining_);
        consumed_ += remaining_;
        remaining_ = 0;
        phase_ = Phase::Done;
        return McpHttpParseStatus::Complete;
    }
    if (phase_ == Phase::Chunked)
        return advanceChunked();
    return phase_ == Phase::Done ? McpHttpParseStatus::Complete : failure_;
}

McpHttpParseStatus McpHttpRequestParser::parseHead(const std::string_view head) {
    // Line endings are CRLF only: a bare CR or LF anywhere in the head is refused, so no two
    // parsers can disagree about where a field ends.
    std::vector<std::string_view> lines;
    std::string_view rest = head;
    while (true) {
        const auto end = rest.find(kCrlf);
        const auto line = rest.substr(0, end);
        if (line.find('\r') != std::string_view::npos || line.find('\n') != std::string_view::npos)
            return fail(McpHttpParseStatus::Malformed);
        lines.push_back(line);
        if (end == std::string_view::npos)
            break;
        rest.remove_prefix(end + 2);
    }
    // Request line: method SP request-target SP HTTP-version, single spaces.
    const auto& requestLine = lines.front();
    const auto firstSpace = requestLine.find(' ');
    const auto lastSpace = requestLine.rfind(' ');
    if (firstSpace == std::string_view::npos || firstSpace == lastSpace)
        return fail(McpHttpParseStatus::Malformed);
    const auto method = requestLine.substr(0, firstSpace);
    const auto target = requestLine.substr(firstSpace + 1, lastSpace - firstSpace - 1);
    const auto version = requestLine.substr(lastSpace + 1);
    if (!token(method) || target.empty() || target.find(' ') != std::string_view::npos ||
        !fieldValue(target))
        return fail(McpHttpParseStatus::Malformed);
    if (version != "HTTP/1.1") {
        const bool httpVersion = version.size() == 8 && version.starts_with("HTTP/") &&
                                 version[5] >= '0' && version[5] <= '9' && version[6] == '.' &&
                                 version[7] >= '0' && version[7] <= '9';
        return fail(httpVersion || version == "HTTP/2" ? McpHttpParseStatus::UnsupportedVersion
                                                       : McpHttpParseStatus::Malformed);
    }
    if (target.front() != '/')
        return fail(McpHttpParseStatus::Malformed);
    request_.method = methodOf(method);
    request_.path = std::string(target);
    if (lines.size() - 1 > limits_.headerCount)
        return fail(McpHttpParseStatus::TooManyHeaders);
    for (std::size_t index = 1; index < lines.size(); ++index) {
        const auto line = lines[index];
        const auto colon = line.find(':');
        // No obsolete line folding, no whitespace before the colon, a token name.
        if (colon == std::string_view::npos || !token(line.substr(0, colon)))
            return fail(McpHttpParseStatus::Malformed);
        const auto value = trim(line.substr(colon + 1));
        if (!fieldValue(value))
            return fail(McpHttpParseStatus::Malformed);
        request_.headers.emplace_back(lowered(line.substr(0, colon)), std::string(value));
    }
    // Message framing (RFC 9112 section 6).
    const auto encodings = fieldCount(request_, "transfer-encoding");
    const auto lengths = fieldCount(request_, "content-length");
    if (encodings > 0 && lengths > 0)
        return fail(McpHttpParseStatus::AmbiguousLength);
    if (encodings > 0) {
        if (encodings > 1 || !equalsIgnoringCase(*request_.header("transfer-encoding"), "chunked"))
            return fail(McpHttpParseStatus::UnsupportedTransferEncoding);
        phase_ = Phase::Chunked;
        chunkStep_ = ChunkStep::Size;
        return advanceChunked();
    }
    if (lengths > 0) {
        std::optional<std::string_view> agreed;
        for (const auto& [name, value] : request_.headers) {
            if (name != "content-length")
                continue;
            if (agreed && *agreed != value)
                return fail(McpHttpParseStatus::AmbiguousLength);
            agreed = value;
        }
        const auto text = *agreed;
        if (text.empty() ||
            !std::ranges::all_of(text, [](const char c) { return c >= '0' && c <= '9'; }))
            return fail(McpHttpParseStatus::Malformed);
        std::uint64_t length = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), length);
        if (parsed.ec == std::errc::result_out_of_range || length > bodyLimit_)
            return fail(McpHttpParseStatus::BodyTooLarge);
        if (parsed.ec != std::errc{})
            return fail(McpHttpParseStatus::Malformed);
        remaining_ = static_cast<std::size_t>(length);
        phase_ = Phase::Body;
        return McpHttpParseStatus::Complete;
    }
    if (request_.method == McpHttpMethod::Post)
        return fail(McpHttpParseStatus::LengthRequired);
    phase_ = Phase::Body;
    remaining_ = 0;
    return McpHttpParseStatus::Complete;
}

McpHttpParseStatus McpHttpRequestParser::advanceChunked() {
    while (true) {
        const auto available = std::string_view(buffer_).substr(consumed_);
        switch (chunkStep_) {
        case ChunkStep::Size: {
            const auto end = available.find(kCrlf);
            if (end == std::string_view::npos) {
                if (available.size() > kChunkLineLimit)
                    return fail(McpHttpParseStatus::Malformed);
                return McpHttpParseStatus::NeedMore;
            }
            if (end > kChunkLineLimit)
                return fail(McpHttpParseStatus::Malformed);
            const auto line = available.substr(0, end);
            if (line.find('\r') != std::string_view::npos ||
                line.find('\n') != std::string_view::npos || !fieldValue(line))
                return fail(McpHttpParseStatus::Malformed);
            // chunk-size [ chunk-ext ]: hex digits, then nothing, whitespace or ';'.
            const auto digits = line.substr(0, line.find_first_not_of("0123456789abcdefABCDEF"));
            const auto after = line.substr(digits.size());
            if (digits.empty() || digits.size() > 16 ||
                (!after.empty() && after.front() != ';' && after.front() != ' ' &&
                 after.front() != '\t'))
                return fail(McpHttpParseStatus::Malformed);
            std::uint64_t size = 0;
            if (std::from_chars(digits.data(), digits.data() + digits.size(), size, 16).ec !=
                std::errc{})
                return fail(McpHttpParseStatus::Malformed);
            if (size > bodyLimit_ || request_.body.size() + size > bodyLimit_)
                return fail(McpHttpParseStatus::BodyTooLarge);
            consumed_ += end + 2;
            remaining_ = static_cast<std::size_t>(size);
            chunkStep_ = size == 0 ? ChunkStep::Trailer : ChunkStep::Data;
            break;
        }
        case ChunkStep::Data: {
            const auto take = std::min(remaining_, available.size());
            request_.body.append(available.substr(0, take));
            consumed_ += take;
            remaining_ -= take;
            if (remaining_ > 0)
                return McpHttpParseStatus::NeedMore;
            chunkStep_ = ChunkStep::DataEnd;
            break;
        }
        case ChunkStep::DataEnd:
            if (available.size() < 2)
                return McpHttpParseStatus::NeedMore;
            if (!available.starts_with(kCrlf))
                return fail(McpHttpParseStatus::Malformed);
            consumed_ += 2;
            chunkStep_ = ChunkStep::Size;
            break;
        case ChunkStep::Trailer: {
            // Trailer fields are read and discarded under the header bounds; none is honoured.
            const auto end = available.find(kCrlf);
            if (end == std::string_view::npos) {
                if (trailerBytes_ + available.size() > limits_.headerBytes)
                    return fail(McpHttpParseStatus::HeaderTooLarge);
                return McpHttpParseStatus::NeedMore;
            }
            trailerBytes_ += end + 2;
            if (trailerBytes_ > limits_.headerBytes)
                return fail(McpHttpParseStatus::HeaderTooLarge);
            const auto line = available.substr(0, end);
            consumed_ += end + 2;
            if (line.empty()) {
                phase_ = Phase::Done;
                return McpHttpParseStatus::Complete;
            }
            const auto colon = line.find(':');
            if (colon == std::string_view::npos || !token(line.substr(0, colon)) ||
                line.find('\r') != std::string_view::npos ||
                line.find('\n') != std::string_view::npos || !fieldValue(line.substr(colon + 1)))
                return fail(McpHttpParseStatus::Malformed);
            break;
        }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Admission

bool hostAllowed(const std::string_view host, const std::uint16_t port) noexcept {
    const auto colon = host.rfind(':');
    if (colon == std::string_view::npos)
        return false;
    const auto name = host.substr(0, colon);
    const auto digits = host.substr(colon + 1);
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (digits.empty() || parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() ||
        digits.front() == '0' || value != port)
        return false;
    return name == "127.0.0.1" || equalsIgnoringCase(name, "localhost");
}

bool originAllowed(const std::optional<std::string_view> origin,
                   const McpHttpAdmissionPolicy& policy) noexcept {
    if (!origin)
        return true;
    const auto value = *origin;
    if (value.empty() || equalsIgnoringCase(value, "null"))
        return false;
    // Lower-case the scheme and host (everything before the first '/' after "://").
    const auto scheme = value.find("://");
    if (scheme == std::string_view::npos)
        return false;
    try {
        const auto normalized = lowered(value);
        return std::ranges::any_of(policy.allowedOrigins, [&](const std::string& allowed) {
            return !equalsIgnoringCase(allowed, "null") && lowered(allowed) == normalized;
        });
    } catch (...) {
        return false;
    }
}

bool bearerMatches(const std::optional<std::string_view> authorization,
                   const std::string_view token) noexcept {
    if (!authorization || token.empty())
        return false;
    auto value = *authorization;
    constexpr std::string_view scheme = "bearer";
    if (value.size() <= scheme.size() || !equalsIgnoringCase(value.substr(0, 6), scheme) ||
        value[6] != ' ')
        return false;
    value.remove_prefix(7);
    while (!value.empty() && value.front() == ' ')
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    // Constant time in the token's contents: every byte of the expected token is compared, and a
    // length mismatch (the length is public) still walks the whole token.
    unsigned difference = value.size() == token.size() ? 0U : 1U;
    for (std::size_t index = 0; index < token.size(); ++index) {
        const auto presented = index < value.size() ? static_cast<unsigned char>(value[index])
                                                    : static_cast<unsigned char>(0);
        difference |= static_cast<unsigned>(presented ^ static_cast<unsigned char>(token[index]));
    }
    return difference == 0U;
}

McpHttpRejection admitRequest(const McpHttpRequest& request, const McpHttpAdmissionPolicy& policy,
                              const std::string_view token) noexcept {
    try {
        const auto host = singleField(request, "host");
        if (!host.value || host.duplicate || !hostAllowed(*host.value, policy.port))
            return McpHttpRejection::HostNotAllowed;
        const auto origin = singleField(request, "origin");
        if (origin.duplicate || !originAllowed(origin.value, policy))
            return McpHttpRejection::OriginNotAllowed;
        if (request.path != kMcpEndpointPath)
            return McpHttpRejection::NotFound;
        const auto authorization = singleField(request, "authorization");
        if (authorization.duplicate || !bearerMatches(authorization.value, token))
            return McpHttpRejection::Unauthorized;
        if (request.method != McpHttpMethod::Post && request.method != McpHttpMethod::Delete)
            return McpHttpRejection::MethodNotAllowed;
        if (request.method == McpHttpMethod::Post) {
            const auto type = singleField(request, "content-type");
            if (!type.value || type.duplicate || mediaType(*type.value) != "application/json")
                return McpHttpRejection::UnsupportedMediaType;
            const auto accept = singleField(request, "accept");
            if (!accept.value || accept.duplicate || !accepts(*accept.value, "application/json") ||
                !accepts(*accept.value, "text/event-stream"))
                return McpHttpRejection::NotAcceptable;
            const auto first = request.body.find_first_not_of(" \t\r\n");
            if (first != std::string::npos && request.body[first] == '[')
                return McpHttpRejection::BatchNotSupported;
        }
        return McpHttpRejection::None;
    } catch (...) {
        return McpHttpRejection::Busy;
    }
}

// ---------------------------------------------------------------------------------------------
// Responses

std::string_view httpReasonPhrase(const int status) noexcept {
    switch (status) {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 406:
        return "Not Acceptable";
    case 408:
        return "Request Timeout";
    case 411:
        return "Length Required";
    case 413:
        return "Content Too Large";
    case 415:
        return "Unsupported Media Type";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Error";
    }
}

std::string httpResponse(const int status,
                         const std::span<const std::pair<std::string_view, std::string>> headers,
                         const std::optional<std::string_view> body) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " ";
    out += httpReasonPhrase(status);
    out += kCrlf;
    for (const auto& [name, value] : headers) {
        out += name;
        out += ": ";
        out += value;
        out += kCrlf;
    }
    if (body) {
        out += "Content-Length: " + std::to_string(body->size());
        out += kCrlf;
    }
    out += kCrlf;
    if (body)
        out += *body;
    return out;
}

std::string sseMessage(const std::string_view json) {
    std::string out = "event: message\r\ndata: ";
    out += json;
    out += "\r\n\r\n";
    return out;
}

std::string base64Url(const std::span<const std::byte> bytes) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((bytes.size() * 4 + 2) / 3);
    std::size_t index = 0;
    for (; index + 3 <= bytes.size(); index += 3) {
        const auto value = (std::to_integer<unsigned>(bytes[index]) << 16U) |
                           (std::to_integer<unsigned>(bytes[index + 1]) << 8U) |
                           std::to_integer<unsigned>(bytes[index + 2]);
        out.push_back(alphabet[(value >> 18U) & 63U]);
        out.push_back(alphabet[(value >> 12U) & 63U]);
        out.push_back(alphabet[(value >> 6U) & 63U]);
        out.push_back(alphabet[value & 63U]);
    }
    const auto left = bytes.size() - index;
    if (left > 0) {
        auto value = std::to_integer<unsigned>(bytes[index]) << 16U;
        if (left == 2)
            value |= std::to_integer<unsigned>(bytes[index + 1]) << 8U;
        out.push_back(alphabet[(value >> 18U) & 63U]);
        out.push_back(alphabet[(value >> 12U) & 63U]);
        if (left == 2)
            out.push_back(alphabet[(value >> 6U) & 63U]);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Tokens

std::string maskToken(const std::string_view token) {
    // Ten bullets (U+2022) between the prefix and the last four characters.
    std::string bullets;
    for (int index = 0; index < 10; ++index)
        bullets += "\xe2\x80\xa2";
    if (!tokenWellFormed(token))
        return bullets;
    return std::string(kMcpTokenPrefix) + bullets + std::string(token.substr(token.size() - 4));
}

bool tokenWellFormed(const std::string_view token) noexcept {
    if (token.size() != kMcpTokenLength || !token.starts_with(kMcpTokenPrefix))
        return false;
    return std::ranges::all_of(token.substr(kMcpTokenPrefix.size()), [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
    });
}

} // namespace bloom::mcp
