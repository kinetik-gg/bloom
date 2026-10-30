#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// MCP Streamable HTTP (2025-11-25) for the in-app server (ADR 0034, section 3). Qt-free; owned by
// src/mcp. Pure framing and admission: no sockets (src/platform owns those), no clock, no session
// table. Everything here is deterministic and unit-testable with byte strings.
//
// Endpoint: one path, POST and DELETE only.
// - POST /mcp carries exactly one JSON-RPC message. A request is answered with application/json,
//   or with text/event-stream when it is a Task tool call that carried a progress token (progress
//   notifications, then the response, then the stream closes). A notification or a client response
//   is answered 202 Accepted with no body.
// - DELETE /mcp ends the caller's MCP session (204).
// - GET /mcp is 405: Bloom sends no server-initiated messages, so it offers no standalone stream.
// - Everything else is 404 (including /.well-known/* OAuth discovery: Bloom uses a static bearer
//   token, not MCP OAuth).
//
// HTTP/1.1 only. Request bodies use Content-Length or chunked transfer coding (not both; never any
// other coding); JSON responses use Content-Length and keep the connection alive; event streams are
// close-delimited. No compression, no ranges, no upgrades, no CORS headers ever.
namespace bloom::mcp {

inline constexpr std::string_view kMcpEndpointPath = "/mcp";
inline constexpr std::uint16_t kMcpDefaultPort = 6767;
// Ports below 1024 are privileged on Linux and macOS; Bloom never asks for them.
inline constexpr std::uint16_t kMcpMinimumPort = 1024;
inline constexpr std::uint16_t kMcpMaximumPort = 65535;

[[nodiscard]] constexpr bool portInRange(const std::uint32_t port) noexcept {
    return port >= kMcpMinimumPort && port <= kMcpMaximumPort;
}

// HTTP-level bounds. The body bound is McpMessageLimits::requestBytes (1 MiB), unchanged.
struct McpHttpLimits final {
    // Request line plus headers.
    std::size_t headerBytes = std::size_t{16} * 1024U;
    std::size_t headerCount = 64;
    // Slow-client bounds: the whole header block, then the whole body, must arrive in time.
    std::chrono::milliseconds headerTimeout{10000};
    std::chrono::milliseconds bodyTimeout{30000};
    // An idle keep-alive connection is closed after this.
    std::chrono::milliseconds keepAliveIdle{60000};
    // A client that stops reading a response is disconnected after this.
    std::chrono::milliseconds writeTimeout{30000};
};

enum class McpHttpMethod : std::uint8_t { Get, Post, Delete, Options, Other };

struct McpHttpRequest final {
    McpHttpMethod method = McpHttpMethod::Other;
    // The origin-form target, without query (a query string is a 404).
    std::string path;
    // Field names lower-cased; values trimmed; order kept; duplicates kept (admission rejects a
    // duplicate of any field it reads).
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    // The last header field named `name` (lower-case), if any.
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
};

enum class McpHttpParseStatus : std::uint8_t {
    Complete = 0,
    NeedMore = 1,
    // 431, then close.
    HeaderTooLarge = 2,
    TooManyHeaders = 3,
    // 413, then close.
    BodyTooLarge = 4,
    // 400, then close.
    Malformed = 5,
    // Not HTTP/1.1 (HTTP/1.0 and HTTP/2 prior knowledge are refused with 505).
    UnsupportedVersion = 6,
    // Transfer-Encoding other than exactly "chunked".
    UnsupportedTransferEncoding = 7,
    // Both Content-Length and Transfer-Encoding, or conflicting Content-Length values.
    AmbiguousLength = 8,
    // A POST with neither Content-Length nor chunked coding (411).
    LengthRequired = 9,
};

// Incremental request parser for one connection. Feed bytes as they arrive; after Complete, take
// the request and the parser is ready for the next one (pipelined bytes are kept). Any failure is
// terminal for the connection: the server answers it and closes.
class McpHttpRequestParser final {
  public:
    McpHttpRequestParser(McpHttpLimits limits, std::size_t bodyLimit);
    [[nodiscard]] McpHttpParseStatus feed(std::span<const char> bytes);
    [[nodiscard]] McpHttpRequest take();
    [[nodiscard]] bool idle() const noexcept;

  private:
    McpHttpLimits limits_;
    std::size_t bodyLimit_ = 0;
    std::string buffer_;
    std::size_t state_ = 0;
};

// Why a request is refused before it reaches the protocol core. Admission runs in this order, so a
// browser page learns nothing past the first check it fails.
enum class McpHttpRejection : std::uint8_t {
    None = 0,
    // Host is not exactly 127.0.0.1:<port> or localhost:<port> (DNS-rebinding defence, 403).
    HostNotAllowed = 1,
    // An Origin header is present and not on the allow-list (403). Non-browser clients send none.
    OriginNotAllowed = 2,
    // Path is not kMcpEndpointPath (404).
    NotFound = 3,
    // Authorization is missing, malformed or not the current token (401, WWW-Authenticate: Bearer
    // realm="bloom"). Compared in constant time.
    Unauthorized = 4,
    // GET, OPTIONS or anything other than POST/DELETE (405, Allow: POST, DELETE).
    MethodNotAllowed = 5,
    // POST without Content-Type application/json (415).
    UnsupportedMediaType = 6,
    // POST whose Accept does not permit both application/json and text/event-stream (406).
    NotAcceptable = 7,
    // A non-initialize POST, or a DELETE, without Mcp-Session-Id (400).
    MissingSession = 8,
    // Mcp-Session-Id is unknown, ended or expired (404: the client starts a new session).
    UnknownSession = 9,
    // MCP-Protocol-Version is present and is not the negotiated version (400).
    UnsupportedProtocolVersion = 10,
    // A JSON array body (batches were removed in 2025-11-25) (400).
    BatchNotSupported = 11,
    // Body larger than requestBytes (413).
    PayloadTooLarge = 12,
    // Connection, client or pending-message limit reached (503, Retry-After: 1).
    Busy = 13,
    // The server is stopping (503, Connection: close).
    Stopping = 14,
};

[[nodiscard]] constexpr int httpStatus(const McpHttpRejection rejection) noexcept {
    switch (rejection) {
    case McpHttpRejection::None:
        return 200;
    case McpHttpRejection::HostNotAllowed:
    case McpHttpRejection::OriginNotAllowed:
        return 403;
    case McpHttpRejection::NotFound:
    case McpHttpRejection::UnknownSession:
        return 404;
    case McpHttpRejection::Unauthorized:
        return 401;
    case McpHttpRejection::MethodNotAllowed:
        return 405;
    case McpHttpRejection::UnsupportedMediaType:
        return 415;
    case McpHttpRejection::NotAcceptable:
        return 406;
    case McpHttpRejection::MissingSession:
    case McpHttpRejection::UnsupportedProtocolVersion:
    case McpHttpRejection::BatchNotSupported:
        return 400;
    case McpHttpRejection::PayloadTooLarge:
        return 413;
    case McpHttpRejection::Busy:
    case McpHttpRejection::Stopping:
        return 503;
    }
    return 500;
}

// The facts admission checks against. The token is borrowed for the call only.
struct McpHttpAdmissionPolicy final {
    std::uint16_t port = kMcpDefaultPort;
    // Exact origins (scheme://host[:port]) a browser-based client may use. Empty by default: Bloom
    // serves no web page, so no browser origin is trusted. "null" is never accepted.
    std::vector<std::string> allowedOrigins;
};

// Host header check: exactly "127.0.0.1:<port>" or "localhost:<port>" (case-insensitive host),
// with the listener's port. Pure.
[[nodiscard]] bool hostAllowed(std::string_view host, std::uint16_t port) noexcept;

// Origin check: an absent Origin is allowed; a present one must equal an allow-list entry
// byte-for-byte after lower-casing scheme and host. Pure.
[[nodiscard]] bool originAllowed(std::optional<std::string_view> origin,
                                 const McpHttpAdmissionPolicy& policy) noexcept;

// "Authorization: Bearer <token>" against `token` in constant time with respect to the token's
// contents (the length is public). Pure.
[[nodiscard]] bool bearerMatches(std::optional<std::string_view> authorization,
                                 std::string_view token) noexcept;

// Runs the transport-independent checks (Host, Origin, path, Authorization, method, Content-Type,
// Accept, batch) in order. Session and protocol-version checks need the session table and run in
// McpServerService after this. Pure.
[[nodiscard]] McpHttpRejection admitRequest(const McpHttpRequest& request,
                                            const McpHttpAdmissionPolicy& policy,
                                            std::string_view token) noexcept;

// ---------------------------------------------------------------------------------------------
// Sessions and tokens

// Mcp-Session-Id: 128 bits from the platform CSPRNG as 32 lower-case hex digits (visible ASCII, as
// the specification requires). It identifies a protocol session; it is not a credential (the bearer
// token is), and it is never shown in the panel, the activity log or a diagnostic.
inline constexpr std::size_t kMcpSessionIdBytes = 16;

// Bearer token: "bloom_" followed by 256 bits from the platform CSPRNG in unpadded base64url
// (43 characters). Stored owner-only (platform::writeOwnerOnlySecret).
inline constexpr std::string_view kMcpTokenPrefix = "bloom_";
inline constexpr std::size_t kMcpTokenRandomBytes = 32;
inline constexpr std::size_t kMcpTokenLength = 6 + 43;

// The masked form the snapshot carries: the prefix, a fixed run of bullets and the last four
// characters ("bloom_••••••••••3fQa"). Pure; never returns more of the token.
[[nodiscard]] std::string maskToken(std::string_view token);

// A syntactically valid token (prefix, length, base64url alphabet). Pure.
[[nodiscard]] bool tokenWellFormed(std::string_view token) noexcept;

} // namespace bloom::mcp
